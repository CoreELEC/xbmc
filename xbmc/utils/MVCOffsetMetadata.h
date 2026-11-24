/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// subtitle depth of 3D Blu-ray: the offset metadata SEI of the MVC stream holds one offset
// per picture of a GOP, in output order, for each offset sequence
class CMVCOffsetMetadata
{
public:
  static constexpr int NO_PLANE = -1;

  // parse one Annex-B access unit, kept only by a following Commit() so a refused
  // access unit fed again is not counted twice
  void Parse(const uint8_t* data, size_t size);
  void Commit(double pts);

  // offset of sequence `plane` for the picture at `pts`, 0 without data
  int GetDepth(double pts, int plane);

  void Reset();

private:
  struct Gop
  {
    int sequences = 0;
    int frames = 0;
    std::vector<int8_t> offsets;
    std::vector<double> pts;
  };

  void ParseSei(const std::vector<uint8_t>& rbsp, size_t pos, bool nested);

  static constexpr double PTS_TOLERANCE = 1000.0;
  static constexpr size_t MAX_GOPS = 64;

  std::optional<Gop> m_parsed;
  std::deque<Gop> m_gops;
};
