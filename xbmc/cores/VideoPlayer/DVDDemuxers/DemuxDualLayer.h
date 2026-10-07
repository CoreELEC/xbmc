/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

struct DemuxPacket;

class CDemuxDualLayer
{
public:
  // the enhancement layer of a paired unit, or nullptr if the unit has none
  static uint8_t* GetEnhancementLayer(const DemuxPacket& packet);
};
