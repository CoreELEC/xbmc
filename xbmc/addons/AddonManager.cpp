/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AddonManager.h"

#include "LangInfo.h"
#include "ServiceBroker.h"
#include "events/AddonManagementEvent.h"
#include "events/EventLog.h"
#include "events/NotificationEvent.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XMLUtils.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

using namespace XFILE;

namespace ADDON
{

void cp_fatalErrorHandler(const char *msg);
void cp_logger(cp_log_severity_t level, const char *msg, const char *apid, void *user_data);

namespace {
// Note that all of these characters are url-safe
const std::string VALID_ADDON_IDENTIFIER_CHARACTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_@!$";
}

/**********************************************************
 * CAddonMgr
 *
 */

std::map<TYPE, IAddonMgrCallback*> CAddonMgr::m_managers;

static cp_extension_t* GetFirstExtPoint(const cp_plugin_info_t* addon, TYPE type)
{
  for (unsigned int i = 0; i < addon->num_extensions; ++i)
  {
    cp_extension_t* ext = &addon->extensions[i];
    if (strcmp(ext->ext_point_id, "kodi.addon.metadata") == 0 || strcmp(ext->ext_point_id, "xbmc.addon.metadata") == 0)
      continue;

    if (type == ADDON_UNKNOWN)
      return ext;

    if (type == CAddonInfo::TranslateType(ext->ext_point_id))
      return ext;
  }
  return nullptr;
}

AddonPtr CAddonMgr::Factory(const cp_plugin_info_t* plugin, TYPE type)
{
  CAddonBuilder builder;
  if (Factory(plugin, type, builder))
    return builder.Build();
  return nullptr;
}

bool CAddonMgr::Factory(const cp_plugin_info_t* plugin, TYPE type, CAddonBuilder& builder, bool ignoreExtensions/* = false*/, const CRepository::DirInfo& repo)
{
  if (!plugin || !plugin->identifier)
    return false;

  // Check addon identifier for forbidden characters
  // The identifier is used e.g. in URLs so we shouldn't allow just
  // any character to go through.
  if (std::string{plugin->identifier}.find_first_not_of(VALID_ADDON_IDENTIFIER_CHARACTERS) != std::string::npos)
  {
    CLog::Log(LOGERROR, "Plugin identifier {} is invalid", plugin->identifier);
    return false;
  }

  if (!PlatformSupportsAddon(plugin))
    return false;

  if (!ignoreExtensions)
  {
    cp_extension_t* ext = GetFirstExtPoint(plugin, type);

    if (ext == nullptr && type != ADDON_UNKNOWN)
      return false; // no extension point satisfies the type requirement

    if (ext)
    {
      builder.SetType(CAddonInfo::TranslateType(ext->ext_point_id));
      builder.SetExtPoint(ext);

      auto libname = CServiceBroker::GetAddonMgr().GetExtValue(ext->configuration, "@library");
      if (libname.empty())
        libname = CServiceBroker::GetAddonMgr().GetPlatformLibraryName(ext->configuration);
      builder.SetLibName(libname);
    }
  }

  FillCpluffMetadata(plugin, builder, repo);
  return true;
}

void CAddonMgr::FillCpluffMetadata(const cp_plugin_info_t* plugin, CAddonBuilder& builder, const CRepository::DirInfo& repo)
{
  builder.SetId(plugin->identifier);

  if (plugin->version)
    builder.SetVersion(AddonVersion(plugin->version));

  if (plugin->abi_bw_compatibility)
    builder.SetMinVersion(AddonVersion(plugin->abi_bw_compatibility));

  if (plugin->name)
    builder.SetName(plugin->name);

  if (plugin->provider_name)
    builder.SetAuthor(plugin->provider_name);

  {
    std::vector<DependencyInfo> dependencies;
    for (unsigned int i = 0; i < plugin->num_imports; ++i)
    {
      if (plugin->imports[i].plugin_id)
      {
        std::string id(plugin->imports[i].plugin_id);
        AddonVersion version(plugin->imports[i].version ? plugin->imports[i].version : "0.0.0");
        dependencies.emplace_back(id, version, plugin->imports[i].optional != 0);
      }
    }
    builder.SetDependencies(std::move(dependencies));
  }

  auto metadata = CServiceBroker::GetAddonMgr().GetExtension(plugin, "xbmc.addon.metadata");
  if (!metadata)
    metadata = CServiceBroker::GetAddonMgr().GetExtension(plugin, "kodi.addon.metadata");

  std::string path;
  if (metadata)
    path = CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "path");

  if (plugin->plugin_path && strcmp(plugin->plugin_path, "") != 0 && strcmp(plugin->plugin_path, "memory") != 0)
    builder.SetPath(plugin->plugin_path);
  else
  {
    if (path.empty())
      builder.SetPath(URIUtils::AddFileToFolder(repo.datadir, plugin->identifier,
        StringUtils::Format("{}-{}.zip", plugin->identifier, builder.GetVersion().asString())));
    else
      builder.SetPath(URIUtils::AddFileToFolder(repo.datadir, path));
  }

  std::string assetBasePath = repo.artdir;
  if (repo.artdir.empty() && plugin->plugin_path)
  {
    // Default for add-on information not loaded from repository
    assetBasePath = plugin->plugin_path;
  }
  else
  {
    if (path.empty())
      assetBasePath = URIUtils::AddFileToFolder(assetBasePath, plugin->identifier);
    else
      assetBasePath = URIUtils::AddFileToFolder(assetBasePath, StringUtils::Split(path, '/')[0]);
  }

