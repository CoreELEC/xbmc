/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoDecodeSession.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "DVDDemuxers/DVDFactoryDemuxer.h"
#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "FileItem.h"
#include "Process/ProcessInfo.h"
#include "ServiceBroker.h"
#include "messaging/ApplicationMessenger.h"
#include "threads/Condition.h"
#include "threads/CriticalSection.h"
#include "utils/MemUtils.h"
#include "utils/log.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace
{
// a job holds a 64MB read cache for network files and, for 4K, about 90MB of decoded pictures;
// the reserve is left for playback and the GUI
constexpr uint64_t DECODE_JOB_RESERVE = 100 * 1024 * 1024;
constexpr uint64_t DECODE_JOB_BUDGET = 150 * 1024 * 1024;
constexpr auto DECODE_JOB_RECHECK = std::chrono::milliseconds(500);

CCriticalSection decodeJobSection;
XbmcThreads::ConditionVariable decodeJobCondition;
int decodeJobs = 0;
} // namespace

CVideoDecodeJob::CVideoDecodeJob(bool wait)
{
  std::unique_lock lock(decodeJobSection);

  const auto messenger = CServiceBroker::GetAppMessenger();
  if (wait && (!messenger || !messenger->IsProcessThread()))
  {
    KODI::MEMORY::MemoryStatus status{};
    while (decodeJobs > 0)
    {
      // free CMA is not counted, as Linux kernels can keep tasks with a positive nice value out
      // of it and the job threads run below normal priority
      KODI::MEMORY::GetMemoryStatus(&status);
      const uint64_t available =
          status.availPhys > status.availCma ? status.availPhys - status.availCma : 0;
      if (available >= DECODE_JOB_RESERVE + DECODE_JOB_BUDGET * (decodeJobs + 1))
        break;

      decodeJobCondition.wait(lock, DECODE_JOB_RECHECK);
    }
  }
  decodeJobs++;
  m_admitted = true;
}

CVideoDecodeJob::~CVideoDecodeJob()
{
  Release();
}

CVideoDecodeJob::CVideoDecodeJob(CVideoDecodeJob&& other) noexcept
  : m_admitted(std::exchange(other.m_admitted, false))
{
}

CVideoDecodeJob& CVideoDecodeJob::operator=(CVideoDecodeJob&& other) noexcept
{
  if (this != &other)
  {
    Release();
    m_admitted = std::exchange(other.m_admitted, false);
  }
  return *this;
}

void CVideoDecodeJob::Release()
{
  if (!m_admitted)
    return;

  std::unique_lock lock(decodeJobSection);
  decodeJobs--;
  m_admitted = false;
  decodeJobCondition.notifyAll();
}

std::optional<VideoDecodeSession> OpenVideoDecodeSession(const CFileItem& fileItem,
                                                         int codecOptions,
                                                         const std::string& redactPath)
{
  CFileItem item(fileItem);
  item.SetMimeTypeForInternetFile();

  VideoDecodeSession session;
  session.inputStream = CDVDFactoryInputStream::CreateInputStream(nullptr, item);
  if (!session.inputStream || !session.inputStream->Open())
  {
    CLog::Log(LOGERROR, "VideoDecodeSession: error opening input stream for {}", redactPath);
    return std::nullopt;
  }

  session.demuxer.reset(CDVDFactoryDemuxer::CreateDemuxer(session.inputStream, true));
  if (!session.demuxer)
  {
    CLog::Log(LOGERROR, "VideoDecodeSession: error creating demuxer for {}", redactPath);
    return std::nullopt;
  }

  for (CDemuxStream* stream : session.demuxer->GetStreams())
  {
    if (!stream)
      continue;

    // The first video stream, which is the base layer in Dolby Vision dual-track files.
    if (stream->type == StreamType::VIDEO && !(stream->flags & AV_DISPOSITION_ATTACHED_PIC) &&
        session.videoStream == -1)
    {
      session.videoStream = stream->uniqueId;
      session.demuxerId = stream->demuxerId;
    }
    else
      session.demuxer->EnableStream(stream->demuxerId, stream->uniqueId, false);
  }

  if (session.videoStream == -1)
    return std::nullopt;

  session.processInfo.reset(CProcessInfo::CreateInstance());
  std::vector<AVPixelFormat> pixFmts{AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10,
                                     AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV422P10,
                                     AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10};
  session.processInfo->SetPixFormats(pixFmts);

  session.hint.Assign(*session.demuxer->GetStream(session.demuxerId, session.videoStream), true);
  session.hint.codecOptions = codecOptions;

  session.codec = CDVDFactoryCodec::CreateVideoCodec(session.hint, *session.processInfo);
  if (!session.codec)
  {
    CLog::Log(LOGERROR, "VideoDecodeSession: error creating video codec for {}", redactPath);
    return std::nullopt;
  }

  return session;
}

bool SeekAndDecodePictureAt(CDVDDemux& demuxer,
                            CDVDVideoCodec& codec,
                            int videoStream,
                            int64_t seekToMs,
                            const std::string& redactPath,
                            VideoPicture& picture,
                            int& packetsTried,
                            const std::function<bool()>& abort)
{
  CLog::LogF(LOGDEBUG, "seeking to pos {}ms (total: {}ms) in {}", seekToMs,
             demuxer.GetStreamLength(), redactPath);

  if (!demuxer.SeekTime(static_cast<double>(seekToMs), true))
    return false;

  CDVDVideoCodec::VCReturn iDecoderState = CDVDVideoCodec::VC_NONE;

  for (int attemptsLeft = demuxer.GetNrOfStreams() * 160; attemptsLeft >= 0; attemptsLeft--)
  {
    if (abort && abort())
      return false;

    DemuxPacket* pPacket = demuxer.Read();
    packetsTried++;

    if (!pPacket)
      break;

    if (pPacket->iStreamId != videoStream)
    {
      CDVDDemuxUtils::FreeDemuxPacket(pPacket);
      continue;
    }

    codec.AddData(*pPacket);
    CDVDDemuxUtils::FreeDemuxPacket(pPacket);

    // A filter holding a frame back answers VC_NONE, so the codec is not asked for ever.
    iDecoderState = CDVDVideoCodec::VC_NONE;
    for (int asks = 50; asks > 0 && iDecoderState == CDVDVideoCodec::VC_NONE; --asks)
      iDecoderState = codec.GetPicture(&picture);

    if (iDecoderState == CDVDVideoCodec::VC_PICTURE)
    {
      if (!(picture.iFlags & DVP_FLAG_DROPPED))
        break;
    }
  }

  return iDecoderState == CDVDVideoCodec::VC_PICTURE && !(picture.iFlags & DVP_FLAG_DROPPED);
}
