/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoThumbLoader.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "cores/VideoPlayer/DVDFileInfo.h"
#include "FileItem.h"
#include "ServiceBroker.h"
#include "filesystem/Directory.h"
#include "filesystem/DirectoryCache.h"
#include "filesystem/StackDirectory.h"
#include "guilib/GUIComponent.h"
#include "filesystem/File.h"
#include "filesystem/CurlFile.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/StereoscopicsManager.h"
#include "GUIUserMessages.h"
#include "music/MusicDatabase.h"
#include "settings/AdvancedSettings.h"
#include "settings/lib/Setting.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "cores/VideoSettings.h"
#include "TextureCache.h"
#include "URL.h"
#include "utils/log.h"
#include "utils/EmbeddedArt.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "video/tags/VideoInfoTagLoaderFactory.h"
#include "video/VideoDatabase.h"
#include "video/VideoInfoTag.h"

using namespace XFILE;
using namespace VIDEO;

CThumbExtractor::CThumbExtractor(const CFileItem& item,
                                 const std::string& listpath,
                                 bool thumb,
                                 const std::string& target,
                                 int64_t pos,
                                 bool fillStreamDetails)
{
  m_listpath = listpath;
  m_target = target;
  m_thumb = thumb;
  m_item = item;
  m_pos = pos;
  m_fillStreamDetails = fillStreamDetails;

  if (item.IsVideoDb() && item.HasVideoInfoTag())
    m_item.SetPath(item.GetVideoInfoTag()->m_strFileNameAndPath);

  if (m_item.IsStack())
    m_item.SetPath(CStackDirectory::GetFirstStackedFile(m_item.GetPath()));
}

CThumbExtractor::~CThumbExtractor() = default;

bool CThumbExtractor::operator==(const CJob* job) const
{
  if (strcmp(job->GetType(),GetType()) == 0)
  {
    const CThumbExtractor* jobExtract = dynamic_cast<const CThumbExtractor*>(job);
    if (jobExtract && jobExtract->m_listpath == m_listpath
                   && jobExtract->m_target == m_target)
      return true;
  }
  return false;
}

