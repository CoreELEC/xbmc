/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MVCFrameNumRewriter.h"

namespace
{
std::vector<uint8_t> Unescape(const uint8_t* data, size_t size)
{
  std::vector<uint8_t> out;
  out.reserve(size);
  int zeros = 0;
  for (size_t i = 0; i < size; i++)
  {
    if (zeros >= 2 && data[i] == 0x03)
    {
      zeros = 0;
      continue;
    }
    out.push_back(data[i]);
    zeros = data[i] == 0 ? zeros + 1 : 0;
  }
  return out;
}

void Escape(const std::vector<uint8_t>& rbsp, std::vector<uint8_t>& out)
{
  out.clear();
  out.reserve(rbsp.size() + rbsp.size() / 64 + 4);
  int zeros = 0;
  for (uint8_t b : rbsp)
  {
    if (zeros >= 2 && b <= 0x03)
    {
      out.push_back(0x03);
      zeros = 0;
    }
    out.push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
}

class BitReader
{
public:
  explicit BitReader(const std::vector<uint8_t>& data, size_t bitPos = 0)
    : m_data(data),
      m_pos(bitPos)
  {
  }
  uint32_t U(int n)
  {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
    {
      if (m_pos >= m_data.size() * 8)
      {
        m_error = true;
        return 0;
      }
      v = (v << 1) | ((m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1);
      m_pos++;
    }
    return v;
  }
  uint32_t Ue()
  {
    int zeros = 0;
    while (U(1) == 0)
    {
      if (m_error || ++zeros > 31)
      {
        m_error = true;
        return 0;
      }
    }
    return ((1u << zeros) - 1) + U(zeros);
  }
  int32_t Se()
  {
    uint32_t k = Ue();
    return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
  }
  size_t Pos() const { return m_pos; }
  bool Error() const { return m_error; }

private:
  const std::vector<uint8_t>& m_data;
  size_t m_pos;
  bool m_error = false;
};

class BitWriter
{
public:
  void U(uint32_t v, int n)
  {
    for (int i = n - 1; i >= 0; i--)
      Bit((v >> i) & 1);
  }
  void Ue(uint32_t v)
  {
    const uint64_t x = static_cast<uint64_t>(v) + 1;
    int len = 0;
    while ((x >> len) > 1)
      len++;
    U(0, len);
    U(static_cast<uint32_t>(x), len + 1);
  }
  void Copy(const std::vector<uint8_t>& src, size_t from, size_t to)
  {
    for (size_t i = from; i < to; i++)
      Bit((src[i >> 3] >> (7 - (i & 7))) & 1);
  }
  void Bit(int b)
  {
    if ((m_bits & 7) == 0)
      m_data.push_back(0);
    if (b)
      m_data.back() |= 0x80 >> (m_bits & 7);
    m_bits++;
  }
  size_t Bits() const { return m_bits; }
  std::vector<uint8_t>& Data() { return m_data; }

private:
  std::vector<uint8_t> m_data;
  size_t m_bits = 0;
};

// position (in bits) of the last set bit = rbsp_stop_one_bit, or 0 if none
size_t StopBitPos(const std::vector<uint8_t>& d)
{
  for (size_t i = d.size(); i > 0; i--)
  {
    const uint8_t b = d[i - 1];
    if (b)
    {
      int bit = 7;
      while (!((b >> (7 - bit)) & 1))
        bit--;
      return (i - 1) * 8 + bit;
    }
  }
  return 0;
}

void SkipScalingList(BitReader& r, int size)
{
  int last = 8, next = 8;
  for (int j = 0; j < size; j++)
  {
    if (next != 0)
      next = (last + r.Se() + 256) % 256;
    last = next == 0 ? last : next;
  }
}
} // namespace

void CMVCFrameNumRewriter::ResetState()
{
  m_view[0] = ViewState();
  m_view[1] = ViewState();
  m_convertToIdr = false;
  m_dropLeading = false;
  m_dropPicture = false;
}

bool CMVCFrameNumRewriter::Process(const uint8_t* nal, size_t size, std::vector<uint8_t>& out)
{
  if (!nal || size < 2)
    return false;

  const int nalType = nal[0] & 0x1f;
  if (nalType != 7 && nalType != 15 && nalType != 8 && nalType != 1 && nalType != 5 &&
      nalType != 20)
    return false;

  std::vector<uint8_t> rbsp = Unescape(nal, size);

  if (nalType == 8)
  {
    ParsePps(rbsp);
    return false;
  }

  bool changed;
  if (nalType == 7 || nalType == 15)
    changed = RewriteSps(rbsp, nalType == 15);
  else
    changed = RewriteSlice(rbsp, nalType);

  if (!changed)
    return false;

  if (rbsp.empty())
  {
    out.clear(); // dropped picture
    return true;
  }
  Escape(rbsp, out);
  return true;
}

bool CMVCFrameNumRewriter::ProcessAnnexB(const uint8_t* data,
                                         size_t size,
                                         std::vector<uint8_t>& out)
{
  if (!data || size < 4)
    return false;

  // positions of the first payload byte of every NAL unit (after 00 00 01)
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= size; i++)
  {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
    {
      starts.push_back(i + 3);
      i += 2;
    }
  }
  if (starts.empty())
    return false;

  bool changed = false;
  bool dropPacket = false;
  out.clear();
  out.reserve(size + 64);
  out.insert(out.end(), data, data + starts[0]); // leading start code / zero bytes
  for (size_t n = 0; n < starts.size(); n++)
  {
    const size_t begin = starts[n];
    size_t end = n + 1 < starts.size() ? starts[n + 1] - 3 : size;
    size_t payloadEnd = end;
    // trailing zero bytes belong to the next 4 byte start code / trailing_zero_8bits
    while (payloadEnd > begin && data[payloadEnd - 1] == 0)
      payloadEnd--;
    if (payloadEnd > begin && Process(data + begin, payloadEnd - begin, m_nalBuffer))
    {
      if (m_nalBuffer.empty())
        dropPacket = true;
      out.insert(out.end(), m_nalBuffer.begin(), m_nalBuffer.end());
      changed = true;
    }
    else
      out.insert(out.end(), data + begin, data + payloadEnd);
    // zero bytes + next start code (00 00 01) verbatim
    const size_t copyEnd = n + 1 < starts.size() ? starts[n + 1] : size;
    out.insert(out.end(), data + payloadEnd, data + copyEnd);
  }
  if (dropPacket)
    out.clear();
  return changed;
}

bool CMVCFrameNumRewriter::RewriteSps(std::vector<uint8_t>& rbsp, bool subset)
{
  BitReader r(rbsp, 8);
  Sps sps;
  const uint32_t profile = r.U(8);
  r.U(16); // constraint flags + level
  const uint32_t id = r.Ue();
  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
      profile == 139 || profile == 134 || profile == 135)
  {
    sps.chromaFormatIdc = r.Ue();
    if (sps.chromaFormatIdc == 3)
      sps.separateColourPlane = r.U(1);
    r.Ue(); // bit_depth_luma
    r.Ue(); // bit_depth_chroma
    r.U(1); // qpprime_y_zero_transform_bypass
    if (r.U(1)) // seq_scaling_matrix_present
    {
      for (int i = 0; i < (sps.chromaFormatIdc != 3 ? 8 : 12); i++)
        if (r.U(1))
          SkipScalingList(r, i < 6 ? 16 : 64);
    }
  }
  const size_t fnPos = r.Pos();
  sps.log2MaxFrameNum = r.Ue() + 4;
  const size_t fnEnd = r.Pos();
  sps.pocType = r.Ue();
  if (sps.pocType == 0)
    sps.log2MaxPocLsb = r.Ue() + 4;
  else if (sps.pocType == 1)
  {
    sps.deltaPicOrderAlwaysZero = r.U(1);
    r.Se();
    r.Se();
    const uint32_t n = r.Ue();
    for (uint32_t i = 0; i < n && !r.Error(); i++)
      r.Se();
  }
  r.Ue(); // max_num_ref_frames
  r.U(1); // gaps_in_frame_num_value_allowed
  r.Ue(); // width
  r.Ue(); // height
  sps.frameMbsOnly = r.U(1);

