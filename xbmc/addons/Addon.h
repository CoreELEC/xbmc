/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "addons/IAddon.h"
#include "utils/XBMCTinyXML.h"

class TiXmlElement;
class CAddonCallbacksAddon;
class CVariant;

typedef struct cp_plugin_info_t cp_plugin_info_t;
typedef struct cp_extension_t cp_extension_t;

namespace ADDON
{
  typedef std::vector<AddonPtr> VECADDONS;
  typedef std::vector<AddonPtr>::iterator IVECADDONS;

  const char* const ORIGIN_SYSTEM = "b6a50484-93a0-4afb-a01c-8d17e059feda";

  void OnPreInstall(const AddonPtr& addon);
  void OnPostInstall(const AddonPtr& addon, bool update, bool modal);
  void OnPreUnInstall(const AddonPtr& addon);
  void OnPostUnInstall(const AddonPtr& addon);

typedef enum {
  LE_ADDON_ENABLED,
  LE_ADDON_DISABLED,
  LE_ADDON_POST_UPDATE,
  LE_ADDON_PRE_UNINSTALL,
} LE_ADDON_CONTEXT;

void LEAddonHook(const AddonPtr& addon, const LE_ADDON_CONTEXT context);

class CAddon : public IAddon
{
public:
  explicit CAddon(CAddonInfo addonInfo);
  ~CAddon() override = default;

  TYPE Type() const override { return m_addonInfo.MainType(); }
  TYPE FullType() const override { return Type(); }
  bool IsType(TYPE type) const override { return type == m_addonInfo.MainType(); }
  std::string ID() const override{ return m_addonInfo.ID(); }
  std::string Name() const override { return m_addonInfo.Name(); }
  bool IsInUse() const override{ return false; };
  AddonVersion Version() const override { return m_addonInfo.Version(); }
  AddonVersion MinVersion() const override { return m_addonInfo.MinVersion(); }
  std::string Summary() const override { return m_addonInfo.Summary(); }
  std::string Description() const override { return m_addonInfo.Description(); }
  std::string Path() const override { return m_addonInfo.Path(); }
  std::string Profile() const override { return m_profilePath; }
  std::string LibPath() const override;
  std::string Author() const override { return m_addonInfo.Author(); }
  std::string ChangeLog() const override { return m_addonInfo.ChangeLog(); }
  std::string Icon() const override { return m_addonInfo.Icon(); };
  ArtMap Art() const override { return m_addonInfo.Art(); }
  std::vector<std::string> Screenshots() const override { return m_addonInfo.Screenshots(); };
  std::string Disclaimer() const override { return m_addonInfo.Disclaimer(); }
  std::string Broken() const override { return m_addonInfo.Broken(); }
  CDateTime InstallDate() const override { return m_addonInfo.InstallDate(); }
  CDateTime LastUpdated() const override { return m_addonInfo.LastUpdated(); }
  CDateTime LastUsed() const override { return m_addonInfo.LastUsed(); }
  std::string Origin() const override { return m_addonInfo.Origin(); }
  uint64_t PackageSize() const override { return m_addonInfo.PackageSize(); }
  const InfoMap& ExtraInfo() const override { return m_addonInfo.ExtraInfo(); }
  const std::vector<DependencyInfo>& GetDependencies() const override { return m_addonInfo.GetDependencies(); }

  std::string FanArt() const override
  {
    auto it = m_addonInfo.Art().find("fanart");
    return it != m_addonInfo.Art().end() ? it->second : "";
  }

  /*! \brief Check whether the this addon can be configured or not
   \return true if the addon has settings, false otherwise
   \sa LoadSettings, LoadUserSettings, SaveSettings, HasUserSettings, GetSetting, UpdateSetting
   */
  bool HasSettings() override;

  /*! \brief Check whether the user has configured this addon or not
   \return true if previously saved settings are found, false otherwise
   \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, GetSetting, UpdateSetting
   */
  virtual bool HasUserSettings();

  /*! \brief Save any user configured settings
   \sa LoadSettings, LoadUserSettings, HasSettings, HasUserSettings, GetSetting, UpdateSetting
   */
  void SaveSettings() override;

  /*! \brief Update a user-configured setting with a new value
   \param key the id of the setting to update
   \param value the value that the setting should take
   \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting
   */
  void UpdateSetting(const std::string& key, const std::string& value) override;

  /*! \brief Update a user-configured setting with a new boolean value
  \param key the id of the setting to update
  \param value the value that the setting should take
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting
  */
  bool UpdateSettingBool(const std::string& key, bool value) override;

  /*! \brief Update a user-configured setting with a new integer value
  \param key the id of the setting to update
  \param value the value that the setting should take
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting
  */
  bool UpdateSettingInt(const std::string& key, int value) override;

  /*! \brief Update a user-configured setting with a new number value
  \param key the id of the setting to update
  \param value the value that the setting should take
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting
  */
  bool UpdateSettingNumber(const std::string& key, double value) override;