  if (!assetBasePath.empty())
  {
    //backwards compatibility
    std::string icon = metadata && CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "noicon") == "true" ? "" : "icon.png";
    std::string fanart = metadata && CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "nofanart") == "true" ? "" : "fanart.jpg";
    if (!icon.empty())
      builder.SetIcon(URIUtils::AddFileToFolder(assetBasePath, icon));
    if (!fanart.empty())
      builder.SetArt("fanart", URIUtils::AddFileToFolder(assetBasePath, fanart));
  }

  if (metadata)
  {
    builder.SetSummary(CServiceBroker::GetAddonMgr().GetTranslatedString(metadata->configuration, "summary"));
    builder.SetDescription(CServiceBroker::GetAddonMgr().GetTranslatedString(metadata->configuration, "description"));
    builder.SetDisclaimer(CServiceBroker::GetAddonMgr().GetTranslatedString(metadata->configuration, "disclaimer"));
    builder.SetChangelog(CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "news"));
    builder.SetLicense(CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "license"));
    builder.SetPackageSize(StringUtils::ToUint64(CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "size"), 0));

    {
      InfoMap extrainfo;

      std::string metaString = CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "language");
      if (!metaString.empty())
        extrainfo.insert(std::make_pair("language", metaString));

      metaString = CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "reuselanguageinvoker");
      if (!metaString.empty())
        extrainfo.insert(std::make_pair("reuselanguageinvoker", metaString));

      if (!extrainfo.empty())
        builder.SetExtrainfo(std::move(extrainfo));
    }

    builder.SetBroken(CServiceBroker::GetAddonMgr().GetExtValue(metadata->configuration, "broken"));

    if (!assetBasePath.empty())
    {
      auto assets = CServiceBroker::GetAddonMgr().GetExtElement(metadata->configuration, "assets");
      if (assets)
      {
        builder.SetIcon("");
        builder.SetArt("fanart", "");
        std::string icon = CServiceBroker::GetAddonMgr().GetExtValue(assets, "icon");
        if (!icon.empty())
          icon = URIUtils::AddFileToFolder(assetBasePath, icon);
        builder.SetIcon(icon);

        std::map<std::string, std::string> art;
        std::array<std::string, 3> artTypes{{"fanart", "banner", "clearlogo"}};
        for (auto type : artTypes)
        {
          auto value = CServiceBroker::GetAddonMgr().GetExtValue(assets, type.c_str());
          if (!value.empty())
          {
            value = URIUtils::AddFileToFolder(assetBasePath, value);
            builder.SetArt(type, value);
          }
        }

        std::vector<std::string> screenshots;
        ELEMENTS elements;
        if (CServiceBroker::GetAddonMgr().GetExtElements(assets, "screenshot", elements))
        {
          for (const auto& elem : elements)
          {
            if (elem->value && strcmp(elem->value, "") != 0)
              screenshots.emplace_back(URIUtils::AddFileToFolder(assetBasePath, elem->value));
          }
        }
        builder.SetScreenshots(std::move(screenshots));
      }
    }
  }
}

static bool LoadManifest(std::set<std::string>& system, std::set<std::string>& optional)
{
  CXBMCTinyXML doc;
  if (!doc.LoadFile("special://xbmc/system/addon-manifest.xml"))
  {
    CLog::Log(LOGERROR, "ADDONS: manifest missing");
    return false;
  }

  auto root = doc.RootElement();
  if (!root || root->ValueStr() != "addons")
  {
    CLog::Log(LOGERROR, "ADDONS: malformed manifest");
    return false;
  }

  auto elem = root->FirstChildElement("addon");
  while (elem)
  {
    if (elem->FirstChild())
    {
      if (XMLUtils::GetAttribute(elem, "optional") == "true")
        optional.insert(elem->FirstChild()->ValueStr());
      else
        system.insert(elem->FirstChild()->ValueStr());
    }
    elem = elem->NextSiblingElement("addon");
  }
  return true;
}

CAddonMgr::~CAddonMgr()
{
  DeInit();
}

IAddonMgrCallback* CAddonMgr::GetCallbackForType(TYPE type)
{
  if (m_managers.find(type) == m_managers.end())
    return NULL;
  else
    return m_managers[type];
}

bool CAddonMgr::RegisterAddonMgrCallback(const TYPE type, IAddonMgrCallback* cb)
{
  if (cb == NULL)
    return false;

  m_managers.erase(type);
  m_managers[type] = cb;

  return true;
}

void CAddonMgr::UnregisterAddonMgrCallback(TYPE type)
{
  m_managers.erase(type);
}

