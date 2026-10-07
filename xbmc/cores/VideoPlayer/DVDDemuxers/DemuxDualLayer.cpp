/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DemuxDualLayer.h"

#include "cores/VideoPlayer/Interface/DemuxPacket.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

uint8_t* CDemuxDualLayer::GetEnhancementLayer(const DemuxPacket& packet)
{
  if (packet.elSize == 0)
    return nullptr;

  return packet.pData + packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE;
}
