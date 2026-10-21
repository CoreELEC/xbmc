/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDStreamInfo.h"

#include <memory>
#include <optional>
#include <string>

class CDVDDemux;
class CDVDInputStream;
class CDVDVideoCodec;
class CFileItem;
class CProcessInfo;
struct VideoPicture;

//! \brief Admission of one job that opens a file and software decodes from it. With \p wait it
//! waits while the memory left for such jobs is below a reserve plus a budget per running job; the
//! first job and the GUI thread always start. Without \p wait the job is only counted.
class CVideoDecodeJob
{
public:
  explicit CVideoDecodeJob(bool wait = true);
  ~CVideoDecodeJob();
  CVideoDecodeJob(CVideoDecodeJob&& other) noexcept;
  CVideoDecodeJob& operator=(CVideoDecodeJob&& other) noexcept;
  CVideoDecodeJob(const CVideoDecodeJob&) = delete;
  CVideoDecodeJob& operator=(const CVideoDecodeJob&) = delete;

private:
  void Release();

  bool m_admitted{false};
};

//! \brief An open demux-and-decode session on a file's first video stream.
struct VideoDecodeSession
{
  CVideoDecodeJob decodeJob;
  std::shared_ptr<CDVDInputStream> inputStream;
  std::unique_ptr<CDVDDemux> demuxer;
  int videoStream{-1};
  int64_t demuxerId{-1};
  std::unique_ptr<CProcessInfo> processInfo;
  CDVDStreamInfo hint;
  std::unique_ptr<CDVDVideoCodec> codec;
};

//! \brief Open \p fileItem for decoding: input stream, demuxer with every stream but the first
//! real video one disabled, and a codec for it opened with \p codecOptions.
std::optional<VideoDecodeSession> OpenVideoDecodeSession(const CFileItem& fileItem,
                                                         int codecOptions,
                                                         const std::string& redactPath);

//! \brief Seek to a position in milliseconds and decode the first usable picture there.
bool SeekAndDecodePictureAt(CDVDDemux& demuxer,
                            CDVDVideoCodec& codec,
                            int videoStream,
                            int64_t seekToMs,
                            const std::string& redactPath,
                            VideoPicture& picture,
                            int& packetsTried);