bool CAddonMgr::Init()
{
  CSingleLock lock(m_critSection);

  cp_set_fatal_error_handler(cp_fatalErrorHandler);

  cp_status_t status;
  status = cp_init();
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_init() returned status: %i", status);
    return false;
  }

  //! @todo could separate addons into different contexts would allow partial unloading of addon framework
  m_cp_context = cp_create_context(&status);
  assert(m_cp_context);
  status = cp_register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://home/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = cp_register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://xbmc/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = cp_register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://xbmcbin/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = cp_register_logger(m_cp_context, cp_logger, this, CP_LOG_WARNING);
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_logger() returned status: %i", status);
    return false;
  }

  if (!LoadManifest(m_systemAddons, m_optionalAddons))
  {
    CLog::Log(LOGERROR, "ADDONS: Failed to read manifest");
    return false;
  }

 if (!m_database.Open())
   CLog::Log(LOGFATAL, "ADDONS: Failed to open database");

  FindAddons();

  //Ensure required add-ons are installed and enabled
  for (const auto& id : m_systemAddons)
  {
    AddonPtr addon;
    if (!GetAddon(id, addon, ADDON_UNKNOWN))
    {
      CLog::Log(LOGFATAL, "addon '%s' not installed or not enabled.", id.c_str());
      return false;
    }
  }

  return true;
}

void CAddonMgr::DeInit()
{
  cp_destroy_context(m_cp_context);
  m_database.Close();
}

bool CAddonMgr::HasAddons(const TYPE &type)
{
  VECADDONS addons;
  return GetAddonsInternal(type, addons, true);
}

bool CAddonMgr::HasInstalledAddons(const TYPE &type)
{
  VECADDONS addons;
  return GetAddonsInternal(type, addons, false);
}

void CAddonMgr::AddToUpdateableAddons(AddonPtr &pAddon)
{
  CSingleLock lock(m_critSection);
  m_updateableAddons.push_back(pAddon);
}

void CAddonMgr::RemoveFromUpdateableAddons(AddonPtr &pAddon)
{
  CSingleLock lock(m_critSection);
  VECADDONS::iterator it = std::find(m_updateableAddons.begin(), m_updateableAddons.end(), pAddon);

  if(it != m_updateableAddons.end())
  {
    m_updateableAddons.erase(it);
  }
}

struct AddonIdFinder
{
    explicit AddonIdFinder(const std::string& id)
      : m_id(id)
    {}

    bool operator()(const AddonPtr& addon)
    {
      return m_id == addon->ID();
    }
    private:
    std::string m_id;
};

bool CAddonMgr::ReloadSettings(const std::string &id)
{
  CSingleLock lock(m_critSection);
  VECADDONS::iterator it = std::find_if(m_updateableAddons.begin(), m_updateableAddons.end(), AddonIdFinder(id));

  if( it != m_updateableAddons.end())
  {
    return (*it)->ReloadSettings();
  }
  return false;
}

VECADDONS CAddonMgr::GetAvailableUpdates()
{
  CSingleLock lock(m_critSection);
  auto start = XbmcThreads::SystemClockMillis();

  VECADDONS updates;
  VECADDONS installed;
  GetAddons(installed);
  for (const auto& addon : installed)
  {
    AddonPtr remote;
    if (m_database.GetAddon(addon->ID(), remote) && remote->Version() > addon->Version())
      updates.emplace_back(std::move(remote));
  }
  CLog::Log(LOGDEBUG, "CAddonMgr::GetAvailableUpdates took %i ms", XbmcThreads::SystemClockMillis() - start);
  return updates;
}

bool CAddonMgr::HasAvailableUpdates()
{
  return !GetAvailableUpdates().empty();
}

bool CAddonMgr::GetAddons(VECADDONS& addons)
{
  return GetAddonsInternal(ADDON_UNKNOWN, addons, true);
}

bool CAddonMgr::GetAddons(VECADDONS& addons, const TYPE& type)
{
  return GetAddonsInternal(type, addons, true);
}

bool CAddonMgr::GetInstalledAddons(VECADDONS& addons)
{
  return GetAddonsInternal(ADDON_UNKNOWN, addons, false);
}

bool CAddonMgr::GetInstalledAddons(VECADDONS& addons, const TYPE& type)
{
  return GetAddonsInternal(type, addons, false);
}

bool CAddonMgr::GetDisabledAddons(VECADDONS& addons)
{
  return CAddonMgr::GetDisabledAddons(addons, ADDON_UNKNOWN);
}

bool CAddonMgr::GetDisabledAddons(VECADDONS& addons, const TYPE& type)
{
  VECADDONS all;
  if (GetInstalledAddons(all, type))
  {
    std::copy_if(all.begin(), all.end(), std::back_inserter(addons),
        [this](const AddonPtr& addon){ return IsAddonDisabled(addon->ID()); });
    return true;
  }
  return false;
}

bool CAddonMgr::GetInstallableAddons(VECADDONS& addons)
{
  return GetInstallableAddons(addons, ADDON_UNKNOWN);
}

bool CAddonMgr::GetInstallableAddons(VECADDONS& addons, const TYPE &type)
{
  CSingleLock lock(m_critSection);

  // get all addons
  if (!m_database.GetRepositoryContent(addons))
    return false;

  // go through all addons and remove all that are already installed

  addons.erase(std::remove_if(addons.begin(), addons.end(),
    [this, type](const AddonPtr& addon)
    {
      bool bErase = false;

      // check if the addon matches the provided addon type
      if (type != ADDON::ADDON_UNKNOWN && addon->Type() != type && !addon->IsType(type))
        bErase = true;

      if (!this->CanAddonBeInstalled(addon))
        bErase = true;

      return bErase;
    }), addons.end());

  return true;
}

