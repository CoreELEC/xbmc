/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDFactoryInputStream.h"
#include "DVDInputStream.h"
#include "DVDInputStreamFile.h"
#include "DVDInputStreamNavigator.h"
#include "DVDInputStreamFFmpeg.h"
#include "DVDInputStreamFFmpegArchive.h"
#include "InputStreamAddon.h"
#include "InputStreamMultiSource.h"
#include "InputStreamPVRChannel.h"
#include "InputStreamPVRRecording.h"
#ifdef HAVE_LIBBLURAY
#include "DVDInputStreamBluray.h"
#endif
#include "DVDInputStreamStack.h"
#include "FileItem.h"
#include "storage/MediaManager.h"
#include "URL.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/IFileTypes.h"
#include "utils/URIUtils.h"
#include "ServiceBroker.h"
#include "addons/binary-addons/BinaryAddonManager.h"
#include "Util.h"
#include "utils/log.h"


std::shared_ptr<CDVDInputStream> CDVDFactoryInputStream::CreateInputStream(IVideoPlayer* pPlayer, const CFileItem &fileitem, bool scanforextaudio)
{
  using namespace ADDON;

  std::string file = fileitem.GetDynPath();
  if (scanforextaudio)
  {
    // find any available external audio tracks
    std::vector<std::string> filenames;
    filenames.push_back(file);
    CUtil::ScanForExternalAudio(file, filenames);
    CUtil::ScanForExternalDemuxSub(file, filenames);
    if (filenames.size() >= 2)
    {
      return CreateInputStream(pPlayer, fileitem, filenames);
    }
  }

  BinaryAddonBaseList addonInfos;
  CServiceBroker::GetBinaryAddonManager().GetAddonInfos(addonInfos, true /*enabled only*/, ADDON_INPUTSTREAM);
  for (auto addonInfo : addonInfos)
  {
    if (CInputStreamAddon::Supports(addonInfo, fileitem))
      return std::shared_ptr<CInputStreamAddon>(new CInputStreamAddon(addonInfo, pPlayer, fileitem));
  }

  if (fileitem.IsDiscImage())
  {
#ifdef HAVE_LIBBLURAY
    CURL url("udf://");
    url.SetHostName(file);
    url.SetFileName("BDMV/index.bdmv");
    if(XFILE::CFile::Exists(url.Get()))
      return std::shared_ptr<CDVDInputStreamBluray>(new CDVDInputStreamBluray(pPlayer, fileitem));
#endif

    return std::shared_ptr<CDVDInputStreamNavigator>(new CDVDInputStreamNavigator(pPlayer, fileitem));
  }

#ifdef HAS_DVD_DRIVE
  if(file.compare(g_mediaManager.TranslateDevicePath("")) == 0)
  {
#ifdef HAVE_LIBBLURAY
    if(XFILE::CFile::Exists(URIUtils::AddFileToFolder(file, "BDMV", "index.bdmv")))
      return std::shared_ptr<CDVDInputStreamBluray>(new CDVDInputStreamBluray(pPlayer, fileitem));
#endif

    return std::shared_ptr<CDVDInputStreamNavigator>(new CDVDInputStreamNavigator(pPlayer, fileitem));
  }
#endif

  if (fileitem.IsDVDFile(false, true))
    return std::shared_ptr<CDVDInputStreamNavigator>(new CDVDInputStreamNavigator(pPlayer, fileitem));
  else if (URIUtils::IsPVRChannel(file))
    return std::shared_ptr<CInputStreamPVRChannel>(new CInputStreamPVRChannel(pPlayer, fileitem));
  else if (URIUtils::IsPVRRecording(file))
    return std::shared_ptr<CInputStreamPVRRecording>(new CInputStreamPVRRecording(pPlayer, fileitem));
#ifdef HAVE_LIBBLURAY
  else if (fileitem.IsType(".bdmv") || fileitem.IsType(".mpls") || StringUtils::StartsWithNoCase(file, "bluray:"))
    return std::shared_ptr<CDVDInputStreamBluray>(new CDVDInputStreamBluray(pPlayer, fileitem));
#endif
  else if(StringUtils::StartsWithNoCase(file, "rtp://") ||
          StringUtils::StartsWithNoCase(file, "rtsp://") ||
          StringUtils::StartsWithNoCase(file, "rtsps://") ||
          StringUtils::StartsWithNoCase(file, "sdp://") ||
          StringUtils::StartsWithNoCase(file, "udp://") ||
          StringUtils::StartsWithNoCase(file, "tcp://") ||
          StringUtils::StartsWithNoCase(file, "mms://") ||
          StringUtils::StartsWithNoCase(file, "mmst://") ||
          StringUtils::StartsWithNoCase(file, "mmsh://") ||
          StringUtils::StartsWithNoCase(file, "rtmp://") ||
          StringUtils::StartsWithNoCase(file, "rtmpt://") ||
          StringUtils::StartsWithNoCase(file, "rtmpe://") ||
          StringUtils::StartsWithNoCase(file, "rtmpte://") ||
          StringUtils::StartsWithNoCase(file, "rtmps://"))
  {
    return std::shared_ptr<CDVDInputStreamFFmpeg>(new CDVDInputStreamFFmpeg(fileitem));
  }
  else if(StringUtils::StartsWithNoCase(file, "stack://"))
    return std::shared_ptr<CDVDInputStreamStack>(new CDVDInputStreamStack(fileitem));

  CFileItem finalFileitem(fileitem);

  if (finalFileitem.IsInternetStream())
  {
    if (finalFileitem.ContentLookup())
    {
      CURL origUrl(finalFileitem.GetDynURL());
      XFILE::CCurlFile curlFile;
      // try opening the url to resolve all redirects if any
      try
      {
        if (curlFile.Open(finalFileitem.GetDynURL()))
        {
          CURL finalUrl(curlFile.GetURL());
          finalUrl.SetProtocolOptions(origUrl.GetProtocolOptions());
          finalUrl.SetUserName(origUrl.GetUserName());
          finalUrl.SetPassword(origUrl.GetPassWord());
          finalFileitem.SetDynPath(finalUrl.Get());
        }
        curlFile.Close();
      }
      catch (XFILE::CRedirectException *pRedirectEx)
      {
        if (pRedirectEx)
        {
          delete pRedirectEx->m_pNewFileImp;
          delete pRedirectEx;
        }
      }
    }

    if (finalFileitem.IsType(".m3u8") || finalFileitem.IsType(".php"))
    {
      if (fileitem.IsPVRChannelWithArchive() || fileitem.IsEPGWithArchive())
      {
        CLog::Log(LOGDEBUG, "%s: CDVDInputStreamFFmpegArchive", __FUNCTION__);
        return std::shared_ptr<CDVDInputStreamFFmpegArchive>(new CDVDInputStreamFFmpegArchive(finalFileitem));
      }
      else
      {
        return std::shared_ptr<CDVDInputStreamFFmpeg>(new CDVDInputStreamFFmpeg(finalFileitem));
      }
    }

    if (finalFileitem.GetMimeType() == "application/vnd.apple.mpegurl")
      return std::shared_ptr<CDVDInputStreamFFmpeg>(new CDVDInputStreamFFmpeg(finalFileitem));

    if (URIUtils::IsProtocol(finalFileitem.GetPath(), "udp"))
      return std::shared_ptr<CDVDInputStreamFFmpeg>(new CDVDInputStreamFFmpeg(finalFileitem));
  }

  // our file interface handles all these types of streams
  CLog::Log(LOGDEBUG, "%s: All else failed, creating CDVDInputStreamFile", __FUNCTION__);
  return std::shared_ptr<CDVDInputStreamFile>(new CDVDInputStreamFile(finalFileitem,
                                                                      XFILE::READ_TRUNCATED |
                                                                      XFILE::READ_BITRATE |
                                                                      XFILE::READ_CHUNKED));
}

std::shared_ptr<CDVDInputStream> CDVDFactoryInputStream::CreateInputStream(IVideoPlayer* pPlayer, const CFileItem &fileitem, const std::vector<std::string>& filenames)
{
  return std::shared_ptr<CInputStreamMultiSource>(new CInputStreamMultiSource(pPlayer, fileitem, filenames));
}