  if (r.Error() || id > 31)
  {
    m_errors++;
    return false;
  }

  (subset ? m_subsetSps : m_sps)[id] = sps;

  if (sps.log2MaxFrameNum >= TARGET_LOG2_MAX_FRAME_NUM)
    return false;

  BitWriter w;
  w.Copy(rbsp, 0, fnPos);
  w.Ue(TARGET_LOG2_MAX_FRAME_NUM - 4);
  w.Copy(rbsp, fnEnd, StopBitPos(rbsp) + 1); // rest including rbsp_stop_one_bit
  while (w.Bits() & 7)
    w.Bit(0);
  rbsp.swap(w.Data());
  return true;
}

void CMVCFrameNumRewriter::ParsePps(const std::vector<uint8_t>& rbsp)
{
  BitReader r(rbsp, 8);
  Pps pps;
  const uint32_t id = r.Ue();
  pps.spsId = r.Ue();
  pps.cabac = r.U(1);
  pps.bottomFieldPicOrder = r.U(1);
  pps.sliceGroups = r.Ue() > 0;
  if (!pps.sliceGroups)
  {
    pps.numRefIdxL0 = r.Ue() + 1;
    pps.numRefIdxL1 = r.Ue() + 1;
    pps.weightedPred = r.U(1);
    pps.weightedBipredIdc = r.U(2);
    r.Se(); // pic_init_qp
    r.Se(); // pic_init_qs
    r.Se(); // chroma_qp_index_offset
    pps.deblockingFilterControl = r.U(1);
    r.U(1); // constrained_intra_pred
    pps.redundantPicCnt = r.U(1);
  }
  if (r.Error() || id > 255)
  {
    m_errors++;
    return;
  }
  m_pps[id] = pps;
}