bool CAddonMgr::FindInstallableById(const std::string& addonId, AddonPtr& result)
{
  VECADDONS versions;
  {
    CSingleLock lock(m_critSection);
    if (!m_database.FindByAddonId(addonId, versions) || versions.empty())
      return false;
  }

  result = *std::max_element(versions.begin(), versions.end(),
      [](const AddonPtr& a, const AddonPtr& b) { return a->Version() < b->Version(); });
  return true;
}

bool CAddonMgr::GetInstalledBinaryAddons(BINARY_ADDON_LIST& binaryAddonList)
{
  CSingleLock lock(m_critSection);
  if (!m_cp_context)
    return false;

  std::vector<CAddonBuilder> builders;
  m_database.GetInstalled(builders);

  for (auto builder : builders)
  {
    BINARY_ADDON_LIST_ENTRY binaryAddon;
    if (GetInstalledBinaryAddon(builder.GetId(), binaryAddon))
      binaryAddonList.push_back(std::move(binaryAddon));
  }

  return !binaryAddonList.empty();
}

bool CAddonMgr::GetInstalledBinaryAddon(const std::string& addonId, BINARY_ADDON_LIST_ENTRY& binaryAddon)
{
  bool ret = false;
  cp_status_t status;

  CSingleLock lock(m_critSection);

  cp_plugin_info_t *cp_addon = cp_get_plugin_info(m_cp_context, addonId.c_str(), &status);
  if (status == CP_OK && cp_addon)
  {
    cp_extension_t* props = GetFirstExtPoint(cp_addon, ADDON_UNKNOWN);
    if (props != nullptr)
    {
      CAddonBuilder builder;
      std::string value = GetPlatformLibraryName(props->plugin->extensions->configuration);
      if (!value.empty() &&
          props->plugin->plugin_path &&
          strcmp(props->plugin->plugin_path, "") != 0 &&
          Factory(cp_addon, ADDON_UNKNOWN, builder, true))
      {
        binaryAddon = BINARY_ADDON_LIST_ENTRY(!IsAddonDisabled(cp_addon->identifier), std::move(builder.GetAddonInfo()));
        ret = true;
      }
    }
    cp_release_info(m_cp_context, cp_addon);
  }

  return ret;
}

bool CAddonMgr::GetAddonsInternal(const TYPE &type, VECADDONS &addons, bool enabledOnly)
{
  CSingleLock lock(m_critSection);
  if (!m_cp_context)
    return false;

  std::vector<CAddonBuilder> builders;
  m_database.GetInstalled(builders);

  for (auto& builder : builders)
  {
    cp_status_t status;
    cp_plugin_info_t* cp_addon = cp_get_plugin_info(m_cp_context, builder.GetId().c_str(), &status);
    if (status == CP_OK && cp_addon)
    {
      if (enabledOnly && IsAddonDisabled(cp_addon->identifier))
      {
        cp_release_info(m_cp_context, cp_addon);
        continue;
      }

      //FIXME: hack for skipping special dependency addons (xbmc.python etc.).
      //Will break if any extension point is added to them
      cp_extension_t *props = GetFirstExtPoint(cp_addon, type);
      if (props == nullptr)
      {
        cp_release_info(m_cp_context, cp_addon);
        continue;
      }

      AddonPtr addon;
      if (Factory(cp_addon, type, builder))
        addon = builder.Build();
      cp_release_info(m_cp_context, cp_addon);

      if (addon)
      {
        // if the addon has a running instance, grab that
        AddonPtr runningAddon = addon->GetRunningInstance();
        if (runningAddon)
          addon = runningAddon;
        addons.emplace_back(std::move(addon));
      }
    }
  }
  return addons.size() > 0;
}

bool CAddonMgr::GetAddon(const std::string &str, AddonPtr &addon, const TYPE &type/*=ADDON_UNKNOWN*/, bool enabledOnly /*= true*/)
{
  CSingleLock lock(m_critSection);

  cp_status_t status;
  cp_plugin_info_t *cpaddon = cp_get_plugin_info(m_cp_context, str.c_str(), &status);
  if (status == CP_OK && cpaddon)
  {
    addon = Factory(cpaddon, type);
    cp_release_info(m_cp_context, cpaddon);

    if (addon)
    {
      if (enabledOnly && IsAddonDisabled(addon->ID()))
        return false;

      // if the addon has a running instance, grab that
      AddonPtr runningAddon = addon->GetRunningInstance();
      if (runningAddon)
        addon = runningAddon;
    }
    return NULL != addon.get();
  }
  if (cpaddon)
    cp_release_info(m_cp_context, cpaddon);

  return false;
}

bool CAddonMgr::HasType(const std::string &id, const TYPE &type)
{
  AddonPtr addon;
  return GetAddon(id, addon, type, false);
}

