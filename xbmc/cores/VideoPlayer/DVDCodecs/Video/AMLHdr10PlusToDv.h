/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AMLHdr10Plus.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace KODI::AML::HDR
{

// V29 clamps the L1 average at 819; V40 parks it at the CMv4.0 floor and
// carries the remainder in a level 3 offset.
enum class DvCmMode
{
  V29,
  V40
};

struct Hdr10PlusPqValues
{
  uint16_t source_min_pq;
  uint16_t source_max_pq;
  uint16_t min_pq;
  uint16_t max_pq;
  uint16_t avg_pq;
  uint16_t avg_pq_offset; // level 3, 2048 = neutral
  uint16_t max_display_mastering_luminance;
  uint16_t min_display_mastering_luminance;
  uint16_t max_content_light_level;
  uint16_t max_frame_average_light_level;
  uint16_t l9[8]; // level 9 custom primaries, x32767, R G B white
  uint8_t l9_index; // preset index, or 255 for the custom values above
  bool cmv40;

  bool operator==(const Hdr10PlusPqValues& o) const
  {
    return source_min_pq == o.source_min_pq && source_max_pq == o.source_max_pq &&
           min_pq == o.min_pq && max_pq == o.max_pq && avg_pq == o.avg_pq &&
           avg_pq_offset == o.avg_pq_offset &&
           max_display_mastering_luminance == o.max_display_mastering_luminance &&
           min_display_mastering_luminance == o.min_display_mastering_luminance &&
           max_content_light_level == o.max_content_light_level &&
           max_frame_average_light_level == o.max_frame_average_light_level &&
           l9_index == o.l9_index && cmv40 == o.cmv40 &&
           std::equal(std::begin(l9), std::end(l9), std::begin(o.l9));
  }
};

// Per-stream RPU cache; never shared between decodes.
struct Hdr10PlusRpuCache
{
  std::vector<uint8_t> last_rpu_refresh;
  std::vector<uint8_t> last_rpu_hold;
  Hdr10PlusPqValues last_pq = {};
  Hdr10PlusPqValues saved_pq = {};
  bool warned_no_histogram = false;
  bool warned_peak_clamped = false;
  bool warned_no_mdcv = false;
  bool announced_cm = false;
  bool warned_generation_failed = false;

  //! \brief Snapshot the key before an access unit is built.
  void BeginAu() { saved_pq = last_pq; }

  //! \brief The decoder refused the access unit; the identical packet is
  //! re-offered, so restore the key rather than invent a scene cut.
  void RollbackAu() { last_pq = saved_pq; }

  //! \brief Seek/flush: zero the key so the next call regenerates and emits the
  //! refresh variant. On a generation failure the fallback re-emits the last
  //! RPU, hold variant included.
  void InvalidateKey() { last_pq = {}; saved_pq = {}; }
};

std::vector<uint8_t> create_rpu_nalu_for_hdr10plus(
    Hdr10PlusRpuCache& cache,
    const Hdr10PlusMetadata& meta,
    const HDRStaticMetadataInfo& hdrStaticMetadataInfo,
    DvCmMode cm_mode);

// One HDR10+ -> Dolby Vision conversion session per stream. Runs after the
// shared CBitstreamConverter, on the assembled access unit, so that no
// cross-platform file has to change.
class CHdr10PlusToDvSession
{
public:
  void SetCmMode(DvCmMode mode) { m_cmMode = mode; }

  //! \brief Seed the static HDR metadata from the container at Open. The
  //! bitstream's own MDCV/CLL SEIs override this when they appear.
  void SetStaticMetadata(const HDRStaticMetadataInfo& value) { m_static = value; }

  //! \brief Rewrite one Annex-B access unit: drop the HDR10+ SEI and append
  //! the generated RPU. False means submit the access unit unchanged.
  bool ProcessAccessUnit(const uint8_t* in, int inSize, std::vector<uint8_t>& out);

  //! \brief True once this stream has produced at least one RPU.
  bool Converted() const { return m_converted; }

  //! \brief True only for the access unit just processed.
  bool ConvertedThisAu() const { return m_convertedThisAu; }

  //! \brief The decoder refused this access unit and will be re-offered it.
  void RollbackAu()
  {
    m_cache.RollbackAu();
    if (m_holdRun > 0)
      --m_holdRun;
  }

  //! \brief Seek/flush: the next RPU must re-anchor the display.
  void Reset()
  {
    m_cache.InvalidateKey();
    m_convertedThisAu = false;
    m_holdRun = 0;
    m_holdWarned = false;
  }

private:
  // consecutive access units served from the stale hold RPU; crossing this
  // warns once and playback continues - disarming mid-session would flip the
  // decoder out of DV mid-stream
  static constexpr unsigned int MAX_HOLD_RUN = 240;

  Hdr10PlusRpuCache m_cache;
  HDRStaticMetadataInfo m_static{};
  DvCmMode m_cmMode{DvCmMode::V29};
  std::vector<uint8_t> m_rpu;
  bool m_converted{false};
  bool m_convertedThisAu{false};
  unsigned int m_holdRun{0};
  bool m_holdWarned{false};
};

} // namespace KODI::AML::HDR
