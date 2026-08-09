/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "guilib/guiinfo/CEGUIInfoRegistry.h"
#include "guilib/guiinfo/GUIInfo.h"
#include "guilib/guiinfo/GUIInfoProvider.h"
#include "utils/SystemInfo.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <mutex>
#include <string>

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
};

// providers are deliberately leaked because they must outlive GUI teardown
inline void Register(CGUIInfoManager& infoManager)
{
  static bool registered = false;
  if (registered)
    return;
  registered = true;

  CLabelRegistry& registry = CLabelRegistry::GetInstance();
  registry.Add("system.linuxver", CE_SYSTEM_LINUX_VER);

  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  infoManager.GetInfoProviders().RegisterProvider(new CCEPlatformGUIInfo, true);
}

} // namespace CE::GUIINFO