bool CAddonMgr::FindAddons()
{
  bool result = false;
  CSingleLock lock(m_critSection);
  if (m_cp_context)
  {
    result = true;
    cp_scan_plugins(m_cp_context, CP_SP_UPGRADE);

    //Sync with db
    {
      std::set<std::pair<std::string, std::string>> installed;
      cp_status_t status;
      int n;
      cp_plugin_info_t** cp_addons = cp_get_plugins_info(m_cp_context, &status, &n);
      for (int i = 0; i < n; ++i)
      {
        installed.emplace(cp_addons[i]->identifier, cp_addons[i]->version ? cp_addons[i]->version : "");
      }
      cp_release_info(m_cp_context, cp_addons);
      // Log separately so list is sorted
      for (const auto& installedAddon : installed)
      {
        CLog::Log(LOGNOTICE, "ADDON: {} v{} installed", installedAddon.first, installedAddon.second);
      }

      std::set<std::string> installedIdentifiers;
      std::transform(
          installed.cbegin(), installed.cend(),
          std::inserter(installedIdentifiers, installedIdentifiers.begin()),
          [](const decltype(installed)::value_type& p) { return p.first; }
      );
      m_database.SyncInstalled(installedIdentifiers, m_systemAddons, m_optionalAddons);
    }

    // Reload caches
    std::set<std::string> tmp;
    m_database.GetDisabled(tmp);
    m_disabled = std::move(tmp);

    tmp.clear();
    m_database.GetBlacklisted(tmp);
    m_updateBlacklist = std::move(tmp);
  }

  return result;
}

bool CAddonMgr::UnloadAddon(const std::string& addonId)
{
  CSingleLock lock(m_critSection);

  if (!IsAddonInstalled(addonId))
    return true;

  if (m_cp_context)
  {
    if (cp_uninstall_plugin(m_cp_context, addonId.c_str()) == CP_OK)
    {
      CLog::Log(LOGDEBUG, "CAddonMgr: %s unloaded", addonId.c_str());

      lock.Leave();
      AddonEvents::Unload event(addonId);
      m_unloadEvents.HandleEvent(event);
      return true;
    }
  }
  CLog::Log(LOGERROR, "CAddonMgr: failed to unload %s", addonId.c_str());
  return false;
}

bool CAddonMgr::LoadAddon(const std::string& addonId)
{
  CSingleLock lock(m_critSection);
  if (!m_cp_context)
    return false;

  AddonPtr addon;
  if (GetAddon(addonId, addon, ADDON_UNKNOWN, false))
  {
    return true;
  }

  if (!FindAddons())
  {
    CLog::Log(LOGERROR, "CAddonMgr: could not reload add-on %s. FindAddons failed.", addonId.c_str());
    return false;
  }

  if (!GetAddon(addonId, addon, ADDON_UNKNOWN, false))
  {
    CLog::Log(LOGERROR, "CAddonMgr: could not load add-on %s. No add-on with that ID is installed.", addonId.c_str());
    return false;
  }

  lock.Leave();

  AddonEvents::Load event(addon->ID());
  m_unloadEvents.HandleEvent(event);

  if (IsAddonDisabled(addon->ID()))
  {
    EnableAddon(addon->ID());
    return true;
  }

  m_events.Publish(AddonEvents::ReInstalled(addon->ID()));
  CLog::Log(LOGDEBUG, "CAddonMgr: %s successfully loaded", addon->ID().c_str());
  return true;
}

void CAddonMgr::OnPostUnInstall(const std::string& id)
{
  CSingleLock lock(m_critSection);
  m_disabled.erase(id);
  m_updateBlacklist.erase(id);
  m_events.Publish(AddonEvents::UnInstalled(id));
}

bool CAddonMgr::RemoveFromUpdateBlacklist(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (!IsBlacklisted(id))
    return true;
  return m_database.RemoveAddonFromBlacklist(id) && m_updateBlacklist.erase(id) > 0;
}

bool CAddonMgr::AddToUpdateBlacklist(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (IsBlacklisted(id))
    return true;
  return m_database.BlacklistAddon(id) && m_updateBlacklist.insert(id).second;
}

bool CAddonMgr::IsBlacklisted(const std::string& id) const
{
  CSingleLock lock(m_critSection);
  return m_updateBlacklist.find(id) != m_updateBlacklist.end();
}

void CAddonMgr::UpdateLastUsed(const std::string& id)
{
  auto time = CDateTime::GetCurrentDateTime();
  CJobManager::GetInstance().Submit([this, id, time](){
    {
      CSingleLock lock(m_critSection);
      m_database.SetLastUsed(id, time);
    }
    m_events.Publish(AddonEvents::MetadataChanged(id));
  });
}

static void ResolveDependencies(const std::string& addonId, std::vector<std::string>& needed, std::vector<std::string>& missing)
{
  if (std::find(needed.begin(), needed.end(), addonId) != needed.end())
    return;

  AddonPtr addon;
  if (!CServiceBroker::GetAddonMgr().GetAddon(addonId, addon, ADDON_UNKNOWN, false))
    missing.push_back(addonId);
  else
  {
    needed.push_back(addonId);
    for (const auto& dep : addon->GetDependencies())
      if (!dep.optional)
        ResolveDependencies(dep.id, needed, missing);
  }
}

bool CAddonMgr::DisableAddon(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (!CanAddonBeDisabled(id))
    return false;
  if (m_disabled.find(id) != m_disabled.end())
    return true; //already disabled
  if (!m_database.DisableAddon(id))
    return false;
  if (!m_disabled.insert(id).second)
    return false;

  //success
  CLog::Log(LOGDEBUG, "CAddonMgr: %s disabled", id.c_str());
  AddonPtr addon;
  if (GetAddon(id, addon, ADDON_UNKNOWN, false) && addon != NULL)
  {
    ADDON::LEAddonHook(addon, ADDON::LE_ADDON_DISABLED);
    CServiceBroker::GetEventLog().Add(EventPtr(new CAddonManagementEvent(addon, 24141)));
  }

  m_events.Publish(AddonEvents::Disabled(id));
  return true;
}

