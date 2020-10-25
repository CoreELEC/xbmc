/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

#include "settings/SettingControl.h"
#include "settings/SettingCreator.h"
#include "settings/SettingsBase.h"

class CSettingList;
class TiXmlNode;

/*!
 \brief Wrapper around CSettingsManager responsible for properly setting up
 the settings manager and registering all the callbacks, handlers and custom
 setting types.
 \sa CSettingsManager
 */
class CSettings : public CSettingsBase, public CSettingCreator, public CSettingControlCreator
{
public:
  static const std::string SETTING_LOOKANDFEEL_SKIN;
  static const std::string SETTING_LOOKANDFEEL_SKINSETTINGS;
  static const std::string SETTING_LOOKANDFEEL_SKINTHEME;
  static const std::string SETTING_LOOKANDFEEL_SKINCOLORS;
  static const std::string SETTING_LOOKANDFEEL_FONT;
  static const std::string SETTING_LOOKANDFEEL_SKINZOOM;
  static const std::string SETTING_LOOKANDFEEL_STARTUPACTION;
  static const std::string SETTING_LOOKANDFEEL_STARTUPWINDOW;
  static const std::string SETTING_LOOKANDFEEL_SOUNDSKIN;
  static const std::string SETTING_LOOKANDFEEL_ENABLERSSFEEDS;
  static const std::string SETTING_LOOKANDFEEL_RSSEDIT;
  static const std::string SETTING_LOOKANDFEEL_STEREOSTRENGTH;
  static const std::string SETTING_LOCALE_LANGUAGE;
  static const std::string SETTING_LOCALE_COUNTRY;
  static const std::string SETTING_LOCALE_CHARSET;
  static const std::string SETTING_LOCALE_KEYBOARDLAYOUTS;
  static const std::string SETTING_LOCALE_ACTIVEKEYBOARDLAYOUT;
  static const std::string SETTING_LOCALE_TIMEZONECOUNTRY;
  static const std::string SETTING_LOCALE_TIMEZONE;
  static const std::string SETTING_LOCALE_SHORTDATEFORMAT;
  static const std::string SETTING_LOCALE_LONGDATEFORMAT;
  static const std::string SETTING_LOCALE_TIMEFORMAT;
  static const std::string SETTING_LOCALE_USE24HOURCLOCK;
  static const std::string SETTING_LOCALE_TEMPERATUREUNIT;
  static const std::string SETTING_LOCALE_SPEEDUNIT;
  static const std::string SETTING_FILELISTS_SHOWPARENTDIRITEMS;
  static const std::string SETTING_FILELISTS_SHOWEXTENSIONS;
  static const std::string SETTING_FILELISTS_IGNORETHEWHENSORTING;
  static const std::string SETTING_FILELISTS_ALLOWFILEDELETION;
  static const std::string SETTING_FILELISTS_SHOWADDSOURCEBUTTONS;
  static const std::string SETTING_FILELISTS_SHOWHIDDEN;
  static const std::string SETTING_SCREENSAVER_MODE;
  static const std::string SETTING_SCREENSAVER_SETTINGS;
  static const std::string SETTING_SCREENSAVER_PREVIEW;
  static const std::string SETTING_SCREENSAVER_TIME;
  static const std::string SETTING_SCREENSAVER_USEMUSICVISINSTEAD;
  static const std::string SETTING_SCREENSAVER_USEDIMONPAUSE;
  static const std::string SETTING_WINDOW_WIDTH;
  static const std::string SETTING_WINDOW_HEIGHT;
  static const std::string SETTING_VIDEOLIBRARY_SHOWUNWATCHEDPLOTS;
  static const std::string SETTING_VIDEOLIBRARY_ACTORTHUMBS;
  static const std::string SETTING_MYVIDEOS_FLATTEN;
  static const std::string SETTING_VIDEOLIBRARY_FLATTENTVSHOWS;
  static const std::string SETTING_VIDEOLIBRARY_TVSHOWSSELECTFIRSTUNWATCHEDITEM;
  static const std::string SETTING_VIDEOLIBRARY_TVSHOWSINCLUDEALLSEASONSANDSPECIALS;
  static const std::string SETTING_VIDEOLIBRARY_SHOWALLITEMS;
  static const std::string SETTING_VIDEOLIBRARY_GROUPMOVIESETS;
  static const std::string SETTING_VIDEOLIBRARY_GROUPSINGLEITEMSETS;
  static const std::string SETTING_VIDEOLIBRARY_UPDATEONSTARTUP;
  static const std::string SETTING_VIDEOLIBRARY_BACKGROUNDUPDATE;
  static const std::string SETTING_VIDEOLIBRARY_CLEANUP;
  static const std::string SETTING_VIDEOLIBRARY_EXPORT;
  static const std::string SETTING_VIDEOLIBRARY_IMPORT;
  static const std::string SETTING_VIDEOLIBRARY_SHOWEMPTYTVSHOWS;
  static const std::string SETTING_LOCALE_AUDIOLANGUAGE;
  static const std::string SETTING_VIDEOPLAYER_PREFERDEFAULTFLAG;
  static const std::string SETTING_VIDEOPLAYER_AUTOPLAYNEXTITEM;
  static const std::string SETTING_VIDEOPLAYER_SEEKSTEPS;
  static const std::string SETTING_VIDEOPLAYER_SEEKDELAY;
  static const std::string SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE;
  static const std::string SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK;
  static const std::string SETTING_VIDEOPLAYER_ERRORINASPECT;
  static const std::string SETTING_VIDEOPLAYER_STRETCH43;
  static const std::string SETTING_VIDEOPLAYER_TELETEXTENABLED;
  static const std::string SETTING_VIDEOPLAYER_TELETEXTSCALE;
  static const std::string SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE;
  static const std::string SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP;
  static const std::string SETTING_VIDEOPLAYER_RENDERMETHOD;
  static const std::string SETTING_VIDEOPLAYER_HQSCALERS;
  static const std::string SETTING_VIDEOPLAYER_USEAMCODEC;
  static const std::string SETTING_VIDEOPLAYER_USEAMCODECMPEG2;
  static const std::string SETTING_VIDEOPLAYER_USEAMCODECMPEG4;
  static const std::string SETTING_VIDEOPLAYER_USEAMCODECH264;
  static const std::string SETTING_VIDEOPLAYER_USEMEDIACODEC;
  static const std::string SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE;
  static const std::string SETTING_VIDEOPLAYER_USEVDPAU;
  static const std::string SETTING_VIDEOPLAYER_USEVDPAUMIXER;
  static const std::string SETTING_VIDEOPLAYER_USEVDPAUMPEG2;
  static const std::string SETTING_VIDEOPLAYER_USEVDPAUMPEG4;
  static const std::string SETTING_VIDEOPLAYER_USEVDPAUVC1;
  static const std::string SETTING_VIDEOPLAYER_USEDXVA2;
  static const std::string SETTING_VIDEOPLAYER_USEOMXPLAYER;
  static const std::string SETTING_VIDEOPLAYER_USEVTB;
  static const std::string SETTING_VIDEOPLAYER_USEMMAL;
  static const std::string SETTING_VIDEOPLAYER_USEPRIMEDECODER;
  static const std::string SETTING_VIDEOPLAYER_USESTAGEFRIGHT;
  static const std::string SETTING_VIDEOPLAYER_LIMITGUIUPDATE;
  static const std::string SETTING_VIDEOPLAYER_SUPPORTMVC;
  static const std::string SETTING_MYVIDEOS_SELECTACTION;
  static const std::string SETTING_MYVIDEOS_USETAGS;
  static const std::string SETTING_MYVIDEOS_EXTRACTFLAGS;
  static const std::string SETTING_MYVIDEOS_EXTRACTCHAPTERTHUMBS;
  static const std::string SETTING_MYVIDEOS_REPLACELABELS;
  static const std::string SETTING_MYVIDEOS_EXTRACTTHUMB;
  static const std::string SETTING_MYVIDEOS_STACKVIDEOS;
  static const std::string SETTING_LOCALE_SUBTITLELANGUAGE;
  static const std::string SETTING_SUBTITLES_PARSECAPTIONS;
  static const std::string SETTING_SUBTITLES_ALIGN;
  static const std::string SETTING_SUBTITLES_STEREOSCOPICDEPTH;
  static const std::string SETTING_SUBTITLES_FONT;
  static const std::string SETTING_SUBTITLES_HEIGHT;
  static const std::string SETTING_SUBTITLES_STYLE;
  static const std::string SETTING_SUBTITLES_COLOR;
  static const std::string SETTING_SUBTITLES_BGCOLOR;
  static const std::string SETTING_SUBTITLES_BGOPACITY;
  static const std::string SETTING_SUBTITLES_CHARSET;
  static const std::string SETTING_SUBTITLES_OVERRIDEASSFONTS;
  static const std::string SETTING_SUBTITLES_LANGUAGES;
  static const std::string SETTING_SUBTITLES_STORAGEMODE;
  static const std::string SETTING_SUBTITLES_CUSTOMPATH;
  static const std::string SETTING_SUBTITLES_PAUSEONSEARCH;
  static const std::string SETTING_SUBTITLES_DOWNLOADFIRST;
  static const std::string SETTING_SUBTITLES_TV;
  static const std::string SETTING_SUBTITLES_MOVIE;
  static const std::string SETTING_DVDS_AUTORUN;
  static const std::string SETTING_DVDS_PLAYERREGION;
  static const std::string SETTING_DVDS_AUTOMENU;
  static const std::string SETTING_DISC_PLAYBACK;
  static const std::string SETTING_BLURAY_PLAYERREGION;
  static const std::string SETTING_ACCESSIBILITY_AUDIOVISUAL;
  static const std::string SETTING_ACCESSIBILITY_AUDIOHEARING;
  static const std::string SETTING_ACCESSIBILITY_SUBHEARING;
  static const std::string SETTING_SCRAPERS_MOVIESDEFAULT;
  static const std::string SETTING_SCRAPERS_TVSHOWSDEFAULT;
  static const std::string SETTING_SCRAPERS_MUSICVIDEOSDEFAULT;
  static const std::string SETTING_PVRMANAGER_PRESELECTPLAYINGCHANNEL;
  static const std::string SETTING_PVRMANAGER_SYNCCHANNELGROUPS;
  static const std::string SETTING_PVRMANAGER_BACKENDCHANNELORDER;
  static const std::string SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERS;
  static const std::string SETTING_PVRMANAGER_CLIENTPRIORITIES;
  static const std::string SETTING_PVRMANAGER_CHANNELMANAGER;
  static const std::string SETTING_PVRMANAGER_GROUPMANAGER;
  static const std::string SETTING_PVRMANAGER_CHANNELSCAN;
  static const std::string SETTING_PVRMANAGER_RESETDB;
  static const std::string SETTING_PVRMENU_DISPLAYCHANNELINFO;
  static const std::string SETTING_PVRMENU_CLOSECHANNELOSDONSWITCH;
  static const std::string SETTING_PVRMENU_ICONPATH;
  static const std::string SETTING_PVRMENU_SEARCHICONS;
  static const std::string SETTING_EPG_PAST_DAYSTODISPLAY;
  static const std::string SETTING_EPG_FUTURE_DAYSTODISPLAY;
  static const std::string SETTING_EPG_SELECTACTION;
  static const std::string SETTING_EPG_HIDENOINFOAVAILABLE;
  static const std::string SETTING_EPG_EPGUPDATE;
  static const std::string SETTING_EPG_PREVENTUPDATESWHILEPLAYINGTV;
  static const std::string SETTING_EPG_IGNOREDBFORCLIENT;
  static const std::string SETTING_EPG_RESETEPG;
  static const std::string SETTING_PVRPLAYBACK_SWITCHTOFULLSCREEN;
  static const std::string SETTING_PVRPLAYBACK_SIGNALQUALITY;
  static const std::string SETTING_PVRPLAYBACK_CONFIRMCHANNELSWITCH;
  static const std::string SETTING_PVRPLAYBACK_CHANNELENTRYTIMEOUT;
  static const std::string SETTING_PVRPLAYBACK_DELAYMARKLASTWATCHED;
  static const std::string SETTING_PVRPLAYBACK_FPS;
  static const std::string SETTING_PVRRECORD_INSTANTRECORDACTION;
  static const std::string SETTING_PVRRECORD_INSTANTRECORDTIME;
  static const std::string SETTING_PVRRECORD_MARGINSTART;
  static const std::string SETTING_PVRRECORD_MARGINEND;
  static const std::string SETTING_PVRRECORD_TIMERNOTIFICATIONS;
  static const std::string SETTING_PVRRECORD_GROUPRECORDINGS;
  static const std::string SETTING_PVRPOWERMANAGEMENT_ENABLED;
  static const std::string SETTING_PVRPOWERMANAGEMENT_BACKENDIDLETIME;
  static const std::string SETTING_PVRPOWERMANAGEMENT_SETWAKEUPCMD;
  static const std::string SETTING_PVRPOWERMANAGEMENT_PREWAKEUP;
  static const std::string SETTING_PVRPOWERMANAGEMENT_DAILYWAKEUP;
  static const std::string SETTING_PVRPOWERMANAGEMENT_DAILYWAKEUPTIME;
  static const std::string SETTING_PVRPARENTAL_ENABLED;
  static const std::string SETTING_PVRPARENTAL_PIN;
  static const std::string SETTING_PVRPARENTAL_DURATION;
  static const std::string SETTING_PVRCLIENT_MENUHOOK;
  static const std::string SETTING_PVRTIMERS_HIDEDISABLEDTIMERS;
  static const std::string SETTING_MUSICLIBRARY_SHOWCOMPILATIONARTISTS;
  static const std::string SETTING_MUSICLIBRARY_USEARTISTSORTNAME;
  static const std::string SETTING_MUSICLIBRARY_DOWNLOADINFO;
  static const std::string SETTING_MUSICLIBRARY_ARTISTSFOLDER;
  static const std::string SETTING_MUSICLIBRARY_PREFERONLINEALBUMART;
  static const std::string SETTING_MUSICLIBRARY_ALBUMSSCRAPER;
  static const std::string SETTING_MUSICLIBRARY_ARTISTSSCRAPER;
  static const std::string SETTING_MUSICLIBRARY_OVERRIDETAGS;
  static const std::string SETTING_MUSICLIBRARY_SHOWALLITEMS;
  static const std::string SETTING_MUSICLIBRARY_UPDATEONSTARTUP;
  static const std::string SETTING_MUSICLIBRARY_BACKGROUNDUPDATE;
  static const std::string SETTING_MUSICLIBRARY_CLEANUP;
  static const std::string SETTING_MUSICLIBRARY_EXPORT;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_FILETYPE;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_FOLDER;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_ITEMS;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_UNSCRAPED;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_OVERWRITE;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_ARTWORK;
  static const std::string SETTING_MUSICLIBRARY_EXPORT_SKIPNFO;
  static const std::string SETTING_MUSICLIBRARY_IMPORT;
  static const std::string SETTING_MUSICPLAYER_AUTOPLAYNEXTITEM;
  static const std::string SETTING_MUSICPLAYER_QUEUEBYDEFAULT;
  static const std::string SETTING_MUSICPLAYER_SEEKSTEPS;
  static const std::string SETTING_MUSICPLAYER_SEEKDELAY;
  static const std::string SETTING_MUSICPLAYER_REPLAYGAINTYPE;
  static const std::string SETTING_MUSICPLAYER_REPLAYGAINPREAMP;
  static const std::string SETTING_MUSICPLAYER_REPLAYGAINNOGAINPREAMP;
  static const std::string SETTING_MUSICPLAYER_REPLAYGAINAVOIDCLIPPING;
  static const std::string SETTING_MUSICPLAYER_CROSSFADE;
  static const std::string SETTING_MUSICPLAYER_CROSSFADEALBUMTRACKS;
  static const std::string SETTING_MUSICPLAYER_VISUALISATION;
  static const std::string SETTING_MUSICFILES_USETAGS;
  static const std::string SETTING_MUSICFILES_TRACKFORMAT;
  static const std::string SETTING_MUSICFILES_NOWPLAYINGTRACKFORMAT;
  static const std::string SETTING_MUSICFILES_LIBRARYTRACKFORMAT;
  static const std::string SETTING_MUSICFILES_FINDREMOTETHUMBS;
  static const std::string SETTING_AUDIOCDS_AUTOACTION;
  static const std::string SETTING_AUDIOCDS_USECDDB;
  static const std::string SETTING_AUDIOCDS_RECORDINGPATH;
  static const std::string SETTING_AUDIOCDS_TRACKPATHFORMAT;
  static const std::string SETTING_AUDIOCDS_ENCODER;
  static const std::string SETTING_AUDIOCDS_SETTINGS;
  static const std::string SETTING_AUDIOCDS_EJECTONRIP;
  static const std::string SETTING_MYMUSIC_SONGTHUMBINVIS;
  static const std::string SETTING_MYMUSIC_DEFAULTLIBVIEW;
  static const std::string SETTING_PICTURES_USETAGS;
  static const std::string SETTING_PICTURES_GENERATETHUMBS;
  static const std::string SETTING_PICTURES_SHOWVIDEOS;
  static const std::string SETTING_PICTURES_DISPLAYRESOLUTION;
  static const std::string SETTING_SLIDESHOW_STAYTIME;
  static const std::string SETTING_SLIDESHOW_DISPLAYEFFECTS;
  static const std::string SETTING_SLIDESHOW_SHUFFLE;
  static const std::string SETTING_SLIDESHOW_HIGHQUALITYDOWNSCALING;
  static const std::string SETTING_WEATHER_CURRENTLOCATION;
  static const std::string SETTING_WEATHER_ADDON;
  static const std::string SETTING_WEATHER_ADDONSETTINGS;
  static const std::string SETTING_SERVICES_DEVICENAME;
  static const std::string SETTING_SERVICES_DEVICEUUID;
  static const std::string SETTING_SERVICES_UPNP;
  static const std::string SETTING_SERVICES_UPNPSERVER;
  static const std::string SETTING_SERVICES_UPNPANNOUNCE;
  static const std::string SETTING_SERVICES_UPNPLOOKFOREXTERNALSUBTITLES;
  static const std::string SETTING_SERVICES_UPNPCONTROLLER;
  static const std::string SETTING_SERVICES_UPNPRENDERER;
  static const std::string SETTING_SERVICES_WEBSERVER;
  static const std::string SETTING_SERVICES_WEBSERVERPORT;
  static const std::string SETTING_SERVICES_WEBSERVERUSERNAME;
  static const std::string SETTING_SERVICES_WEBSERVERPASSWORD;
  static const std::string SETTING_SERVICES_WEBSERVERSSL;
  static const std::string SETTING_SERVICES_WEBSKIN;
  static const std::string SETTING_SERVICES_ESENABLED;
  static const std::string SETTING_SERVICES_ESPORT;
  static const std::string SETTING_SERVICES_ESPORTRANGE;
  static const std::string SETTING_SERVICES_ESMAXCLIENTS;
  static const std::string SETTING_SERVICES_ESALLINTERFACES;
  static const std::string SETTING_SERVICES_ESINITIALDELAY;
  static const std::string SETTING_SERVICES_ESCONTINUOUSDELAY;
  static const std::string SETTING_SERVICES_ZEROCONF;
  static const std::string SETTING_SERVICES_AIRPLAY;
  static const std::string SETTING_SERVICES_AIRPLAYVOLUMECONTROL;
  static const std::string SETTING_SERVICES_USEAIRPLAYPASSWORD;
  static const std::string SETTING_SERVICES_AIRPLAYPASSWORD;
  static const std::string SETTING_SERVICES_AIRPLAYVIDEOSUPPORT;
  static const std::string SETTING_SMB_WINSSERVER;
  static const std::string SETTING_SMB_WORKGROUP;
  static const std::string SETTING_SMB_MINPROTOCOL;
  static const std::string SETTING_SMB_MAXPROTOCOL;
  static const std::string SETTING_SMB_LEGACYSECURITY;
  static const std::string SETTING_VIDEOSCREEN_MONITOR;
  static const std::string SETTING_VIDEOSCREEN_SCREEN;
  static const std::string SETTING_VIDEOSCREEN_WHITELIST;
  static const std::string SETTING_VIDEOSCREEN_RESOLUTION;
  static const std::string SETTING_VIDEOSCREEN_SCREENMODE;
  static const std::string SETTING_VIDEOSCREEN_FAKEFULLSCREEN;
  static const std::string SETTING_VIDEOSCREEN_BLANKDISPLAYS;
  static const std::string SETTING_VIDEOSCREEN_STEREOSCOPICMODE;
  static const std::string SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE;
  static const std::string SETTING_VIDEOSCREEN_NOOFBUFFERS;
  static const std::string SETTING_VIDEOSCREEN_3DLUT;
  static const std::string SETTING_VIDEOSCREEN_DISPLAYPROFILE;
  static const std::string SETTING_VIDEOSCREEN_GUICALIBRATION;
  static const std::string SETTING_VIDEOSCREEN_TESTPATTERN;
  static const std::string SETTING_VIDEOSCREEN_LIMITEDRANGE;
  static const std::string SETTING_VIDEOSCREEN_FRAMEPACKING;
  static const std::string SETTING_AUDIOOUTPUT_AUDIODEVICE;
  static const std::string SETTING_AUDIOOUTPUT_CHANNELS;
  static const std::string SETTING_AUDIOOUTPUT_CONFIG;
  static const std::string SETTING_AUDIOOUTPUT_SAMPLERATE;
  static const std::string SETTING_AUDIOOUTPUT_STEREOUPMIX;
  static const std::string SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME;
  static const std::string SETTING_AUDIOOUTPUT_PROCESSQUALITY;
  static const std::string SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD;
  static const std::string SETTING_AUDIOOUTPUT_STREAMSILENCE;
  static const std::string SETTING_AUDIOOUTPUT_STREAMNOISE;
  static const std::string SETTING_AUDIOOUTPUT_GUISOUNDMODE;
  static const std::string SETTING_AUDIOOUTPUT_PASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE;
  static const std::string SETTING_AUDIOOUTPUT_AC3PASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_AC3TRANSCODE;
  static const std::string SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_DTSPASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH;
  static const std::string SETTING_AUDIOOUTPUT_VOLUMESTEPS;
  static const std::string SETTING_INPUT_PERIPHERALS;
  static const std::string SETTING_INPUT_PERIPHERALLIBRARIES;
  static const std::string SETTING_INPUT_ENABLEMOUSE;
  static const std::string SETTING_INPUT_ASKNEWCONTROLLERS;
  static const std::string SETTING_INPUT_CONTROLLERCONFIG;
  static const std::string SETTING_INPUT_RUMBLENOTIFY;
  static const std::string SETTING_INPUT_TESTRUMBLE;
  static const std::string SETTING_INPUT_CONTROLLERPOWEROFF;
  static const std::string SETTING_INPUT_APPLEREMOTEMODE;
  static const std::string SETTING_INPUT_APPLEREMOTEALWAYSON;
  static const std::string SETTING_INPUT_APPLEREMOTESEQUENCETIME;
  static const std::string SETTING_NETWORK_USEHTTPPROXY;
  static const std::string SETTING_NETWORK_HTTPPROXYTYPE;
  static const std::string SETTING_NETWORK_HTTPPROXYSERVER;
  static const std::string SETTING_NETWORK_HTTPPROXYPORT;
  static const std::string SETTING_NETWORK_HTTPPROXYUSERNAME;
  static const std::string SETTING_NETWORK_HTTPPROXYPASSWORD;
  static const std::string SETTING_NETWORK_BANDWIDTH;
  static const std::string SETTING_POWERMANAGEMENT_DISPLAYSOFF;
  static const std::string SETTING_POWERMANAGEMENT_SHUTDOWNTIME;
  static const std::string SETTING_POWERMANAGEMENT_SHUTDOWNSTATE;
  static const std::string SETTING_POWERMANAGEMENT_WAKEONACCESS;
  static const std::string SETTING_POWERMANAGEMENT_WAITFORNETWORK;
  static const std::string SETTING_DEBUG_SHOWLOGINFO;
  static const std::string SETTING_DEBUG_EXTRALOGGING;
  static const std::string SETTING_DEBUG_SETEXTRALOGLEVEL;
  static const std::string SETTING_DEBUG_SCREENSHOTPATH;
  static const std::string SETTING_EVENTLOG_ENABLED;
  static const std::string SETTING_EVENTLOG_ENABLED_NOTIFICATIONS;
  static const std::string SETTING_EVENTLOG_SHOW;
  static const std::string SETTING_MASTERLOCK_LOCKCODE;
  static const std::string SETTING_MASTERLOCK_STARTUPLOCK;
  static const std::string SETTING_MASTERLOCK_MAXRETRIES;
  static const std::string SETTING_COREELEC_AMLOGIC_DEINTERLACING;
  static const std::string SETTING_COREELEC_AMLOGIC_NOISEREDUCTION;
  static const std::string SETTING_COREELEC_AMLOGIC_LIMIT8BIT;
  static const std::string SETTING_COREELEC_AMLOGIC_FORCE422;
  static const std::string SETTING_COREELEC_AMLOGIC_HEVCWORKAROUND;
  static const std::string SETTING_COREELEC_AMLOGIC_MUTEHDMIAUDIO;
  static const std::string SETTING_CACHE_HARDDISK;
  static const std::string SETTING_CACHEVIDEO_DVDROM;
  static const std::string SETTING_CACHEVIDEO_LAN;
  static const std::string SETTING_CACHEVIDEO_INTERNET;
  static const std::string SETTING_CACHEAUDIO_DVDROM;
  static const std::string SETTING_CACHEAUDIO_LAN;
  static const std::string SETTING_CACHEAUDIO_INTERNET;
  static const std::string SETTING_CACHEDVD_DVDROM;
  static const std::string SETTING_CACHEDVD_LAN;
  static const std::string SETTING_CACHEUNKNOWN_INTERNET;
  static const std::string SETTING_SYSTEM_PLAYLISTSPATH;
  static const std::string SETTING_ADDONS_AUTOUPDATES;
  static const std::string SETTING_ADDONS_NOTIFICATIONS;
  static const std::string SETTING_ADDONS_SHOW_RUNNING;
  static const std::string SETTING_ADDONS_MANAGE_DEPENDENCIES;
  static const std::string SETTING_ADDONS_ALLOW_UNKNOWN_SOURCES;
  static const std::string SETTING_GENERAL_ADDONFOREIGNFILTER;
  static const std::string SETTING_GENERAL_ADDONBROKENFILTER;
  static const std::string SETTING_SOURCE_VIDEOS;
  static const std::string SETTING_SOURCE_MUSIC;
  static const std::string SETTING_SOURCE_PICTURES;
  // values for SETTING_VIDEOLIBRARY_SHOWUNWATCHEDPLOTS
  static const int VIDEOLIBRARY_PLOTS_SHOW_UNWATCHED_MOVIES = 0;
  static const int VIDEOLIBRARY_PLOTS_SHOW_UNWATCHED_TVSHOWEPISODES = 1;
  static const int VIDEOLIBRARY_THUMB_SHOW_UNWATCHED_EPISODE = 2;

