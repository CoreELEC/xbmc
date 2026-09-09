/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLHdr10PlusToDv.h"

#include "AMLHdr10Plus.h"
#include "utils/HevcSei.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#ifdef HAVE_LIBDOVI
#include <libdovi/rpu_parser.h>
#endif
}

namespace KODI::AML::HDR
{

// Nits to PQ
constexpr double ST2084_Y_MAX = 10000.0;
constexpr double ST2084_M1 = 2610.0 / 16384.0;
constexpr double ST2084_M2 = (2523.0 / 4096.0) * 128.0;
constexpr double ST2084_C1 = 3424.0 / 4096.0;
constexpr double ST2084_C2 = (2413.0 / 4096.0) * 32.0;
constexpr double ST2084_C3 = (2392.0 / 4096.0) * 32.0;

// Clamp Values
constexpr std::uint16_t L1_MAX_PQ_MIN_VALUE = 2081;
// CMv2.9 average-PQ floor.
constexpr std::uint16_t L1_AVG_PQ_MIN_VALUE = 819;
// CMv4.0's own average floor. The remainder below it rides in level 3.
constexpr std::uint16_t L1_AVG_PQ_MIN_VALUE_CMV40 = 1229;
// Level 3 offsets are encoded at half level 1's scale: parser.rs scales L1 by
// 4095 and L3 by 2048 (biased +2048) off the same normalised PQ axis.
constexpr double L3_PQ_SCALE = 4095.0 / 2048.0;

static double nits_to_pq(double nits)
{
  double y = nits / ST2084_Y_MAX;
  return std::pow((ST2084_C1 + ST2084_C2 * std::pow(y, ST2084_M1)) /
                      (1.0 + ST2084_C3 * std::pow(y, ST2084_M1)),
                  ST2084_M2);
}

static uint16_t cast_pq(double nits)
{
  // saturate: the 17-bit percentile/maxscl fields reach 13107 nits, which is
  // PQ 1.028 -> 4210, outside the 12-bit range every DM field is validated on
  const double code = std::round(nits_to_pq(nits) * 4095.0);
  return static_cast<uint16_t>(code < 0.0 ? 0.0 : (code > 4095.0 ? 4095.0 : code));
}

// Presence is not validity: zeroed and implausible MDCV SEIs exist on real discs.
// Bounds are ffmpeg's (libavcodec/h2645_sei.c), in the SEI's 0.00002 units.
static bool PlausibleChroma(uint16_t x, uint16_t y)
{
  return x >= 5 && x <= 37000 && y >= 5 && y <= 42000;
}

// ffmpeg's bounds for this SEI, converted from its raw units to nits. Shared by
// the adoption gate and the use site, so both apply the same bounds.
static bool LumPlausible(uint32_t max_lum, uint32_t min_lum)
{
  return max_lum >= 5 && max_lum <= 10000 && min_lum <= 50000 && min_lum < max_lum * 10000;
}

static bool MdcvPlausible(const HDRStaticMetadataInfo& m)
{
  if (!PlausibleChroma(m.white_point_x, m.white_point_y))
    return false;
  for (int i = 0; i < 3; ++i)
    if (!PlausibleChroma(m.display_primaries_x[i], m.display_primaries_y[i]))
      return false;
  return true;
}

static uint16_t maximum_pq(const Hdr10PlusMetadata& meta)
{

  if (meta.num_windows == 0)
    return 0;

  // max(maxscl) is the full-resolution frame peak; ST 2094-10 6.1.5 is taken over
  // 6.1.2's 2x2-averaged reduced pixel set, so they are different statistics.

  const auto& max_scl = meta.luminance[0].maxscl;
  // 17-bit field, so it can encode up to 13107 nits; cast_pq saturates at 4095.
  const uint32_t max_value = *std::max_element(max_scl, max_scl + 3);

  // ST 2094-40 8.3: all-zero maxscl means "not calculated"; Annex B.3 says
  // substitute the percentile distribution. Last node is the highest (8.5.4).
  if (max_value == 0)
  {
    const auto& distributions = meta.luminance[0].distribution_maxrgb;
    if (distributions.empty())
      return 0;
    return cast_pq(static_cast<double>(distributions.back().percentile) / 10.0);
  }

  return cast_pq(static_cast<double>(max_value) / 10.0);
}

static std::optional<uint16_t> average_pq(const Hdr10PlusMetadata& meta)
{
  // guards meta.luminance[0] below: the vector is sized from num_windows, so an
  // empty one would be an out-of-bounds read, not just a wrong number
  if (meta.num_windows == 0)
    return std::nullopt;

  {
    const auto& dist = meta.luminance[0].distribution_maxrgb;

    // Integrate the quantile function over [0,1] using the percentages the
    // bitstream carries. Indices 1 and 2 are skipped ON PURPOSE: ST 2094-40
    // 8.5.4 reserves V1/V2 iff J1=5 and J2=10, and real encoders put junk there.
    const bool reservedSlots =
        dist.size() >= 9 && dist[1].percentage == 5 && dist[2].percentage == 10;
    // >= 9, not == 9: the 10-node layout carries the same reserved J1/J2 slots.

    std::vector<std::pair<double, double>> nodes;
    nodes.reserve(dist.size());
    for (size_t i = 0; i < dist.size(); ++i)
    {
      if (reservedSlots && (i == 1 || i == 2))
        continue;
      if (dist[i].percentage > 100)
        continue;
      const double p = static_cast<double>(dist[i].percentage) / 100.0;
      if (!nodes.empty() && p <= nodes.back().first)
        continue;
      nodes.emplace_back(p, nits_to_pq(static_cast<double>(dist[i].percentile) / 10.0));
    }

    if (nodes.size() >= 3)
    {
      // flat extrapolation into the head [0, p0] and the tail [pN, 1], so the
      // weights cover exactly 1.0 and no normalisation constant is needed
      double mean_pq = nodes.front().second * nodes.front().first;
      for (size_t i = 1; i < nodes.size(); ++i)
        mean_pq +=
            (nodes[i - 1].second + nodes[i].second) / 2.0 * (nodes[i].first - nodes[i - 1].first);
      mean_pq += nodes.back().second * (1.0 - nodes.back().first);

      return static_cast<uint16_t>(std::round(mean_pq * 4095.0));
    }
  }

  // Absent, not 0: a black frame computes 0 too. Not average_maxrgb - ST 2094-40
  // 8.4 averages linearised maxRGB where ST 2094-10 6.1.4 wants PQ-encoded.
  return std::nullopt;
}

// Mastering-display primaries, chromaticity x32767, in libdovi's level 9 order
// (R, G, B, white), from PREDEFINED_COLORSPACE_PRIMARIES in primaries.rs.
struct L9Preset
{
  std::uint8_t index;
  std::uint16_t v[8];
};
static const L9Preset L9_PRESETS[] = {
    {0, {22282, 10485, 8683, 22609, 4915, 1966, 10246, 10780}}, // DCI-P3 D65
    {1, {20971, 10813, 9830, 19660, 4915, 1966, 10246, 10780}}, // BT.709
    {2, {23199, 9568, 5570, 26115, 4292, 1507, 10246, 10780}}, // BT.2020
    {5, {22282, 10485, 8683, 22609, 4915, 1966, 10289, 11501}}, // DCI-P3
};

// MDCV chromaticities are in units of 0.00002 (x 50000); level 9 uses x 32767.
static std::uint16_t mdcv_to_l9(std::uint16_t v)
{
  return static_cast<std::uint16_t>(std::lround(v * 32767.0 / 50000.0));
}

static uint16_t clamp16(uint16_t d, uint16_t min, uint16_t max)
{
  uint16_t t = d < min ? min : d;
  return t > max ? max : t;
}

std::vector<uint8_t> create_rpu_nalu_for_hdr10plus(
    Hdr10PlusRpuCache& cache,
    const Hdr10PlusMetadata& meta,
    const HDRStaticMetadataInfo& hdrStaticMetadataInfo,
    DvCmMode cm_mode)
{

  // Absent/implausible MDCV: fall back to a 1000-nit / 0.0001-nit display.
  // Bounds are ffmpeg's for this SEI (libavcodec/h2645_sei.c).
  const bool lum_ok = LumPlausible(hdrStaticMetadataInfo.max_lum, hdrStaticMetadataInfo.min_lum);
  uint32_t max_lum = lum_ok ? hdrStaticMetadataInfo.max_lum : 1000;
  uint32_t min_lum = lum_ok ? hdrStaticMetadataInfo.min_lum : 1;
  // A declared zero minimum is floored at 0.0001 nits; pannal passes 0 through.
  // Keeps source_min_pq on the mastering-display ladder rather than at 0.
  if (!min_lum)
    min_lum = 1;

  // Static per-title ceiling from the mastering display alone: deriving it
  // per frame makes level 6 move.
  const uint32_t ceiling_nits = std::min<uint32_t>(max_lum, 10000);
  // cast_pq reproduces the 1000/2000/4000/10000 table (3079/3388/3696/4095);
  // cast_pq(100) is the 2081 floor used as the clamp's lower bound below.
  const uint16_t source_max_pq = std::max<uint16_t>(cast_pq(ceiling_nits), L1_MAX_PQ_MIN_VALUE);

  // Closed form reproduces the {1,2,5,10,20,50} -> {7,10,17,26,38,62} ladder.
  // Bounded by the ceiling: a bad mux can push min above max.
  const uint16_t source_min_pq =
      std::min<uint16_t>(cast_pq(static_cast<double>(min_lum) * 1e-4), source_max_pq);

  uint16_t max_pq = maximum_pq(meta);
  if (max_pq == 0)
    max_pq = cast_pq(ceiling_nits);

  const auto avg_pq_opt = average_pq(meta);
  uint16_t avg_raw = avg_pq_opt.value_or(0);
  if (!avg_pq_opt)
  {
    // Scene average genuinely unknown. It anchors the mid point of the DM sigmoid,
    // so take the floor rather than invent a value.
    if (!cache.warned_no_histogram)
    {
      cache.warned_no_histogram = true;
      CLog::Log(LOGWARNING,
                "{} - no usable percentile histogram, falling back to "
                "the minimum average PQ",
                __FUNCTION__);
    }
    avg_raw = L1_AVG_PQ_MIN_VALUE;
  }

  // CMv4.0 needs the mastering primaries for level 9; without a usable MDCV
  // emit CMv2.9 rather than assert a default.
  const bool mdcv_usable = hdrStaticMetadataInfo.has_mdcv && MdcvPlausible(hdrStaticMetadataInfo);
  const bool cmv40 = (cm_mode != DvCmMode::V29) && mdcv_usable;
  if (cm_mode != DvCmMode::V29 && !mdcv_usable && !cache.warned_no_mdcv)
  {
    cache.warned_no_mdcv = true;
    CLog::Log(LOGINFO,
              "{} - CMv4.0 requested but this stream's mastering display "
              "metadata is {}; emitting CMv2.9.",
              __FUNCTION__,
              hdrStaticMetadataInfo.has_mdcv
                  ? fmt::format("implausible (primaries {},{} {},{} {},{} white {},{})",
                                hdrStaticMetadataInfo.display_primaries_x[0],
                                hdrStaticMetadataInfo.display_primaries_y[0],
                                hdrStaticMetadataInfo.display_primaries_x[1],
                                hdrStaticMetadataInfo.display_primaries_y[1],
                                hdrStaticMetadataInfo.display_primaries_x[2],
                                hdrStaticMetadataInfo.display_primaries_y[2],
                                hdrStaticMetadataInfo.white_point_x,
                                hdrStaticMetadataInfo.white_point_y)
                  : std::string("absent"));
  }

  Hdr10PlusPqValues pq = {};
  pq.cmv40 = cmv40;
  pq.source_min_pq = source_min_pq;
  pq.source_max_pq = source_max_pq;
  // No per-frame minimum in HDR10+; the mastering minimum lives in source_min_pq.
  pq.min_pq = 0;
  // Cap the scene peak at the source peak: L1 max_pq above source_max_pq breaks
  // DV tone mapping. Static per title, so an under-declaring master clips.
  pq.max_pq = clamp16(max_pq, L1_MAX_PQ_MIN_VALUE, source_max_pq);
  if (max_pq > source_max_pq && !cache.warned_peak_clamped)
  {
    cache.warned_peak_clamped = true;
    CLog::Log(LOGINFO,
              "{} - frame peak {} exceeds the mastering display ceiling {}; "
              "clamping. Highlight detail above the declared mastering display is "
              "not carried into the RPU.",
              __FUNCTION__, max_pq, source_max_pq);
  }

  // CMv2.9 has one 12-bit average with an 819 floor (2.43 nits). CMv4.0 parks L1
  // at its own floor and carries the remainder in level 3 at half L1's scale, so
  // the pair reads back as L1.avg + (4095/2048)*(L3.avg_pq_offset - 2048).
  if (cmv40)
  {
    pq.avg_pq = clamp16(avg_raw, L1_AVG_PQ_MIN_VALUE_CMV40, (pq.max_pq - 1));
    const double off = 2048.0 + (static_cast<double>(avg_raw) - pq.avg_pq) / L3_PQ_SCALE;
    pq.avg_pq_offset = static_cast<uint16_t>(std::lround(std::clamp(off, 0.0, 4095.0)));
  }
  else
  {
    pq.avg_pq = clamp16(avg_raw, L1_AVG_PQ_MIN_VALUE, (pq.max_pq - 1));
    pq.avg_pq_offset = 2048;
  }

  // Level 6 is static, not re-derived per frame. min_lum is in 0.0001-nit units,
  // so it is not clamped to the 10000 that would mean 1 nit.
  pq.max_display_mastering_luminance = max_lum;
  pq.min_display_mastering_luminance = min_lum;
  pq.max_content_light_level = hdrStaticMetadataInfo.max_cll;
  pq.max_frame_average_light_level = hdrStaticMetadataInfo.max_fall;

  // Level 9 from the MDCV primaries rather than libdovi's DCI-P3 default.
  if (cmv40)
  {
    const uint16_t l9[8] = {
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_x[2]), // red
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_y[2]),
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_x[0]), // green
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_y[0]),
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_x[1]), // blue
        mdcv_to_l9(hdrStaticMetadataInfo.display_primaries_y[1]),
        mdcv_to_l9(hdrStaticMetadataInfo.white_point_x),
        mdcv_to_l9(hdrStaticMetadataInfo.white_point_y),
    };
    pq.l9_index = 255;
    for (const auto& preset : L9_PRESETS)
      if (std::equal(std::begin(preset.v), std::end(preset.v), std::begin(l9)))
      {
        pq.l9_index = preset.index;
        break;
      }
    std::copy(std::begin(l9), std::end(l9), std::begin(pq.l9));
    // libdovi requires every custom primary to be non-zero for length 17; the
    // mdcv_usable gate above rejects zero primaries.
  }

  if (!cache.announced_cm)
  {
    cache.announced_cm = true;
    CLog::Log(LOGINFO, "{} - content mapping version: {}", __FUNCTION__,
              pq.cmv40 ? "CMv4.0" : "CMv2.9");
  }

  // Metadata unchanged: stay in the shot, emit the flag=0 hold variant. This
  // equality test is the scene-cut detector - HDR10+ is piecewise-constant.
  if (pq == cache.last_pq && !cache.last_rpu_hold.empty())
    return cache.last_rpu_hold;

  // declared outside the guard: it is read below whether or not libdovi is built in
  bool generated = false;
  std::string libdovi_error;

