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
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <mutex>

// CE providers append at the BACK of the provider list so upstream answers
// first. CGUIInfoManager::RegisterInfoProvider would front-insert

namespace CE::GUIINFO
{

// providers are deliberately leaked because they must outlive GUI teardown
inline void Register(CGUIInfoManager& infoManager)
{
  static bool registered = false;
  if (registered)
    return;
  registered = true;

  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  (void)infoManager;
}

} // namespace CE::GUIINFO