bool CAddonMgr::EnableSingle(const std::string& id)
{
  CSingleLock lock(m_critSection);

  if (m_disabled.find(id) == m_disabled.end())
    return true; //already enabled

  AddonPtr addon;
  if (!GetAddon(id, addon, ADDON_UNKNOWN, false) || addon == nullptr)
    return false;

  if (!IsCompatible(*addon))
  {
    CLog::Log(LOGERROR, "Add-on '%s' is not compatible with Kodi", addon->ID().c_str());
    CServiceBroker::GetEventLog().AddWithNotification(EventPtr(new CNotificationEvent(addon->Name(), 24152, EventLevel::Error)));
    return false;
  }

  if (!m_database.DisableAddon(id, false))
    return false;
  m_disabled.erase(id);
  ADDON::LEAddonHook(addon, ADDON::LE_ADDON_ENABLED);

  CServiceBroker::GetEventLog().Add(EventPtr(new CAddonManagementEvent(addon, 24064)));

  CLog::Log(LOGDEBUG, "CAddonMgr: enabled %s", addon->ID().c_str());
  m_events.Publish(AddonEvents::Enabled(id));
  return true;
}

bool CAddonMgr::EnableAddon(const std::string& id)
{
  if (id.empty() || !IsAddonInstalled(id))
    return false;
  std::vector<std::string> needed;
  std::vector<std::string> missing;
  ResolveDependencies(id, needed, missing);
  for (const auto& dep : missing)
    CLog::Log(LOGWARNING, "CAddonMgr: '%s' required by '%s' is missing. Add-on may not function "
        "correctly", dep.c_str(), id.c_str());
  for (auto it = needed.rbegin(); it != needed.rend(); ++it)
    EnableSingle(*it);

  return true;
}

bool CAddonMgr::IsAddonDisabled(const std::string& ID)
{
  CSingleLock lock(m_critSection);
  return m_disabled.find(ID) != m_disabled.end();
}

bool CAddonMgr::CanAddonBeDisabled(const std::string& ID)
{
  if (ID.empty())
    return false;

  CSingleLock lock(m_critSection);
  if (IsSystemAddon(ID))
    return false;

  AddonPtr localAddon;
  // can't disable an addon that isn't installed
  if (!GetAddon(ID, localAddon, ADDON_UNKNOWN, false))
    return false;

  // can't disable an addon that is in use
  if (localAddon->IsInUse())
    return false;

  return true;
}

bool CAddonMgr::CanAddonBeEnabled(const std::string& id)
{
  return !id.empty() && IsAddonInstalled(id);
}

bool CAddonMgr::IsAddonInstalled(const std::string& ID)
{
  AddonPtr tmp;
  return GetAddon(ID, tmp, ADDON_UNKNOWN, false);
}

bool CAddonMgr::CanAddonBeInstalled(const AddonPtr& addon)
{
  return addon != nullptr && !addon->IsBroken() && !IsAddonInstalled(addon->ID());
}

bool CAddonMgr::CanUninstall(const AddonPtr& addon)
{
  return addon && CanAddonBeDisabled(addon->ID()) &&
      !StringUtils::StartsWith(addon->Path(), CSpecialProtocol::TranslatePath("special://xbmc/addons"));
}

bool CAddonMgr::IsSystemAddon(const std::string& id)
{
  CSingleLock lock(m_critSection);
  return std::find(m_systemAddons.begin(), m_systemAddons.end(), id) != m_systemAddons.end();
}

std::string CAddonMgr::GetTranslatedString(const cp_cfg_element_t *root, const char *tag)
{
  if (!root)
    return "";

  std::map<std::string, std::string> translatedValues;
  for (unsigned int i = 0; i < root->num_children; i++)
  {
    const cp_cfg_element_t &child = root->children[i];
    if (strcmp(tag, child.name) == 0)
    {
      // see if we have a "lang" attribute
      //! @bug libcpluff isn't const correct
      const char *lang = cp_lookup_cfg_value(const_cast<cp_cfg_element_t*>(&child), "@lang");
      if (lang != NULL && g_langInfo.GetLocale().Matches(lang))
        translatedValues.insert(std::make_pair(lang, child.value != NULL ? child.value : ""));
      else if (lang == NULL || strcmp(lang, "en") == 0 || strcmp(lang, "en_GB") == 0)
        translatedValues.insert(std::make_pair("en_GB", child.value != NULL ? child.value : ""));
      else if (strcmp(lang, "no") == 0)
        translatedValues.insert(std::make_pair("nb_NO", child.value != NULL ? child.value : ""));
    }
  }

  // put together a list of languages
  std::set<std::string> languages;
  for (auto const& translatedValue : translatedValues)
    languages.insert(translatedValue.first);

  // find the language from the list that matches the current locale best
  std::string matchingLanguage = g_langInfo.GetLocale().FindBestMatch(languages);
  if (matchingLanguage.empty())
    matchingLanguage = "en_GB";

  auto const& translatedValue = translatedValues.find(matchingLanguage);
  if (translatedValue != translatedValues.end())
    return translatedValue->second;

  return "";
}

/*
 * libcpluff interaction
 */

