/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MVCOffsetMetadata.h"

#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>

namespace
{
constexpr int NAL_TYPE_SEI = 6;
constexpr int SEI_USER_DATA_UNREGISTERED = 5;
constexpr int SEI_MVC_SCALABLE_NESTING = 37;
constexpr uint8_t OFFSET_METADATA_UUID[16] = {0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
                                              0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66};
constexpr uint8_t OFFSET_METADATA_TYPE[4] = {'O', 'F', 'M', 'D'};
constexpr int MAX_SEQUENCES = 32;

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

class BitReader
{
public:
  BitReader(const std::vector<uint8_t>& data, size_t bytePos) : m_data(data), m_pos(bytePos * 8) {}

  uint32_t U(int n)
  {
    uint32_t v = 0;
    for (int i = 0; i < n && m_pos < m_data.size() * 8; i++, m_pos++)
      v = (v << 1) | ((m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1);

    return v;
  }

  uint32_t Ue()
  {
    int zeros = 0;
    while (U(1) == 0 && zeros < 31)
      zeros++;

    return (1u << zeros) - 1 + U(zeros);
  }

  size_t AlignedBytePos() const { return (m_pos + 7) / 8; }

private:
  const std::vector<uint8_t>& m_data;
  size_t m_pos;
};
} // namespace

void CMVCOffsetMetadata::Parse(const uint8_t* data, size_t size)
{
  m_parsed.reset();
  for (size_t i = 0; i + 3 < size; i++)
  {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;

    const size_t begin = i + 3;
    size_t end = begin;
    while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] <= 1))
      end++;

    if (end + 2 >= size)
      end = size;

    if ((data[begin] & 0x1f) == NAL_TYPE_SEI)
      ParseSei(Unescape(data + begin + 1, end - begin - 1), 0, false);

    i = end - 1;
  }
}

void CMVCOffsetMetadata::ParseSei(const std::vector<uint8_t>& rbsp, size_t pos, bool nested)
{
  while (pos + 2 < rbsp.size())
  {
    int type = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xff)
      type += rbsp[pos++];

    if (pos >= rbsp.size())
      return;

    type += rbsp[pos++];

    size_t size = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xff)
      size += rbsp[pos++];

    if (pos >= rbsp.size())
      return;

    size += rbsp[pos++];
    if (size > rbsp.size() - pos)
      return;

    if (type == SEI_USER_DATA_UNREGISTERED && size >= 30 &&
        std::memcmp(rbsp.data() + pos, OFFSET_METADATA_UUID, 16) == 0 &&
        std::memcmp(rbsp.data() + pos + 16, OFFSET_METADATA_TYPE, 4) == 0)
    {
      // 6 bytes pts, 2 reserved bits, 6 bits sequence count, 8 bits frame count, 16 reserved
      // bits, then per offset 1 bit direction (set: negative) and 7 bits value
      const uint8_t* data = rbsp.data() + pos + 20;
      Gop gop;
      gop.sequences = data[6] & 0x3f;
      gop.frames = data[7];
      const size_t count = static_cast<size_t>(gop.sequences) * gop.frames;
      if (gop.sequences <= MAX_SEQUENCES && count <= size - 30)
      {
        gop.offsets.reserve(count);
        for (size_t n = 0; n < count; n++)
        {
          const uint8_t v = data[10 + n];
          gop.offsets.push_back(static_cast<int8_t>(v & 0x80 ? -(v & 0x7f) : v & 0x7f));
        }

        CLog::Log(LOGDEBUG, LOGVIDEO, "CMVCOffsetMetadata::{}: {} sequences for {} frames",
                  __FUNCTION__, gop.sequences, gop.frames);
        m_parsed = std::move(gop);
      }
    }
    else if (type == SEI_MVC_SCALABLE_NESTING && !nested)
    {
      std::vector<uint8_t> payload(rbsp.begin() + pos, rbsp.begin() + pos + size);
      payload.push_back(0x80);
      BitReader br(payload, 0);
      const bool operationPoint = br.U(1);
      if (operationPoint || !br.U(1)) // all_view_components_in_au_flag
      {
        const uint32_t views = br.Ue() + 1;
        for (uint32_t v = 0; v < views && v < 1024; v++)
          br.U(10); // sei_view_id or sei_op_view_id

        if (operationPoint)
          br.U(3); // sei_op_temporal_id
      }

      ParseSei(payload, br.AlignedBytePos(), true);
    }

    pos += size;
  }
}

void CMVCOffsetMetadata::Commit(double pts)
{
  if (m_parsed)
  {
    m_gops.push_back(std::move(*m_parsed));
    m_parsed.reset();
    if (m_gops.size() > MAX_GOPS)
      m_gops.pop_front();
  }

  if (m_gops.empty() || pts == DVD_NOPTS_VALUE)
    return;

  m_gops.back().pts.push_back(pts);
}

int CMVCOffsetMetadata::GetDepth(double pts, int plane)
{
  if (plane < 0)
    return 0;

  auto gop = m_gops.rbegin();
  for (; gop != m_gops.rend(); ++gop)
  {
    if (!gop->pts.empty() &&
        *std::min_element(gop->pts.begin(), gop->pts.end()) <= pts + PTS_TOLERANCE)
      break;
  }

  if (gop == m_gops.rend())
    return 0;

  m_gops.erase(m_gops.begin(), std::prev(gop.base()));

  const Gop& g = m_gops.front();
  if (plane >= g.sequences || g.frames == 0)
    return 0;

  const auto rank = std::count_if(g.pts.begin(), g.pts.end(),
                                  [pts](double p) { return p < pts - PTS_TOLERANCE; });
  const int frame = std::min(static_cast<int>(rank), g.frames - 1);
  return g.offsets[static_cast<size_t>(plane) * g.frames + frame];
}

void CMVCOffsetMetadata::Reset()
{
  m_parsed.reset();
  m_gops.clear();
}