bool CMVCFrameNumRewriter::RewriteSlice(std::vector<uint8_t>& rbsp, int nalType)
{
  const bool mvc = nalType == 20;
  if (rbsp.size() < (mvc ? 5u : 2u))
    return false;

  const int nri = (rbsp[0] >> 5) & 3;
  bool idr = nalType == 5;
  if (mvc)
    idr = !((rbsp[1] >> 6) & 1); // non_idr_flag

  BitReader r(rbsp, (mvc ? 4 : 1) * 8);
  const uint32_t firstMb = r.Ue();
  const int sliceType = r.Ue() % 5;
  const uint32_t ppsId = r.Ue();

  auto ppsIt = m_pps.find(ppsId);
  if (ppsIt == m_pps.end())
    return false;
  const Pps& pps = ppsIt->second;
  const std::map<int, Sps>& spsMap = mvc ? m_subsetSps : m_sps;
  auto spsIt = spsMap.find(pps.spsId);
  if (spsIt == spsMap.end())
    return false;
  const Sps& sps = spsIt->second;

  if (sps.log2MaxFrameNum >= TARGET_LOG2_MAX_FRAME_NUM)
    return false;

  if (pps.sliceGroups)
  {
    // FMO is not allowed in High / Stereo High profiles, never seen in practice
    m_errors++;
    return false;
  }

  if (sps.separateColourPlane)
    r.U(2);
  const size_t fnPos = r.Pos();
  const uint32_t frameNum = r.U(sps.log2MaxFrameNum);
  const size_t fnEnd = r.Pos();

  // unwrap frame_num into a continuous counter, per view
  ViewState& view = m_view[mvc ? 1 : 0];
  const uint32_t maxFrameNum = 1u << sps.log2MaxFrameNum;
  if (idr || !view.valid)
  {
    view.high = 0;
    view.base = 0;
  }
  else if (frameNum < view.lastFrameNum && view.lastFrameNum - frameNum > maxFrameNum / 2)
    view.high += maxFrameNum;
  view.valid = true;
  view.lastFrameNum = frameNum;
  const uint32_t unwrapped = view.high + frameNum;

  // parse the rest of the slice header to find its end
  bool fieldPic = false;
  if (!sps.frameMbsOnly)
  {
    fieldPic = r.U(1);
    if (fieldPic)
      r.U(1);
  }
  if (idr)
    r.Ue();
  uint32_t pocLsb = 0;
  if (sps.pocType == 0)
  {
    pocLsb = r.U(sps.log2MaxPocLsb);
    if (pps.bottomFieldPicOrder && !fieldPic)
      r.Se();
  }
  else if (sps.pocType == 1 && !sps.deltaPicOrderAlwaysZero)
  {
    r.Se();
    if (pps.bottomFieldPicOrder && !fieldPic)
      r.Se();
  }
  if (pps.redundantPicCnt)
    r.Ue();
  const bool isP = sliceType == 0 || sliceType == 3;
  const bool isB = sliceType == 1;
  const bool isI = sliceType == 2 || sliceType == 4;
  if (isB)
    r.U(1); // direct_spatial_mv_pred
  int numL0 = pps.numRefIdxL0, numL1 = pps.numRefIdxL1;
  if (isP || isB)
  {
    if (r.U(1)) // num_ref_idx_active_override
    {
      numL0 = r.Ue() + 1;
      if (isB)
        numL1 = r.Ue() + 1;
    }
  }
  // ref_pic_list_modification / ref_pic_list_mvc_modification
  for (int list = 0; list < (isB ? 2 : (isI ? 0 : 1)); list++)
  {
    if (r.U(1))
    {
      for (int n = 0; n < 1000 && !r.Error(); n++)
      {
        if (r.Ue() == 3)
          break;
        r.Ue();
      }
    }
  }
  if ((pps.weightedPred && isP) || (pps.weightedBipredIdc == 1 && isB))
  {
    const int chromaArrayType = sps.separateColourPlane ? 0 : sps.chromaFormatIdc;
    r.Ue();
    if (chromaArrayType != 0)
      r.Ue();
    for (int list = 0; list < (isB ? 2 : 1); list++)
    {
      for (int i = 0; i < (list ? numL1 : numL0) && !r.Error(); i++)
      {
        if (r.U(1))
        {
          r.Se();
          r.Se();
        }
        if (chromaArrayType != 0 && r.U(1))
        {
          r.Se();
          r.Se();
          r.Se();
          r.Se();
        }
      }
    }
  }
  const size_t markingPos = r.Pos();
  if (nri)
  {
    if (idr)
      r.U(2);
    else if (r.U(1)) // adaptive_ref_pic_marking_mode
    {
      for (int n = 0; n < 1000 && !r.Error(); n++)
      {
        const uint32_t op = r.Ue();
        if (op == 0)
          break;
        if (op == 1 || op == 3)
          r.Ue();
        if (op == 2)
          r.Ue();
        if (op == 3 || op == 6)
          r.Ue();
        if (op == 4)
          r.Ue();
      }
    }
  }
  const size_t markingEnd = r.Pos();
  if (pps.cabac && !isI)
    r.Ue(); // cabac_init_idc
  r.Se(); // slice_qp_delta
  if (sliceType == 3 || sliceType == 4)
  {
    if (sliceType == 3)
      r.U(1);
    r.Se();
  }
  if (pps.deblockingFilterControl)
  {
    if (r.Ue() != 1)
    {
      r.Se();
      r.Se();
    }
  }
  const size_t hdrEnd = r.Pos();

  if (r.Error())
  {
    m_errors++;
    return false;
  }

  // per access unit (first base view slice): insert an IDR before the 16 bit counter
  // wraps, then drop the leading pictures (POC before the IDR) that reference older pictures
  if (!mvc && firstMb == 0)
  {
    m_convertToIdr = false;
    m_dropPicture = false;
    const uint32_t maxPocLsb = 1u << sps.log2MaxPocLsb;
    if (m_dropLeading)
    {
      const uint32_t diff = (pocLsb - m_idrPocLsb) & (maxPocLsb - 1);
      if (sps.pocType == 0 && diff > maxPocLsb / 2 && !isI)
        m_dropPicture = true;
      else
        m_dropLeading = false;
    }
    if (!m_dropPicture && !idr && isI && nri && sps.pocType == 0 &&
        unwrapped - view.base >= IDR_RESET_THRESHOLD)
    {
      m_convertToIdr = true;
      m_dropLeading = true;
      m_idrPocLsb = pocLsb;
    }
  }

  if (m_dropPicture)
  {
    rbsp.clear();
    return true;
  }

  const bool toIdr = m_convertToIdr && nri && !idr && (!mvc || ((rbsp[3] >> 2) & 1)); // anchor
  if (m_convertToIdr && !toIdr && !idr)
    m_errors++;
  if (toIdr)
    view.base = unwrapped;
  const uint32_t newFrameNum = (unwrapped - view.base) & ((1u << TARGET_LOG2_MAX_FRAME_NUM) - 1);

  BitWriter w;
  w.Copy(rbsp, 0, fnPos);
  w.U(newFrameNum, TARGET_LOG2_MAX_FRAME_NUM);
  if (toIdr)
  {
    w.Ue(0); // idr_pic_id
    w.Copy(rbsp, fnEnd, markingPos);
    w.U(0, 2); // no_output_of_prior_pics_flag, long_term_reference_flag
    w.Copy(rbsp, markingEnd, hdrEnd);
  }
  else
    w.Copy(rbsp, fnEnd, hdrEnd);

  if (pps.cabac)
  {
    // slice data starts byte aligned after cabac_alignment_one_bits
    const size_t alignEnd = (hdrEnd + 7) & ~static_cast<size_t>(7);
    for (size_t i = hdrEnd; i < alignEnd; i++)
    {
      if (!((rbsp[i >> 3] >> (7 - (i & 7))) & 1))
      {
        // header misparsed, leave slice untouched
        m_errors++;
        return false;
      }
    }
    while (w.Bits() & 7)
      w.Bit(1);
    std::vector<uint8_t>& d = w.Data();
    d.insert(d.end(), rbsp.begin() + alignEnd / 8, rbsp.end());
  }
  else
  {
    w.Copy(rbsp, hdrEnd, StopBitPos(rbsp) + 1);
    while (w.Bits() & 7)
      w.Bit(0);
  }

  rbsp.swap(w.Data());
  if (toIdr)
  {
    if (mvc)
      rbsp[1] &= ~0x40; // non_idr_flag = 0
    else
      rbsp[0] = (rbsp[0] & 0xe0) | 5; // nal_unit_type = IDR
  }
  return true;
}