bool CAddonMgr::PlatformSupportsAddon(const cp_plugin_info_t *plugin)
{
  auto *metadata = CServiceBroker::GetAddonMgr().GetExtension(plugin, "xbmc.addon.metadata");
  if (!metadata)
    metadata = CServiceBroker::GetAddonMgr().GetExtension(plugin, "kodi.addon.metadata");

  // if platforms are not specified, assume supported
  if (!metadata)
    return true;

  std::vector<std::string> platforms;
  if (!CServiceBroker::GetAddonMgr().GetExtList(metadata->configuration, "platform", platforms))
    return true;

  if (platforms.empty())
    return true;

  std::vector<std::string> supportedPlatforms = {
    "all",
#if defined(TARGET_ANDROID)
    "android",
#if defined(__ARM_ARCH_7A__)
    "android-armv7",
#elif defined(__aarch64__)
    "android-aarch64",
#elif defined(__i686__)
    "android-i686",
#else
    #warning no architecture dependant platform tag
#endif
#elif defined(TARGET_FREEBSD)
    "freebsd",
    "linux",
#elif defined(TARGET_LINUX)
    "linux",
#elif defined(TARGET_WINDOWS_DESKTOP)
    "windx",
    "windows",
#if defined(_M_IX86)
    "windows-i686",
#elif defined(_M_AMD64)
    "windows-x86_64",
#else
#error no architecture dependant platform tag
#endif
#elif defined(TARGET_WINDOWS_STORE)
    "windowsstore",
#elif defined(TARGET_DARWIN_IOS)
    "ios",
#if defined(__ARM_ARCH_7A__)
    "ios-armv7",
#elif defined(__aarch64__)
    "ios-aarch64",
#else
#warning no architecture dependant platform tag
#endif
#elif defined(TARGET_DARWIN_OSX)
    "osx",
#if defined(__x86_64__)
    "osx64",
    "osx-x86_64",
#elif defined(__i686__)
    "osx-i686",
    "osx32",
#else
#warning no architecture dependant platform tag
#endif
#endif
  };

  return std::find_first_of(platforms.begin(), platforms.end(),
      supportedPlatforms.begin(), supportedPlatforms.end()) != platforms.end();
}

cp_cfg_element_t *CAddonMgr::GetExtElement(cp_cfg_element_t *base, const char *path)
{
  cp_cfg_element_t *element = NULL;
  if (base)
    element = cp_lookup_cfg_element(base, path);
  return element;
}

bool CAddonMgr::GetExtElements(cp_cfg_element_t *base, const char *path, ELEMENTS &elements)
{
  if (!base || !path)
    return false;

  for (unsigned int i = 0; i < base->num_children; i++)
  {
    std::string temp = base->children[i].name;
    if (!temp.compare(path))
      elements.push_back(&base->children[i]);
  }

  return !elements.empty();
}

const cp_extension_t *CAddonMgr::GetExtension(const cp_plugin_info_t *props, const char *extension) const
{
  if (!props)
    return NULL;
  for (unsigned int i = 0; i < props->num_extensions; ++i)
  {
    if (0 == strcmp(props->extensions[i].ext_point_id, extension))
      return &props->extensions[i];
  }
  return NULL;
}

std::string CAddonMgr::GetExtValue(cp_cfg_element_t *base, const char *path) const
{
  const char *value = "";
  if (base && (value = cp_lookup_cfg_value(base, path)))
    return value;
  else
    return "";
}

bool CAddonMgr::GetExtList(cp_cfg_element_t *base, const char *path, std::vector<std::string> &result) const
{
  result.clear();
  if (!base || !path)
    return false;
  const char *all = cp_lookup_cfg_value(base, path);
  if (!all || *all == 0)
    return false;
  StringUtils::Tokenize(all, result, ' ');
  return true;
}

std::string CAddonMgr::GetPlatformLibraryName(cp_cfg_element_t *base) const
{
  std::string libraryName;
#if defined(TARGET_ANDROID)
  libraryName = GetExtValue(base, "@library_android");
#elif defined(TARGET_LINUX) || defined(TARGET_FREEBSD)
#if defined(TARGET_FREEBSD)
  libraryName = GetExtValue(base, "@library_freebsd");
  if (libraryName.empty())
#endif
  libraryName = GetExtValue(base, "@library_linux");
#elif defined(TARGET_WINDOWS_DESKTOP)
  libraryName = GetExtValue(base, "@library_windx");
  if (libraryName.empty())
    libraryName = GetExtValue(base, "@library_windows");
#elif defined(TARGET_WINDOWS_STORE)
  libraryName = GetExtValue(base, "@library_windowsstore");
#elif defined(TARGET_DARWIN)
#if defined(TARGET_DARWIN_IOS)
  libraryName = GetExtValue(base, "@library_ios");
  if (libraryName.empty())
#endif
  libraryName = GetExtValue(base, "@library_osx");
#endif

  return libraryName;
}

