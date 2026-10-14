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
#include <map>
#include <vector>

// Rewrites MVC streams to 16 bit frame_num (bit-identical pictures), because the
// Amlogic MVC firmware corrupts the dependent view when frame_num wraps around.
class CMVCFrameNumRewriter
{
public:
  static constexpr int TARGET_LOG2_MAX_FRAME_NUM = 16;
  // Shortly before the 16 bit counter wraps, the next I picture is rewritten to an IDR.
  static constexpr uint32_t IDR_RESET_THRESHOLD = (1u << TARGET_LOG2_MAX_FRAME_NUM) - 4096;

  // Process one escaped NAL unit without start code.
  // Returns true if out holds a rewritten NAL that replaces the input.
  bool Process(const uint8_t* nal, size_t size, std::vector<uint8_t>& out);

  // Process an Annex-B buffer (NAL units with start codes). Returns true if out holds
  // the rewritten buffer; an empty out means the whole packet must be dropped.
  bool ProcessAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

  // Reset the frame_num unwrap state (call after flush / seek).
  void ResetState();

  // Number of NAL units that could not be parsed (should stay 0).
  unsigned int GetErrorCount() const { return m_errors; }

private:
  struct Sps
  {
    int log2MaxFrameNum = 4;
    int pocType = 0;
    int log2MaxPocLsb = 4;
    bool deltaPicOrderAlwaysZero = false;
    bool frameMbsOnly = true;
    bool separateColourPlane = false;
    int chromaFormatIdc = 1;
  };
  struct Pps
  {
    int spsId = 0;
    bool cabac = false;
    bool bottomFieldPicOrder = false;
    int numRefIdxL0 = 1;
    int numRefIdxL1 = 1;
    bool weightedPred = false;
    int weightedBipredIdc = 0;
    bool deblockingFilterControl = false;
    bool redundantPicCnt = false;
    bool sliceGroups = false;
  };
  struct ViewState
  {
    bool valid = false;
    uint32_t lastFrameNum = 0;
    uint32_t high = 0;
    uint32_t base = 0;
  };

  bool RewriteSps(std::vector<uint8_t>& rbsp, bool subset);
  void ParsePps(const std::vector<uint8_t>& rbsp);
  bool RewriteSlice(std::vector<uint8_t>& rbsp, int nalType);

  std::map<int, Sps> m_sps;
  std::map<int, Sps> m_subsetSps;
  std::map<int, Pps> m_pps;
  ViewState m_view[2];
  bool m_convertToIdr = false; // current access unit is rewritten to an IDR
  bool m_dropLeading = false; // dropping leading pictures after an inserted IDR
  bool m_dropPicture = false; // current access unit is dropped
  uint32_t m_idrPocLsb = 0;
  unsigned int m_errors = 0;
  std::vector<uint8_t> m_nalBuffer;
};