#ifdef HAVE_LIBDOVI
  // Identity level 2 trims at the 100/600/1000 nit targets native content uses;
  // a display given none falls back to its own. CMv4.0 adds level 8 at 1/27/48.
  static constexpr const char* L2_BLOCKS =
      R"({"Level2":{"target_max_pq":2081,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}},)"
      R"({"Level2":{"target_max_pq":2851,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}},)"
      R"({"Level2":{"target_max_pq":3079,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}})";
  static constexpr const char* L8_BLOCKS =
      R"(,{"Level8":{"length":10,"target_display_index":1,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}})"
      R"(,{"Level8":{"length":10,"target_display_index":27,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}})"
      R"(,{"Level8":{"length":10,"target_display_index":48,"trim_slope":2048,"trim_offset":2048,"trim_power":2048,"trim_chroma_weight":2048,"trim_saturation_gain":2048,"ms_weight":2048}})";

  std::string extra;
  if (pq.cmv40)
  {
    // min and max offsets are fixed neutral.
    extra = fmt::format(
        R"(,{{"Level3":{{"min_pq_offset":2048,"max_pq_offset":2048,"avg_pq_offset":{}}}}})",
        pq.avg_pq_offset);
    extra += L8_BLOCKS;
    if (pq.l9_index == 255)
      extra += fmt::format(R"(,{{"Level9":{{"length":17,"source_primary_index":255,)"
                           R"("source_primary_red_x":{},"source_primary_red_y":{},)"
                           R"("source_primary_green_x":{},"source_primary_green_y":{},)"
                           R"("source_primary_blue_x":{},"source_primary_blue_y":{},)"
                           R"("source_primary_white_x":{},"source_primary_white_y":{}}}}})",
                           pq.l9[0], pq.l9[1], pq.l9[2], pq.l9[3], pq.l9[4], pq.l9[5], pq.l9[6],
                           pq.l9[7]);
    else
      extra +=
          fmt::format(R"(,{{"Level9":{{"length":1,"source_primary_index":{}}}}})", pq.l9_index);
  }

  // A 2-frame shot: list[0] carries scene_refresh_flag=1 (metadata just
  // changed), list[1] carries flag=0 for the identical frames that follow.
  std::string json = fmt::format(
      // long_play_mode false, where pannal uses true: it flags every frame as a
      // scene cut, and list[1] below has to be the flag=0 variant.
      R"({{"profile":"8.1","cm_version":"{}","long_play_mode":false,"length":2,)"
      R"("source_min_pq":{},"source_max_pq":{},)"
      R"("level6":{{"max_display_mastering_luminance":{},"min_display_mastering_luminance":{},)"
      R"("max_content_light_level":{},"max_frame_average_light_level":{}}},)"
      R"("default_metadata_blocks":[{{"Level1":{{"min_pq":{},"max_pq":{},"avg_pq":{}}}}},{}{}],)"
      R"("shots":[{{"start":0,"duration":2}}]}})",
      pq.cmv40 ? "V40" : "V29", pq.source_min_pq, pq.source_max_pq,
      pq.max_display_mastering_luminance, pq.min_display_mastering_luminance,
      pq.max_content_light_level, pq.max_frame_average_light_level, pq.min_pq, pq.max_pq, pq.avg_pq,
      L2_BLOCKS, extra);

  const DoviRpuOpaqueList* list = dovi_generate_from_json(json.c_str());
  if (list && list->len >= 2 && list->list && list->list[0] && list->list[1])
  {
    const DoviData* d0 = dovi_write_unspec62_nalu(list->list[0]);
    const DoviData* d1 = dovi_write_unspec62_nalu(list->list[1]);
    if (d0 && d0->data && d1 && d1->data)
    {
      cache.last_rpu_refresh.assign(d0->data, d0->data + d0->len);
      cache.last_rpu_hold.assign(d1->data, d1->data + d1->len);
      // cache the key only once both RPUs are produced, so a generation failure
      // retries next frame instead of pinning a stale pair
      cache.last_pq = pq;
      generated = true;
    }
    if (d0)
      dovi_data_free(d0);
    if (d1)
      dovi_data_free(d1);
  }
  // Block validation reports on the individual RPU, not on list->error.
  if (list && list->error)
    libdovi_error = fmt::format(" - libdovi: {}", list->error);
  else if (list && list->len >= 1 && list->list && list->list[0])
  {
    if (const char* e = dovi_rpu_get_error(list->list[0]))
      libdovi_error = fmt::format(" - libdovi rpu: {}", e);
  }
  if (list)
    dovi_rpu_list_free(list);
