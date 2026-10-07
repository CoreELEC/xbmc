/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DemuxDualLayer.h"

#include "DVDDemux.h"
#include "DVDDemuxUtils.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "utils/MemUtils.h"
#include "utils/log.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>

namespace
{
// not a demuxer stream id and not a DMX_SPECIALID value; CDVDDemuxFFmpeg::Read() drops holds
constexpr int HOLD_STREAM_ID = -1000;

bool HasEnhancementLayer(const std::map<int, CDemuxStream*>& streams)
{
  return std::ranges::any_of(streams,
                             [](const auto& entry)
                             {
                               const CDemuxStream* stream = entry.second;
                               return stream->type == StreamType::VIDEO &&
                                      static_cast<const CDemuxStreamVideo*>(stream)->isELStream &&
                                      static_cast<const AVStream*>(stream->pPrivate)->discard !=
                                          AVDISCARD_ALL;
                             });
}

// copies `packet` and `tail` into one padded buffer owned by `packet`, `gap` zero bytes apart
bool Join(DemuxPacket& packet, const DemuxPacket& tail, int gap)
{
  const int size = packet.iSize + gap + tail.iSize;
  auto* data =
      static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(size + AV_INPUT_BUFFER_PADDING_SIZE, 16));
  if (!data)
    return false;

  std::memcpy(data, packet.pData, packet.iSize);
  std::memset(data + packet.iSize, 0, gap);
  std::memcpy(data + packet.iSize + gap, tail.pData, tail.iSize);
  std::memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  KODI::MEMORY::AlignedFree(packet.pData);
  packet.pData = data;
  return true;
}

double Seconds(int64_t key, AVRational timeBase)
{
  return key * av_q2d(timeBase);
}

DemuxPacket* Hold()
{
  DemuxPacket* hold = CDVDDemuxUtils::AllocateDemuxPacket(0);
  hold->iStreamId = HOLD_STREAM_ID;
  return hold;
}
} // namespace

CDemuxDualLayer::~CDemuxDualLayer()
{
  Flush();
}

DemuxPacket* CDemuxDualLayer::AddPacket(DemuxPacket* packet,
                                        int64_t key,
                                        const CDemuxStreamVideo& stream,
                                        const std::map<int, CDemuxStream*>& streams)
{
  const bool isEL = stream.isELStream;

  // the codec stays open for both layers, so a base layer cannot go on alone
  if (!HasEnhancementLayer(streams))
  {
    DropEnhancementLayer(m_enhancementLayer.size());
    DropBaseLayer(m_baseLayer.size());
    CDVDDemuxUtils::FreeDemuxPacket(packet);
    return Hold();
  }

  std::deque<Entry>& own = isEL ? m_enhancementLayer : m_baseLayer;
  std::deque<Entry>& other = isEL ? m_baseLayer : m_enhancementLayer;

  if (key == AV_NOPTS_VALUE)
  {
    CDVDDemuxUtils::FreeDemuxPacket(packet);
    return Hold();
  }

  const AVRational timeBase = static_cast<const AVStream*>(stream.pPrivate)->time_base;
  const auto match = std::ranges::find_if(
      other, [key, timeBase](const Entry& entry)
      { return av_compare_ts(entry.key, entry.timeBase, key, timeBase) == 0; });
  if (match == other.end())
  {
    own.push_back({packet, key, timeBase});
    return Hold();
  }

  // each layer arrives in decode order, so an entry older than the match has no partner left;
  // orphans are dropped, as a dual layer decoder fed a lone base layer corrupts or stalls
  const size_t skipped = match - other.begin();
  const auto logOrphan = [key, timeBase](const Entry& entry, bool entryIsEL)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO,
              "CDemuxDualLayer::AddPacket: orphan {} key {:.3f} passed by key {:.3f}",
              entryIsEL ? "EL" : "BL", Seconds(entry.key, entry.timeBase), Seconds(key, timeBase));
  };
  for (const Entry& entry : own)
    logOrphan(entry, isEL);
  for (size_t i = 0; i < skipped; ++i)
    logOrphan(other[i], !isEL);

  DemuxPacket* base = packet;
  DemuxPacket* enhancement = packet;
  if (isEL)
  {
    DropEnhancementLayer(m_enhancementLayer.size());
    DropBaseLayer(skipped);
    base = m_baseLayer.front().packet;
    m_baseLayer.pop_front();
  }
  else
  {
    DropEnhancementLayer(skipped);
    DropBaseLayer(m_baseLayer.size());
    enhancement = m_enhancementLayer.front().packet;
    m_enhancementLayer.pop_front();
  }

  if (!Join(*base, *enhancement, AV_INPUT_BUFFER_PADDING_SIZE))
  {
    CDVDDemuxUtils::FreeDemuxPacket(base);
    CDVDDemuxUtils::FreeDemuxPacket(enhancement);
    return Hold();
  }
  base->elSize = enhancement->iSize;
  CLog::Log(LOGDEBUG, LOGVIDEO,
            "CDemuxDualLayer::AddPacket: paired key {:.3f}, BL size {}, EL size {}",
            Seconds(key, timeBase), base->iSize, base->elSize);
  CDVDDemuxUtils::FreeDemuxPacket(enhancement);
  return base;
}

void CDemuxDualLayer::DropBaseLayer(size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_baseLayer.front().packet);
    m_baseLayer.pop_front();
  }
}

void CDemuxDualLayer::DropEnhancementLayer(size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_enhancementLayer.front().packet);
    m_enhancementLayer.pop_front();
  }
}

void CDemuxDualLayer::Flush()
{
  for (const Entry& entry : m_baseLayer)
    CDVDDemuxUtils::FreeDemuxPacket(entry.packet);
  m_baseLayer.clear();
  for (const Entry& entry : m_enhancementLayer)
    CDVDDemuxUtils::FreeDemuxPacket(entry.packet);
  m_enhancementLayer.clear();
}

bool CDemuxDualLayer::IsHold(const DemuxPacket& packet)
{
  return packet.iStreamId == HOLD_STREAM_ID;
}

uint8_t* CDemuxDualLayer::GetEnhancementLayer(const DemuxPacket& packet)
{
  if (packet.elSize == 0)
    return nullptr;

  return packet.pData + packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE;
}
