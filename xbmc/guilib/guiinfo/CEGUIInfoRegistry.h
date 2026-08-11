/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/guiinfo/GUIInfoLabels.h"
#include "utils/StringUtils.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// CoreELEC label registry: CE label names resolve here instead of in the
// upstream tables. Filled once at GUI init, immutable afterwards.
//
// Ids only need to stay out of the ranges upstream allocates or routes
// specially. Current CE blocks, guarded by tools/coreelec/check-label-ids.sh:
// - Player.Process: PLAYER_PROCESS_START + 30 .. + 99, starting where the
//   pre-registry amlogic.* labels were already parked (+20..+29 stay free
//   for upstream growth)
// - other CE labels: 8000..8999

namespace CE::GUIINFO
{

static_assert(PLAYER_PROCESS_VIDEO_QUEUE_DATA_LEVEL == PLAYER_PROCESS_START + 19,
              "upstream renumbered the Player.Process block, re-check CE label ids");
static_assert(ADDON_INFOS_START == 1600,
              "upstream moved ADDON_INFOS_START, re-check CE label ids");

constexpr uint32_t CE_PLAYER_PROCESS_AML_PIXELFORMAT = PLAYER_PROCESS_START + 30;
constexpr uint32_t CE_PLAYER_PROCESS_AML_DISPLAYMODE = PLAYER_PROCESS_START + 31;
constexpr uint32_t CE_PLAYER_PROCESS_AML_EOFT_GAMUT = PLAYER_PROCESS_START + 32;
constexpr uint32_t CE_PLAYER_PROCESS_AUDIOCHANNELS_SINK = PLAYER_PROCESS_START + 33;
constexpr uint32_t CE_PLAYER_PROCESS_VIDEO_SIDEDATA = PLAYER_PROCESS_START + 34;

constexpr uint32_t CE_SYSTEM_LINUX_VER = 8000;

class CLabelRegistry
{
public:
  static CLabelRegistry& GetInstance()
  {
    static CLabelRegistry registry;
    return registry;
  }

  void Add(const char* label, uint32_t id) { m_labels.emplace_back(label, id); }

  // label is the full expression as written in the skin,
  // e.g. player.process(amlogic.pixformat) or system.linuxver
  uint32_t Resolve(const std::string& label) const
  {
    // tolerate the whitespace and quoting the upstream parameter parser
    // accepts, e.g. Player.Process( "amlogic.pixformat" )
    std::string normalized;
    normalized.reserve(label.size());
    for (const char c : label)
    {
      if (c != ' ' && c != '\t' && c != '"')
        normalized += c;
    }
    for (const auto& entry : m_labels)
    {
      if (StringUtils::EqualsNoCase(normalized, entry.first))
        return entry.second;
    }
    return 0;
  }

private:
  CLabelRegistry() = default;

  std::vector<std::pair<const char*, uint32_t>> m_labels;
};

} // namespace CE::GUIINFO