  /*! \brief Update a user-configured setting with a new string value
  \param key the id of the setting to update
  \param value the value that the setting should take
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting
  */
  bool UpdateSettingString(const std::string& key, const std::string& value) override;

  /*! \brief Retrieve a particular settings value
   If a previously configured user setting is available, we return it's value, else we return the default (if available)
   \param key the id of the setting to retrieve
   \return the current value of the setting, or the default if the setting has yet to be configured.
   \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, UpdateSetting
   */
  std::string GetSetting(const std::string& key) override;

  /*! \brief Retrieve a particular settings value as boolean
  If a previously configured user setting is available, we return it's value, else we return the default (if available)
  \param key the id of the setting to retrieve
  \param value the current value of the setting, or the default if the setting has yet to be configured
  \return true if the setting's value was retrieved, false otherwise.
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, UpdateSetting
  */
  bool GetSettingBool(const std::string& key, bool& value) override;

  /*! \brief Retrieve a particular settings value as integer
  If a previously configured user setting is available, we return it's value, else we return the default (if available)
  \param key the id of the setting to retrieve
  \param value the current value of the setting, or the default if the setting has yet to be configured
  \return true if the setting's value was retrieved, false otherwise.
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, UpdateSetting
  */
  bool GetSettingInt(const std::string& key, int& value) override;

  /*! \brief Retrieve a particular settings value as number
  If a previously configured user setting is available, we return it's value, else we return the default (if available)
  \param key the id of the setting to retrieve
  \param value the current value of the setting, or the default if the setting has yet to be configured
  \return true if the setting's value was retrieved, false otherwise.
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, UpdateSetting
  */
  bool GetSettingNumber(const std::string& key, double& value) override;

  /*! \brief Retrieve a particular settings value as string
  If a previously configured user setting is available, we return it's value, else we return the default (if available)
  \param key the id of the setting to retrieve
  \param value the current value of the setting, or the default if the setting has yet to be configured
  \return true if the setting's value was retrieved, false otherwise.
  \sa LoadSettings, LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, UpdateSetting
  */
  bool GetSettingString(const std::string& key, std::string& value) override;

  CAddonSettings* GetSettings() const override;

  /*! \brief get the required version of a dependency.
   \param dependencyID the addon ID of the dependency.
   \return the version this addon requires.
   */
  AddonVersion GetDependencyVersion(const std::string &dependencyID) const override;

  /*! \brief return whether or not this addon satisfies the given version requirements
   \param version the version to meet.
   \return true if  min_version <= version <= current_version, false otherwise.
   */
  bool MeetsVersion(const AddonVersion &version) const override { return m_addonInfo.MeetsVersion(version); }
  bool ReloadSettings() override;

  /*! \brief retrieve the running instance of an add-on if it persists while running.
   */
  AddonPtr GetRunningInstance() const override { return AddonPtr(); }

  void OnPreInstall() override {};
  void OnPostInstall(bool update, bool modal) override {};
  void OnPreUnInstall() override {};
  void OnPostUnInstall() override {};

protected:
  /*! \brief Whether or not the settings have been initialized. */
  virtual bool SettingsInitialized() const;

  /*! \brief Whether or not the settings have been loaded. */
  virtual bool SettingsLoaded() const;

  /*! \brief Load the default settings and override these with any previously configured user settings
   \param bForce force the load of settings even if they are already loaded (reload)
   \param loadUserSettings whether or not to load user settings
   \return true if settings exist, false otherwise
   \sa LoadUserSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting, UpdateSetting
   */
  bool LoadSettings(bool bForce, bool loadUserSettings = true);

  /*! \brief Load the user settings
   \return true if user settings exist, false otherwise
   \sa LoadSettings, SaveSettings, HasSettings, HasUserSettings, GetSetting, UpdateSetting
   */
  virtual bool LoadUserSettings();

  /* \brief Whether there are settings to be saved
   \sa SaveSettings
   */
  virtual bool HasSettingsToSave() const;

  /*! \brief Parse settings from an XML document
   \param doc XML document to parse for settings
   \param loadDefaults if true, the default attribute is used and settings are reset prior to parsing, else the value attribute is used.
   \return true if settings are loaded, false otherwise
   \sa SettingsToXML
   */
  virtual bool SettingsFromXML(const CXBMCTinyXML &doc, bool loadDefaults = false);

  /*! \brief Write settings into an XML document
   \param doc XML document to receive the settings
   \return true if settings are saved, false otherwise
   \sa SettingsFromXML
   */
  virtual bool SettingsToXML(CXBMCTinyXML &doc) const;

  const CAddonInfo m_addonInfo;
  std::string m_userSettingsPath;

private:
  bool m_loadSettingsFailed;
  bool m_hasUserSettings;

  std::string m_profilePath;
  mutable std::shared_ptr<CAddonSettings> m_settings;
};

}; /* namespace ADDON */