#endif

  // Generation failed: return nothing rather than a stale refresh RPU, which
  // would signal a scene cut carrying the wrong peak and average.
  if (!generated)
  {
    const int level = cache.warned_generation_failed ? LOGDEBUG : LOGWARNING;
    cache.warned_generation_failed = true;
    CLog::Log(level, "{} - generation failed (min_pq {} max_pq {} avg_pq {} cm {}){}", __FUNCTION__,
              pq.min_pq, pq.max_pq, pq.avg_pq, pq.cmv40 ? "V40" : "V29", libdovi_error);
    return {};
  }

  CLog::Log(LOGDEBUG,
            "{} - min_pq [{}] max_pq [{}] avg_pq [{}] "
            "mdml max [{}] mdml min [{}] cll [{}] fall [{}]",
            __FUNCTION__, pq.min_pq, pq.max_pq, pq.avg_pq, pq.max_display_mastering_luminance,
            pq.min_display_mastering_luminance, pq.max_content_light_level,
            pq.max_frame_average_light_level);

  // metadata changed -> this frame is the shot start (flag=1)
  return cache.last_rpu_refresh;
}

namespace
{
// HEVC NAL types we care about in the assembled access unit.
constexpr uint8_t NAL_SEI_PREFIX = 39;
constexpr uint8_t NAL_UNSPEC62 = 62; // Dolby Vision RPU
constexpr uint8_t NAL_UNSPEC63 = 63; // Dolby Vision EL

struct AnnexBNal
{
  int offset; // first byte of the NAL, past the start code
  int size;
};

// Split an Annex-B buffer into NALs. CBitstreamConverter emits a 4-byte start code
// for the first NAL and for UNSPEC62 and 3-byte for the rest, so handle both.
std::vector<AnnexBNal> SplitAnnexB(const uint8_t* b, int size)
{
  std::vector<AnnexBNal> nals;
  const auto isStart = [&](int p)
  { return p + 2 < size && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1; };

  int i = 0;
  while (i < size && !isStart(i))
    ++i;

  while (i < size)
  {
    const int payload = i + 3;
    int j = payload;
    while (j < size && !isStart(j))
      ++j;
    int end = j;
    // A zero immediately before the next start code belongs to that start code.
    // Exactly one: the converter prepends a 3- or 4-byte code to each verbatim
    // NAL. Do not loop - a slice's cabac_zero_word padding is part of the NAL.
    if (end > payload && end < size && b[end - 1] == 0)
      --end;
    if (end > payload)
      nals.push_back({payload, end - payload});
    i = j;
  }
  return nals;
}

void AppendNal(std::vector<uint8_t>& out, const uint8_t* nal, int size, bool fourByte)
{
  if (fourByte)
    out.push_back(0);
  out.insert(out.end(), {0, 0, 1});
  out.insert(out.end(), nal, nal + size);
}
} // unnamed namespace