bool CThumbExtractor::DoWork()
{
  if (m_item.IsLiveTV()
  // Due to a pvr addon api design flaw (no support for multiple concurrent streams
  // per addon instance), pvr recording thumbnail extraction does not work (reliably).
  ||  URIUtils::IsPVRRecording(m_item.GetDynPath())
  ||  URIUtils::IsUPnP(m_item.GetPath())
  ||  URIUtils::IsBluray(m_item.GetPath())
  ||  m_item.IsBDFile()
  ||  m_item.IsDVD()
  ||  m_item.IsDiscImage()
  ||  m_item.IsDVDFile(false, true)
  ||  m_item.IsInternetStream()
  ||  m_item.IsDiscStub()
  ||  m_item.IsPlayList())
    return false;

  // For HTTP/FTP we only allow extraction when on a LAN
  if (URIUtils::IsRemote(m_item.GetPath()) &&
     !URIUtils::IsOnLAN(m_item.GetPath())  &&
     (URIUtils::IsFTP(m_item.GetPath())    ||
      URIUtils::IsHTTP(m_item.GetPath())))
    return false;

  bool result=false;
  if (m_thumb)
  {
    CLog::Log(LOGDEBUG,"%s - trying to extract thumb from video file %s", __FUNCTION__, CURL::GetRedacted(m_item.GetPath()).c_str());
    // construct the thumb cache file
    CTextureDetails details;
    details.file = CTextureCache::GetCacheFile(m_target) + ".jpg";
    result = CDVDFileInfo::ExtractThumb(m_item, details, m_fillStreamDetails ? &m_item.GetVideoInfoTag()->m_streamDetails : nullptr, m_pos);
    if(result)
    {
      CTextureCache::GetInstance().AddCachedTexture(m_target, details);
      m_item.SetProperty("HasAutoThumb", true);
      m_item.SetProperty("AutoThumbImage", m_target);
      m_item.SetArt("thumb", m_target);

      CVideoInfoTag* info = m_item.GetVideoInfoTag();
      if (info->m_iDbId > 0 && !info->m_type.empty())
      {
        CVideoDatabase db;
        if (db.Open())
        {
          db.SetArtForItem(info->m_iDbId, info->m_type, "thumb", m_item.GetArt("thumb"));
          db.Close();
        }
      }
    }
  }
  else if (!m_item.IsPlugin() &&
           (!m_item.HasVideoInfoTag() ||
           !m_item.GetVideoInfoTag()->HasStreamDetails()))
  {
    // No tag or no details set, so extract them
    CLog::Log(LOGDEBUG,"%s - trying to extract filestream details from video file %s", __FUNCTION__, CURL::GetRedacted(m_item.GetPath()).c_str());
    result = CDVDFileInfo::GetFileStreamDetails(&m_item);
  }

  if (result)
  {
    CVideoInfoTag* info = m_item.GetVideoInfoTag();
    CVideoDatabase db;
    if (db.Open())
    {
      if (URIUtils::IsStack(m_listpath))
      {
        // Don't know the total time of the stack, so set duration to zero to avoid confusion
        info->m_streamDetails.SetVideoDuration(0, 0);

        // Restore original stack path
        m_item.SetPath(m_listpath);
      }

      if (info->m_iFileId < 0)
        db.SetStreamDetailsForFile(info->m_streamDetails, !info->m_strFileNameAndPath.empty() ? info->m_strFileNameAndPath : m_item.GetPath());
      else
        db.SetStreamDetailsForFileId(info->m_streamDetails, info->m_iFileId);

      // overwrite the runtime value if the one from streamdetails is available
      if (info->m_iDbId > 0
          && info->GetStaticDuration() != info->GetDuration())
      {
        info->SetDuration(info->GetDuration());

        // store the updated information in the database
        db.SetDetailsForItem(info->m_iDbId, info->m_type, *info, m_item.GetArt());
      }

      db.Close();
    }
    return true;
  }

  return false;
}

CVideoThumbLoader::CVideoThumbLoader() :
  CThumbLoader(), CJobQueue(true, 1, CJob::PRIORITY_LOW_PAUSABLE)
{
  m_videoDatabase = new CVideoDatabase();
}

CVideoThumbLoader::~CVideoThumbLoader()
{
  StopThread();
  delete m_videoDatabase;
}

void CVideoThumbLoader::OnLoaderStart()
{
  m_videoDatabase->Open();
  m_artCache.clear();
  CThumbLoader::OnLoaderStart();
}

void CVideoThumbLoader::OnLoaderFinish()
{
  m_videoDatabase->Close();
  m_artCache.clear();
  CThumbLoader::OnLoaderFinish();
}

static void SetupRarOptions(CFileItem& item, const std::string& path)
{
  std::string path2(path);
  if (item.IsVideoDb() && item.HasVideoInfoTag())
    path2 = item.GetVideoInfoTag()->m_strFileNameAndPath;
  CURL url(path2);
  std::string opts = url.GetOptions();
  if (opts.find("flags") != std::string::npos)
    return;
  if (opts.size())
    opts += "&flags=8";
  else
    opts = "?flags=8";
  url.SetOptions(opts);
  if (item.IsVideoDb() && item.HasVideoInfoTag())
    item.GetVideoInfoTag()->m_strFileNameAndPath = url.Get();
  else
    item.SetPath(url.Get());
  g_directoryCache.ClearDirectory(url.GetWithoutFilename());
}

