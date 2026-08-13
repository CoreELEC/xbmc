/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "guilib/guiinfo/CEGUIInfoRegistry.h"
#include "guilib/guiinfo/GUIInfo.h"
#include "guilib/guiinfo/GUIInfoProvider.h"
#include "platform/linux/SysfsPath.h"
#include "utils/SystemInfo.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <cmath>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <vector>

// CE providers append at the BACK of the provider list so upstream answers
// first. CGUIInfoManager::RegisterInfoProvider would front-insert

namespace CE::GUIINFO
{

// CoreELEC platform labels that used to live as rows and cases in the shared
// GUI info files
class CCEPlatformGUIInfo : public KODI::GUILIB::GUIINFO::CGUIInfoProvider
{
public:
  bool InitCurrentItem(CFileItem* item) override { return false; }

  bool GetLabel(std::string& value,
                const CFileItem* item,
                int contextWindow,
                const KODI::GUILIB::GUIINFO::CGUIInfo& info,
                std::string* fallback) const override
  {
    switch (info.GetInfo())
    {
      case CE_PLAYER_PROCESS_AML_PIXELFORMAT:
        value = GetAMLConfigInfo("Colour depth") + ", " + GetAMLConfigInfo("Colourspace");
        return true;
      case CE_PLAYER_PROCESS_AML_DISPLAYMODE:
        value = GetAMLConfigInfo("VIC");
        return true;
      case CE_PLAYER_PROCESS_AML_EOFT_GAMUT:
        value = GetAMLConfigInfo("EOTF") + " " + GetAMLConfigInfo("Colourimetry");
        return true;
      case CE_PLAYER_PROCESS_AUDIOCHANNELS_SINK:
        value = CServiceBroker::GetDataCacheCore().GetAudioChannelsSink();
        return true;
      case CE_SYSTEM_LINUX_VER:
        value = CSysInfo::GetKernelVersionFull();
        return true;
      default:
        return false;
    }
  }

  bool GetInt(int& value,
              const CGUIListItem* item,
              int contextWindow,
              const KODI::GUILIB::GUIINFO::CGUIInfo& info) const override
  {
    return false;
  }

  bool GetBool(bool& value,
               const CGUIListItem* item,
               int contextWindow,
               const KODI::GUILIB::GUIINFO::CGUIInfo& info) const override
  {
    return false;
  }

private:
  static std::string GetAMLConfigInfo(const std::string& item)
  {
    std::string aml_config = "";
    std::string item_value = "unknown";
    std::vector<std::string> aml_config_lines;
    std::vector<std::string> aml_config_item;
    std::vector<std::string>::iterator i;

    CSysfsPath config{"/sys/class/amhdmitx/amhdmitx0/config"};
    if (config.Exists())
      aml_config = config.Get<std::string>().value();

    aml_config_lines = StringUtils::Split(aml_config, "\n");
    for (i = aml_config_lines.begin(); i < aml_config_lines.end(); i++)
    {
      if (StringUtils::StartsWithNoCase(*i, item))
      {
        aml_config_item = StringUtils::Split(*i, ": ");
        if (aml_config_item.size() > 1)
        {
          if (StringUtils::EqualsNoCase(item, "VIC"))
          {
            std::vector<std::string> sub_items = StringUtils::Split(aml_config_item.at(1), " ");

            if (sub_items.size() > 1)
            {
              double fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
              item_value = StringUtils::Left(sub_items.at(1), sub_items.at(1).length() - 4) + " ";

              if (fps != floor(fps))
              {
                float refreshrate = static_cast<float>(atof(StringUtils::Mid(sub_items.at(1), sub_items.at(1).length() - 4, 2).c_str())) / 1.001f;
                float refreshrate_rounded = std::round(refreshrate * 1000.0f) / 1000.0f;
                item_value += fmt::format("{:.6g}Hz", refreshrate_rounded);
              }
              else
                item_value += StringUtils::Mid(sub_items.at(1), sub_items.at(1).length() - 4, 2) + "Hz";
            }
          }
          else
            item_value = aml_config_item.at(1);
          break;
        }
      }
    }

    return item_value;
  }
};

// providers are deliberately leaked because they must outlive GUI teardown
inline void Register(CGUIInfoManager& infoManager)
{
  static bool registered = false;
  if (registered)
    return;
  registered = true;

  CLabelRegistry& registry = CLabelRegistry::GetInstance();
  registry.Add("player.process(amlogic.pixformat)", CE_PLAYER_PROCESS_AML_PIXELFORMAT);
  registry.Add("player.process(amlogic.displaymode)", CE_PLAYER_PROCESS_AML_DISPLAYMODE);
  registry.Add("player.process(amlogic.eoft_gamut)", CE_PLAYER_PROCESS_AML_EOFT_GAMUT);
  registry.Add("player.process(audiochannelssink)", CE_PLAYER_PROCESS_AUDIOCHANNELS_SINK);
  registry.Add("system.linuxver", CE_SYSTEM_LINUX_VER);

  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  infoManager.GetInfoProviders().RegisterProvider(new CCEPlatformGUIInfo, true);
}

} // namespace CE::GUIINFO
