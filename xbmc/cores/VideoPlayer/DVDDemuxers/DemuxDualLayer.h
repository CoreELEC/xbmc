/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

extern "C"
{
#include <libavutil/rational.h>
}

class CDemuxStream;
class CDemuxStreamVideo;
struct DemuxPacket;

// pairs the base and enhancement layer packets of a Dolby Vision dual track stream into one
// unit per frame, keyed on the raw container timestamp
class CDemuxDualLayer
{
public:
  CDemuxDualLayer() = default;
  ~CDemuxDualLayer();

  // takes `packet`, keyed by its raw pts (dts without pts) in the time base of `stream`, and
  // returns the base layer unit it completes, else a packet for which `IsHold` is true; the
  // caller owns either
  DemuxPacket* AddPacket(DemuxPacket* packet,
                         int64_t key,
                         const CDemuxStreamVideo& stream,
                         const std::map<int, CDemuxStream*>& streams);
  void Flush();

  static bool IsHold(const DemuxPacket& packet);
  // the enhancement layer of a paired unit, or nullptr if the unit has none
  static uint8_t* GetEnhancementLayer(const DemuxPacket& packet);

private:
  struct Entry
  {
    DemuxPacket* packet;
    int64_t key;
    AVRational timeBase;
  };

  void DropBaseLayer(size_t count);
  void DropEnhancementLayer(size_t count);

  std::deque<Entry> m_baseLayer;
  std::deque<Entry> m_enhancementLayer;
};
