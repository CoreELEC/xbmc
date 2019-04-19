/*
 *  Copyright (C) 2018 Arthur Liberman
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDInputStreamFFmpegArchive.h"
#include "ServiceBroker.h"
#include "addons/PVRClient.h"
#include "pvr/PVRManager.h"
#include "utils/log.h"

CDVDInputStreamFFmpegArchive::CDVDInputStreamFFmpegArchive(const CFileItem& fileitem)
  : CDVDInputStreamFFmpeg(fileitem),
    m_client(CServiceBroker::GetPVRManager().GetClient(fileitem))
{ }

int64_t CDVDInputStreamFFmpegArchive::GetLength()
{
  int64_t ret = 0;
  if (m_client && m_client->GetLiveStreamLength(ret) != PVR_ERROR_NO_ERROR)
  {
    Times times = {0};
    if (GetTimes(times) && times.ptsEnd >= times.ptsBegin)
      ret = static_cast<int64_t>(times.ptsEnd - times.ptsBegin);
  }
  return ret;
}

bool CDVDInputStreamFFmpegArchive::GetTimes(Times &times)
{
  bool ret = false;
  PVR_STREAM_TIMES streamTimes = {0};
  if (m_client && m_client->GetStreamTimes(&streamTimes) == PVR_ERROR_NO_ERROR)
  {
    times.startTime = streamTimes.startTime;
    times.ptsStart = static_cast<double>(streamTimes.ptsStart);
    times.ptsBegin = static_cast<double>(streamTimes.ptsBegin);
    times.ptsEnd = static_cast<double>(streamTimes.ptsEnd);
    ret = true;
  }
  return ret;
}

int64_t CDVDInputStreamFFmpegArchive::Seek(int64_t offset, int whence)
{
  int64_t iPosition = -1;
  if (m_client)
    m_client->SeekLiveStream(offset, whence, iPosition);
  return iPosition;
}

CURL CDVDInputStreamFFmpegArchive::GetURL()
{
  if (m_client)
  {
    if (m_item.HasEPGInfoTag())
      m_client->FillEpgTagStreamFileItem(m_item);
    else if (m_item.HasPVRChannelInfoTag())
      m_client->FillChannelStreamFileItem(m_item);
  }
  return CDVDInputStream::GetURL();
}

bool CDVDInputStreamFFmpegArchive::IsStreamType(DVDStreamType type) const
{
    return CDVDInputStream::IsStreamType(type) || type == DVDSTREAM_TYPE_PVR_ARCHIVE;
}