bool CHdr10PlusToDvSession::ProcessAccessUnit(const uint8_t* in,
                                              int inSize,
                                              std::vector<uint8_t>& out)
{
  m_convertedThisAu = false;
  m_cache.BeginAu();

  if (!in || inSize <= 0)
    return false;

  const std::vector<AnnexBNal> nals = SplitAnnexB(in, inSize);
  if (nals.empty())
    return false;

  // Pass 1: collect static HDR metadata from every prefix SEI and note which NAL
  // carries the HDR10+ payload. The MDCV may sit after the T.35 NAL.
  std::optional<Hdr10PlusMetadata> hdr10plus;
  // Every NAL carrying a T.35, with its stripped form: a Dolby Vision access unit
  // must not still carry HDR10+. A later T.35 in the same access unit wins.
  std::vector<std::pair<size_t, std::vector<uint8_t>>> strippedSeis;
  for (size_t n = 0; n < nals.size(); ++n)
  {
    const uint8_t* nal = in + nals[n].offset;
    if (nals[n].size < 7 || ((nal[0] >> 1) & 0x3F) != NAL_SEI_PREFIX)
      continue;

    std::vector<uint8_t> clearBuf;
    const auto messages = CHevcSei::ParseSeiRbspUnclearedEmulation(nal, nals[n].size, clearBuf);

    // Frozen once the first RPU committed to a content mapping version: a late
    // MDCV must not flip cm_version or move level 6 inside an open DV decode.
    if (!m_converted)
    {
      if (const auto mdcv = ExtractMasteringDisplayColourVolume(messages, clearBuf))
      {
        // Adopt only what the use site would use - the same chromaticity and
        // luminance bounds - so an unusable in-band SEI does not replace the
        // container seed. All-or-nothing: the MDCV fields only mean anything as a set.
        HDRStaticMetadataInfo cand = m_static;
        for (int i = 0; i < 3; ++i)
        {
          cand.display_primaries_x[i] = mdcv->displayPrimaries[i].x;
          cand.display_primaries_y[i] = mdcv->displayPrimaries[i].y;
        }
        cand.white_point_x = mdcv->whitePoint.x;
        cand.white_point_y = mdcv->whitePoint.y;
        cand.max_lum = mdcv->maxLuminance; // already nits
        cand.min_lum = mdcv->minLuminance;
        if (MdcvPlausible(cand) && LumPlausible(cand.max_lum, cand.min_lum))
        {
          m_static = cand;
          m_static.has_mdcv = true;
        }
      }
      if (const auto cll = ExtractContentLightLevel(messages, clearBuf))
      {
        // zero is "unknown" in this SEI; it must not overwrite a known value
        if (cll->maxContentLightLevel != 0)
          m_static.max_cll = cll->maxContentLightLevel;
        if (cll->maxFrameAverageLightLevel != 0)
          m_static.max_fall = cll->maxFrameAverageLightLevel;
      }
    }

    // Keyed on the T.35 header, not a successful parse: a Dolby Vision access unit
    // must not keep the HDR10+ SEI even when its body was rejected.
    if (CHevcSei::FindHdr10PlusSeiMessage(clearBuf, messages))
    {
      strippedSeis.emplace_back(n, CHevcSei::RemoveHdr10PlusFromSeiNalu(nal, nals[n].size));
      if (const auto meta = ExtractHdr10Plus(messages, clearBuf))
        hdr10plus = *meta;
    }
  }

  // No usable metadata is the same case as a failed generation: hold on the cached
  // RPU rather than emit a Dolby Vision access unit with none.
  std::vector<uint8_t> rpu;
  if (hdr10plus)
    rpu = create_rpu_nalu_for_hdr10plus(m_cache, *hdr10plus, m_static, m_cmMode);

  if (!rpu.empty())
  {
    m_rpu = std::move(rpu);
    m_holdRun = 0;
    m_holdWarned = false;
  }
  else if (m_rpu.empty())
  {
    // Cold cache: nothing to stand in. Leave the access unit alone and let the
    // stream play as native HDR10+; the decoder is not in DV mode yet.
    return false;
  }
  else if (++m_holdRun >= MAX_HOLD_RUN && !m_holdWarned)
  {
    m_holdWarned = true;
    CLog::Log(LOGWARNING,
              "{} - no RPU generated for {} access units; "
              "continuing on the last cached RPU",
              __FUNCTION__, MAX_HOLD_RUN);
  }

  if (m_rpu.empty())
    return false;

  // Pass 2: rebuild the access unit, HDR10+ SEI replaced by its stripped form.
  out.clear();
  out.reserve(static_cast<size_t>(inSize) + m_rpu.size() + 8);
  for (size_t n = 0; n < nals.size(); ++n)
  {
    const uint8_t* nal = in + nals[n].offset;
    const uint8_t type = (nal[0] >> 1) & 0x3F;

    // This access unit is becoming Dolby Vision, so a native RPU/EL is dropped.
    // A remux that kept in-band RPUs but lost its dvcC gets ours instead.
    if (type == NAL_UNSPEC62 || type == NAL_UNSPEC63)
      continue;

    const bool first = out.empty();
    const auto stripped = std::find_if(strippedSeis.begin(), strippedSeis.end(),
                                       [n](const auto& s) { return s.first == n; });
    if (stripped != strippedSeis.end())
    {
      if (!stripped->second.empty())
        AppendNal(out, stripped->second.data(), static_cast<int>(stripped->second.size()), first);
      continue;
    }
    AppendNal(out, nal, nals[n].size, first);
  }

  // x265 always writes UNSPEC62 with a four-byte start code
  AppendNal(out, m_rpu.data(), static_cast<int>(m_rpu.size()), true);

  m_converted = true;
  m_convertedThisAu = true;
  return true;
}

} // namespace KODI::AML::HDR