std::vector<std::string> CVideoThumbLoader::GetArtTypes(const std::string &type)
{
  const std::shared_ptr<CAdvancedSettings> advancedSettings = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  std::vector<std::string> ret;
  if (type == MediaTypeEpisode)
  {
    ret = {"thumb"};
    for (auto& artType : advancedSettings->m_videoEpisodeExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type == MediaTypeTvShow)
  {
    ret = {"poster", "fanart", "banner"};
    for (auto& artType : advancedSettings->m_videoTvShowExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type == MediaTypeSeason)
  {
    ret = {"poster", "fanart", "banner"};
    for (auto& artType : advancedSettings->m_videoTvSeasonExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type == MediaTypeMovie)
  {
    ret = {"poster", "fanart"};
    for (auto& artType : advancedSettings->m_videoMovieExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type == MediaTypeVideoCollection)
  {
    ret = {"poster", "fanart"};
    for (auto& artType : advancedSettings->m_videoMovieSetExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type == MediaTypeMusicVideo)
  {
    ret = {"poster", "fanart"};
    for (auto& artType : advancedSettings->m_videoMusicVideoExtraArt)
    {
      if (find(ret.begin(), ret.end(), artType) == ret.end())
        ret.push_back(artType);
    }
  }
  else if (type.empty()) // unknown, just the basics
    ret = { "poster", "fanart", "banner", "thumb" };

  return ret;
}

/**
 * Look for a thumbnail for pItem.  If one does not exist, look for an autogenerated
 * thumbnail.  If that does not exist, attempt to autogenerate one.  Finally, check
 * for the existence of fanart and set properties accordingly.
 * @return: true if pItem has been modified
 */
bool CVideoThumbLoader::LoadItem(CFileItem* pItem)
{
  bool result  = LoadItemCached(pItem);
       result |= LoadItemLookup(pItem);

  return result;
}

bool CVideoThumbLoader::LoadItemCached(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive
  ||  pItem->IsParentFolder())
    return false;

  m_videoDatabase->Open();

  if (!pItem->HasVideoInfoTag() || !pItem->GetVideoInfoTag()->HasStreamDetails()) // no stream details
  {
    if ((pItem->HasVideoInfoTag() && pItem->GetVideoInfoTag()->m_iFileId >= 0) // file (or maybe folder) is in the database
    || (!pItem->m_bIsFolder && pItem->IsVideo())) // Some other video file for which we haven't yet got any database details
    {
      if (m_videoDatabase->GetStreamDetails(*pItem))
        pItem->SetInvalid();
    }
  }

  // video db items normally have info in the database
  if (pItem->HasVideoInfoTag() && !pItem->HasArt("thumb"))
  {
    FillLibraryArt(*pItem);

    if (!pItem->GetVideoInfoTag()->m_type.empty()                &&
         pItem->GetVideoInfoTag()->m_type != MediaTypeMovie      &&
         pItem->GetVideoInfoTag()->m_type != MediaTypeTvShow     &&
         pItem->GetVideoInfoTag()->m_type != MediaTypeEpisode    &&
         pItem->GetVideoInfoTag()->m_type != MediaTypeMusicVideo)
    {
      m_videoDatabase->Close();
      return true; // nothing else to be done
    }
  }

  // if we have no art, look for it all
  std::map<std::string, std::string> artwork = pItem->GetArt();
  if (artwork.empty())
  {
    std::vector<std::string> artTypes = GetArtTypes(pItem->HasVideoInfoTag() ? pItem->GetVideoInfoTag()->m_type : "");
    if (find(artTypes.begin(), artTypes.end(), "thumb") == artTypes.end())
      artTypes.push_back("thumb"); // always look for "thumb" art for files
    for (std::vector<std::string>::const_iterator i = artTypes.begin(); i != artTypes.end(); ++i)
    {
      std::string type = *i;
      std::string art = GetCachedImage(*pItem, type);
      if (!art.empty())
        artwork.insert(std::make_pair(type, art));
    }
    SetArt(*pItem, artwork);
  }

  // hide thumb if episode is unwatched 
  std::shared_ptr<CSettingList> setting(std::dynamic_pointer_cast<CSettingList>(
    CServiceBroker::GetSettingsComponent()->GetSettings()->GetSetting(CSettings::SETTING_VIDEOLIBRARY_SHOWUNWATCHEDPLOTS)));
  if (pItem->HasArt("thumb") && pItem->HasVideoInfoTag() &&
      pItem->GetVideoInfoTag()->m_type == MediaTypeEpisode &&
      pItem->GetVideoInfoTag()->GetPlayCount() == 0 &&
      setting && 
      !setting->FindIntInList(CSettings::VIDEOLIBRARY_THUMB_SHOW_UNWATCHED_EPISODE)
     )
  {
    pItem->SetArt("thumb", "OverlaySpoiler.png");
  }

  m_videoDatabase->Close();

  return true;
}

bool CVideoThumbLoader::LoadItemLookup(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive || pItem->IsParentFolder() || pItem->GetPath() == "add")
    return false;

  if (pItem->HasVideoInfoTag()                                &&
     !pItem->GetVideoInfoTag()->m_type.empty()                &&
      pItem->GetVideoInfoTag()->m_type != MediaTypeMovie      &&
      pItem->GetVideoInfoTag()->m_type != MediaTypeTvShow     &&
      pItem->GetVideoInfoTag()->m_type != MediaTypeEpisode    &&
      pItem->GetVideoInfoTag()->m_type != MediaTypeMusicVideo)
    return false; // Nothing to do here

  DetectAndAddMissingItemData(*pItem);

  m_videoDatabase->Open();

  std::map<std::string, std::string> artwork = pItem->GetArt();
  std::vector<std::string> artTypes = GetArtTypes(pItem->HasVideoInfoTag() ? pItem->GetVideoInfoTag()->m_type : "");
  if (find(artTypes.begin(), artTypes.end(), "thumb") == artTypes.end())
    artTypes.push_back("thumb"); // always look for "thumb" art for files
  for (std::vector<std::string>::const_iterator i = artTypes.begin(); i != artTypes.end(); ++i)
  {
    std::string type = *i;
    if (!pItem->HasArt(type))
    {
      std::string art = GetLocalArt(*pItem, type, type=="fanart");
      if (!art.empty()) // cache it
      {
        SetCachedImage(*pItem, type, art);
        CTextureCache::GetInstance().BackgroundCacheImage(art);
        artwork.insert(std::make_pair(type, art));
      }
      else
      {
        // If nothing was found, try embedded art
        if (pItem->HasVideoInfoTag() && !pItem->GetVideoInfoTag()->m_coverArt.empty())
        {
          for (auto& it : pItem->GetVideoInfoTag()->m_coverArt)
          {
            if (it.m_type == type)
            {
              art = CTextureUtils::GetWrappedImageURL(pItem->GetPath(), "video_" + type);
              artwork.insert(std::make_pair(type, art));
            }
          }
        }
      }
    }
  }
  SetArt(*pItem, artwork);

  // We can only extract flags/thumbs for file-like items
  if (!pItem->m_bIsFolder && pItem->IsVideo())
  {
    std::string url = pItem->GetArt("thumb");
    if (!CTextureCache::GetInstance().HasCachedImage(url))
    {
      // An auto-generated thumb may have been cached on a different device - check we have it here
      if (StringUtils::StartsWith(url, "image://video@"))
        pItem->SetArt("thumb", "");

      // Check invalid redirections
      else if (URIUtils::IsInternetStream(url))
      {
        std::string mimetype;
        bool valid = XFILE::CCurlFile::GetMimeType(CURL(url), mimetype);
        if (valid && !StringUtils::StartsWith(mimetype, "image/"))
          valid = false;
        if (!valid)
          pItem->SetArt("thumb", "");
      }

      // Check if thumb source still exists
      else if (!XFILE::CFile::Exists(url))
        pItem->SetArt("thumb", "");
    }

    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (!pItem->HasArt("thumb"))
    {
      // create unique thumb for auto generated thumbs
      std::string thumbURL = GetEmbeddedThumbURL(*pItem);
      if (CTextureCache::GetInstance().HasCachedImage(thumbURL))
      {
        CTextureCache::GetInstance().BackgroundCacheImage(thumbURL);
        pItem->SetProperty("HasAutoThumb", true);
        pItem->SetProperty("AutoThumbImage", thumbURL);
        pItem->SetArt("thumb", thumbURL);

        if (pItem->HasVideoInfoTag())
        {
          // Item has cached autogen image but no art entry. Save it to db.
          CVideoInfoTag* info = pItem->GetVideoInfoTag();
          if (info->m_iDbId > 0 && !info->m_type.empty())
            m_videoDatabase->SetArtForItem(info->m_iDbId, info->m_type, "thumb", thumbURL);
        }
      }
      else if (settings->GetBool(CSettings::SETTING_MYVIDEOS_EXTRACTTHUMB) &&
               settings->GetBool(CSettings::SETTING_MYVIDEOS_EXTRACTFLAGS))
      {
        CFileItem item(*pItem);
        std::string path(item.GetPath());
        if (URIUtils::IsInRAR(item.GetPath()))
          SetupRarOptions(item,path);

        CThumbExtractor* extract = new CThumbExtractor(item, path, true, thumbURL);
        AddJob(extract);

        m_videoDatabase->Close();
        return true;
      }
    }

    // flag extraction
    if (settings->GetBool(CSettings::SETTING_MYVIDEOS_EXTRACTFLAGS) &&
       (!pItem->HasVideoInfoTag()                     ||
        !pItem->GetVideoInfoTag()->HasStreamDetails() ) )
    {
      CFileItem item(*pItem);
      std::string path(item.GetPath());
      if (URIUtils::IsInRAR(item.GetPath()))
        SetupRarOptions(item,path);
      CThumbExtractor* extract = new CThumbExtractor(item,path,false);
      AddJob(extract);
    }
  }

  m_videoDatabase->Close();
  return true;
}

void CVideoThumbLoader::SetArt(CFileItem &item, const std::map<std::string, std::string> &artwork)
{
  item.SetArt(artwork);
  if (artwork.find("thumb") == artwork.end())
  { // set fallback for "thumb"
    if (artwork.find("poster") != artwork.end())
      item.SetArtFallback("thumb", "poster");
    else if (artwork.find("banner") != artwork.end())
      item.SetArtFallback("thumb", "banner");
  }
}

bool CVideoThumbLoader::FillLibraryArt(CFileItem &item)
{
  CVideoInfoTag &tag = *item.GetVideoInfoTag();
  if (tag.m_iDbId > -1 && !tag.m_type.empty())
  {
    std::map<std::string, std::string> artwork;
    m_videoDatabase->Open();
    if (m_videoDatabase->GetArtForItem(tag.m_iDbId, tag.m_type, artwork))
      SetArt(item, artwork);
    else if (tag.m_type == "actor" && !tag.m_artist.empty())
    { // we retrieve music video art from the music database (no backward compat)
      CMusicDatabase database;
      database.Open();
      int idArtist = database.GetArtistByName(item.GetLabel());
      if (database.GetArtForItem(idArtist, MediaTypeArtist, artwork))
        item.SetArt(artwork);
    }
    else if (tag.m_type == MediaTypeAlbum)
    { // we retrieve music video art from the music database (no backward compat)
      CMusicDatabase database;
      database.Open();
      int idAlbum = database.GetAlbumByName(item.GetLabel(), tag.m_artist);
      if (database.GetArtForItem(idAlbum, MediaTypeAlbum, artwork))
        item.SetArt(artwork);
    }

    if (tag.m_type == MediaTypeEpisode || tag.m_type == MediaTypeSeason)
    {
      // For episodes and seasons, we want to set fanart for that of the show
      if (!item.HasArt("tvshow.fanart") && tag.m_iIdShow >= 0)
      {
        const ArtMap& artmap = GetArtFromCache(MediaTypeTvShow, tag.m_iIdShow);
        if (!artmap.empty())
        {
          item.AppendArt(artmap, MediaTypeTvShow);
          item.SetArtFallback("fanart", "tvshow.fanart");
          item.SetArtFallback("tvshow.thumb", "tvshow.poster");
        }
      }

      if (tag.m_type == MediaTypeEpisode && !item.HasArt("season.poster") && tag.m_iSeason > -1)
      {
        const ArtMap& artmap = GetArtFromCache(MediaTypeSeason, tag.m_iIdSeason);
        if (!artmap.empty())
          item.AppendArt(artmap, MediaTypeSeason);
      }
    }
    else if (tag.m_type == MediaTypeMovie && tag.m_set.id >= 0 && !item.HasArt("set.fanart"))
    {
      const ArtMap& artmap = GetArtFromCache(MediaTypeVideoCollection, tag.m_set.id);
      if (!artmap.empty())
        item.AppendArt(artmap, MediaTypeVideoCollection);
    }
    m_videoDatabase->Close();
  }
  return !item.GetArt().empty();
}

bool CVideoThumbLoader::FillThumb(CFileItem &item)
{
  if (item.HasArt("thumb"))
    return true;
  std::string thumb = GetCachedImage(item, "thumb");
  if (thumb.empty())
  {
    thumb = GetLocalArt(item, "thumb");
    if (!thumb.empty())
      SetCachedImage(item, "thumb", thumb);
  }
  if (!thumb.empty())
    item.SetArt("thumb", thumb);
  else
  {
    // If nothing was found, try embedded art
    if (item.HasVideoInfoTag() && !item.GetVideoInfoTag()->m_coverArt.empty())
    {
      for (auto& it : item.GetVideoInfoTag()->m_coverArt)
      {
        if (it.m_type == "thumb")
        {
          thumb = CTextureUtils::GetWrappedImageURL(item.GetPath(), "video_" + it.m_type);
          item.SetArt(it.m_type, thumb);
        }
      }
    }
  }

  return !thumb.empty();
}

std::string CVideoThumbLoader::GetLocalArt(const CFileItem &item, const std::string &type, bool checkFolder)
{
  if (item.SkipLocalArt())
    return "";

  /* Cache directory for (sub) folders on streamed filesystems. We need to do this
     else entering (new) directories from the app thread becomes much slower. This
     is caused by the fact that Curl Stat/Exist() is really slow and that the
     thumbloader thread accesses the streamed filesystem at the same time as the
     App thread and the latter has to wait for it.
   */
  if (item.m_bIsFolder && (item.IsInternetStream(true) || CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_cacheBufferMode == CACHE_BUFFER_MODE_ALL))
  {
    CFileItemList items; // Dummy list
    CDirectory::GetDirectory(item.GetPath(), items, "", DIR_FLAG_NO_FILE_DIRS | DIR_FLAG_READ_CACHE | DIR_FLAG_NO_FILE_INFO);
  }

  std::string art;
  if (!type.empty())
  {
    art = item.FindLocalArt(type + ".jpg", checkFolder);
    if (art.empty())
      art = item.FindLocalArt(type + ".png", checkFolder);
  }
  if (art.empty() && (type.empty() || type == "thumb"))
  { // backward compatibility
    art = item.FindLocalArt("", false);
    if (art.empty() && (checkFolder || (item.m_bIsFolder && !item.IsFileFolder()) || item.IsOpticalMediaFile()))
    { // try movie.tbn
      art = item.FindLocalArt("movie.tbn", true);
      if (art.empty()) // try folder.jpg
        art = item.FindLocalArt("folder.jpg", true);
    }
  }

  return art;
}

std::string CVideoThumbLoader::GetEmbeddedThumbURL(const CFileItem &item)
{
  std::string path(item.GetPath());
  if (item.IsVideoDb() && item.HasVideoInfoTag())
    path = item.GetVideoInfoTag()->m_strFileNameAndPath;
  if (URIUtils::IsStack(path))
    path = CStackDirectory::GetFirstStackedFile(path);

  return CTextureUtils::GetWrappedImageURL(path, "video");
}

bool CVideoThumbLoader::GetEmbeddedThumb(const std::string& path,
                                         const std::string& type, EmbeddedArt& art)
{
  CFileItem item(path, false);
  std::unique_ptr<IVideoInfoTagLoader> pLoader;
  pLoader.reset(CVideoInfoTagLoaderFactory::CreateLoader(item,ADDON::ScraperPtr(),false));
  CVideoInfoTag tag;
  std::vector<EmbeddedArt> artv;
  if (pLoader)
    pLoader->Load(tag, false, &artv);

  for (const EmbeddedArt& it : artv)
  {
    if (it.m_type == type)
    {
      art = it;
      break;
    }
  }

  return !art.Empty();
}

void CVideoThumbLoader::OnJobComplete(unsigned int jobID, bool success, CJob* job)
{
  if (success)
  {
    CThumbExtractor* loader = static_cast<CThumbExtractor*>(job);
    loader->m_item.SetPath(loader->m_listpath);

    if (m_pObserver)
      m_pObserver->OnItemLoaded(&loader->m_item);
    CFileItemPtr pItem(new CFileItem(loader->m_item));
    CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_ITEM, 0, pItem);
    CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(msg);
  }
  CJobQueue::OnJobComplete(jobID, success, job);
}

void CVideoThumbLoader::DetectAndAddMissingItemData(CFileItem &item)
{
  if (item.m_bIsFolder) return;

  if (item.HasVideoInfoTag())
  {
    CStreamDetails& details = item.GetVideoInfoTag()->m_streamDetails;

    // add audio language properties
    for (int i = 1; i <= details.GetAudioStreamCount(); i++)
    {
      std::string index = StringUtils::Format("%i", i);
      item.SetProperty("AudioChannels." + index, details.GetAudioChannels(i));
      item.SetProperty("AudioCodec."    + index, details.GetAudioCodec(i).c_str());
      item.SetProperty("AudioLanguage." + index, details.GetAudioLanguage(i).c_str());
    }

    // add subtitle language properties
    for (int i = 1; i <= details.GetSubtitleStreamCount(); i++)
    {
      std::string index = StringUtils::Format("%i", i);
      item.SetProperty("SubtitleLanguage." + index, details.GetSubtitleLanguage(i).c_str());
    }
  }

  const CStereoscopicsManager &stereoscopicsManager = CServiceBroker::GetGUI()->GetStereoscopicsManager();

  std::string stereoMode;

  // detect stereomode for videos
  if (item.HasVideoInfoTag())
    stereoMode = item.GetVideoInfoTag()->m_streamDetails.GetStereoMode();

  if (stereoMode.empty())
  {
    std::string path = item.GetPath();
    if (item.IsVideoDb() && item.HasVideoInfoTag())
      path = item.GetVideoInfoTag()->GetPath();

    // check for custom stereomode setting in video settings
    CVideoSettings itemVideoSettings;
    m_videoDatabase->Open();
    if (m_videoDatabase->GetVideoSettings(item, itemVideoSettings) && itemVideoSettings.m_StereoMode != RENDER_STEREO_MODE_OFF)
    {
      stereoMode = CStereoscopicsManager::ConvertGuiStereoModeToString(static_cast<RENDER_STEREO_MODE>(itemVideoSettings.m_StereoMode));
    }
    m_videoDatabase->Close();

    // still empty, try grabbing from filename
    //! @todo in case of too many false positives due to using the full path, extract the filename only using string utils
    if (stereoMode.empty())
      stereoMode = stereoscopicsManager.DetectStereoModeByString(path);
  }

  if (!stereoMode.empty())
    item.SetProperty("stereomode", CStereoscopicsManager::NormalizeStereoMode(stereoMode));
}

const ArtMap& CVideoThumbLoader::GetArtFromCache(const std::string &mediaType, const int id)
{
  std::pair<MediaType, int> key = std::make_pair(mediaType, id);
  auto it = m_artCache.find(key);
  if (it == m_artCache.end())
  {
    ArtMap newart;
    m_videoDatabase->GetArtForItem(id, mediaType, newart);
    it = m_artCache.insert(std::make_pair(key, std::move(newart))).first;
  }
  return it->second;
}