  /*!
   \brief Creates a new settings wrapper around a new settings manager.

   For access to the "global" settings wrapper the static GetInstance() method should
   be used.
   */
  CSettings() = default;
  ~CSettings() override = default;

  CSettingsManager* GetSettingsManager() const { return m_settingsManager; }

  // specialization of CSettingsBase
  bool Initialize() override;

  // implementations of CSettingsBase
  bool Load() override;
  bool Save() override;

  /*!
   \brief Loads setting values from the given (XML) file.

   \param file Path to an XML file containing setting values
   \return True if the setting values were successfully loaded, false otherwise
   */
  bool Load(const std::string &file);
  /*!
  \brief Loads setting values from the given XML element.

  \param root XML element containing setting values
  \return True if the setting values were successfully loaded, false otherwise
  */
  bool Load(const TiXmlElement *root) { bool updated; return CSettingsBase::LoadValuesFromXml(root, updated); }
  /*!
   \brief Loads setting values from the given XML element.

   \param root XML element containing setting values
   \param hide Whether to hide the loaded settings or not
   \return True if the setting values were successfully loaded, false otherwise
   */
  bool LoadHidden(const TiXmlElement *root) { return CSettingsBase::LoadHiddenValuesFromXml(root); }

  /*!
   \brief Saves the setting values to the given (XML) file.

   \param file Path to an XML file
   \return True if the setting values were successfully saved, false otherwise
   */
  bool Save(const std::string &file);

  /*!
   \brief Loads the setting being represented by the given XML node with the
   given identifier.

   \param node XML node representing the setting to load
   \param settingId Setting identifier
   \return True if the setting was successfully loaded from the given XML node, false otherwise
   */
  bool LoadSetting(const TiXmlNode *node, const std::string &settingId);

  // overwrite (not override) from CSettingsBase
  bool GetBool(const std::string& id) const;

protected:
  // specializations of CSettingsBase
  void InitializeSettingTypes() override;
  void InitializeControls() override;
  void InitializeOptionFillers() override;
  void UninitializeOptionFillers() override;
  void InitializeConditions() override;
  void UninitializeConditions() override;
  void InitializeVisibility() override;
  void InitializeDefaults() override;
  void InitializeISettingsHandlers() override;
  void UninitializeISettingsHandlers() override;
  void InitializeISubSettings() override;
  void UninitializeISubSettings() override;
  void InitializeISettingCallbacks() override;
  void UninitializeISettingCallbacks() override;

  // implementation of CSettingsBase
  bool InitializeDefinitions() override;

private:
  CSettings(const CSettings&) = delete;
  CSettings const& operator=(CSettings const&) = delete;

  bool Initialize(const std::string &file);
  bool Reset();
};