bool CAddonMgr::LoadAddonDescription(const std::string &directory, AddonPtr &addon)
{
  auto addonXmlPath = CSpecialProtocol::TranslatePath(URIUtils::AddFileToFolder(directory, "addon.xml"));

  XFILE::CFile file;
  XFILE::auto_buffer buffer;
  if (file.LoadFile(addonXmlPath, buffer) <= 0)
  {
    CLog::Log(LOGERROR, "Failed to read '%s'", addonXmlPath.c_str());
    return false;
  }

  cp_status_t status;
  cp_context_t* context = cp_create_context(&status);
  if (!context)
    return false;

  auto info = cp_load_plugin_descriptor_from_memory(context, buffer.get(), buffer.size(), &status);
  if (info)
  {
    // Correct the path. load_plugin_descriptor_from_memory sets it to 'memory'
    info->plugin_path = static_cast<char*>(malloc(directory.length() + 1));
    strncpy(info->plugin_path, directory.c_str(), directory.length());
    info->plugin_path[directory.length()] = '\0';
    addon = Factory(info, ADDON_UNKNOWN);

    free(info->plugin_path);
    info->plugin_path = nullptr;
    cp_release_info(context, info);
  }
  else
    CLog::Log(LOGERROR, "Failed to parse '%s'", addonXmlPath.c_str());

  cp_destroy_context(context);
  return addon != nullptr;
}

bool CAddonMgr::AddonsFromRepoXML(const CRepository::DirInfo& repo, const std::string& xml, VECADDONS& addons)
{
  CXBMCTinyXML doc;
  if (!doc.Parse(xml))
  {
    CLog::Log(LOGERROR, "CAddonMgr: Failed to parse addons.xml.");
    return false;
  }

  if (doc.RootElement() == nullptr || doc.RootElement()->ValueStr() != "addons")
  {
    CLog::Log(LOGERROR, "CAddonMgr: Failed to parse addons.xml. Malformed.");
    return false;
  }

  // create a context for these addons
  cp_status_t status;
  cp_context_t *context = cp_create_context(&status);
  if (!context)
    return false;

  // each addon XML should have a UTF-8 declaration
  TiXmlDeclaration decl("1.0", "UTF-8", "");
  auto element = doc.RootElement()->FirstChildElement("addon");
  while (element)
  {
    // dump the XML back to text
    std::string xml;
    xml << decl;
    xml << *element;
    cp_status_t status;
    cp_plugin_info_t *info = cp_load_plugin_descriptor_from_memory(context, xml.c_str(), xml.size(), &status);
    if (info)
    {
      CAddonBuilder builder;
      if (Factory(info, ADDON_UNKNOWN, builder, false, repo))
      {
        auto addon = builder.Build();
        if (addon)
          addons.push_back(std::move(addon));
      }
      free(info->plugin_path);
      info->plugin_path = nullptr;
      cp_release_info(context, info);
    }
    element = element->NextSiblingElement("addon");
  }
  cp_destroy_context(context);
  return true;
}

bool CAddonMgr::IsCompatible(const IAddon& addon)
{
  for (const auto& dependency : addon.GetDependencies())
  {
    if (!dependency.optional)
    {
      // Intentionally only check the xbmc.* and kodi.* magic dependencies. Everything else will
      // not be missing anyway, unless addon was installed in an unsupported way.
      if (StringUtils::StartsWith(dependency.id, "xbmc.") ||
          StringUtils::StartsWith(dependency.id, "kodi."))
      {
        AddonPtr addon;
        bool haveAddon = GetAddon(dependency.id, addon);
        if (!haveAddon || !addon->MeetsVersion(dependency.requiredVersion))
          return false;
      }
    }
  }
  return true;
}

std::vector<DependencyInfo> CAddonMgr::GetDepsRecursive(const std::string& id)
{
  std::vector<DependencyInfo> added;
  AddonPtr root_addon;
  if (!FindInstallableById(id, root_addon) && !GetAddon(id, root_addon))
    return added;

  std::vector<DependencyInfo> toProcess;
  for (const auto& dep : root_addon->GetDependencies())
    toProcess.push_back(dep);

  while (!toProcess.empty())
  {
    auto current_dep = *toProcess.begin();
    toProcess.erase(toProcess.begin());
    if (StringUtils::StartsWith(current_dep.id, "xbmc.") ||
        StringUtils::StartsWith(current_dep.id, "kodi."))
      continue;

    auto added_it = std::find_if(added.begin(), added.end(), [&](const DependencyInfo& d){ return d.id == current_dep.id;});
    if (added_it != added.end())
    {
      if (current_dep.requiredVersion < added_it->requiredVersion)
        continue;

      bool aopt = added_it->optional;
      added.erase(added_it);
      added.push_back(current_dep);
      if (!current_dep.optional && aopt)
        continue;
    }
    else
      added.push_back(current_dep);

    AddonPtr current_addon;
    if (FindInstallableById(current_dep.id, current_addon))
    {
      for (const auto& item : current_addon->GetDependencies())
        toProcess.push_back(item);
    }
  }

  return added;
}

int cp_to_clog(cp_log_severity_t lvl)
{
  if (lvl >= CP_LOG_ERROR)
    return LOGINFO;
  return LOGDEBUG;
}

void cp_fatalErrorHandler(const char *msg)
{
  CLog::Log(LOGERROR, "ADDONS: CPluffFatalError(%s)", msg);
}

void cp_logger(cp_log_severity_t level, const char *msg, const char *apid, void *user_data)
{
  if(!apid)
    CLog::Log(cp_to_clog(level), "ADDON: cpluff: '%s'", msg);
  else
    CLog::Log(cp_to_clog(level), "ADDON: cpluff: '%s' reports '%s'", apid, msg);
}

} /* namespace ADDON */

