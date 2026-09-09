/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLHdr10Plus.h"

#include "utils/BitstreamReader.h"
#include "utils/HevcSei.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace KODI::AML::HDR
{

std::optional<Hdr10PlusMetadata> hdr10plus_sei_to_metadata(CBitstreamReader& br)
{
  Hdr10PlusMetadata metadata = {};

  metadata.itu_t_t35_country_code = br.ReadBits(8);
  metadata.itu_t_t35_terminal_provider_code = br.ReadBits(16);
  metadata.itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);
  metadata.application_identifier = br.ReadBits(8);
  metadata.application_version = br.ReadBits(8);
  metadata.num_windows = br.ReadBits(2);
  // ST 2094-40 allows 1..3; 0 desynchronises every field that follows
  if (metadata.num_windows < 1 || metadata.num_windows > 3)
  {
    CLog::Log(LOGDEBUG, "{} - rejected: num_windows {}", __FUNCTION__, metadata.num_windows);
    return std::nullopt;
  }

  if (metadata.num_windows > 1)
  {

    metadata.processing_windows = std::vector<ProcessingWindow>(metadata.num_windows);

    for (uint8_t i = 1; i < metadata.num_windows; i++)
    {
      ProcessingWindow& window = metadata.processing_windows[i];
      window.window_upper_left_corner_x = br.ReadBits(16);
      window.window_upper_left_corner_y = br.ReadBits(16);
      window.window_lower_right_corner_x = br.ReadBits(16);
      window.window_lower_right_corner_y = br.ReadBits(16);
      window.center_of_ellipse_x = br.ReadBits(16);
      window.center_of_ellipse_y = br.ReadBits(16);
      window.rotation_angle = br.ReadBits(8);
      window.semimajor_axis_internal_ellipse = br.ReadBits(16);
      window.semimajor_axis_external_ellipse = br.ReadBits(16);
      window.semiminor_axis_external_ellipse = br.ReadBits(16);
      window.overlap_process_option = br.ReadBits(1);
    }
  }

  metadata.targeted_system_display_maximum_luminance = br.ReadBits(27);
  metadata.targeted_system_display_actual_peak_luminance_flag = br.ReadBits(1);

  if (metadata.targeted_system_display_actual_peak_luminance_flag)
  {
    ActualTargetedSystemDisplay& display = metadata.actual_targeted_system_display;
    display.num_rows_targeted_system_display_actual_peak_luminance = br.ReadBits(5);
    display.num_cols_targeted_system_display_actual_peak_luminance = br.ReadBits(5);
    // ST 2094-40 allows 2..25 rows/cols
    if (display.num_rows_targeted_system_display_actual_peak_luminance < 2 ||
        display.num_rows_targeted_system_display_actual_peak_luminance > 25 ||
        display.num_cols_targeted_system_display_actual_peak_luminance < 2 ||
        display.num_cols_targeted_system_display_actual_peak_luminance > 25)
    {
      CLog::Log(LOGDEBUG, "{} - rejected: targeted grid {}x{}", __FUNCTION__,
                display.num_rows_targeted_system_display_actual_peak_luminance,
                display.num_cols_targeted_system_display_actual_peak_luminance);
      return std::nullopt;
    }
    display.targeted_system_display_actual_peak_luminance.resize(
        display.num_rows_targeted_system_display_actual_peak_luminance,
        std::vector<uint8_t>(display.num_cols_targeted_system_display_actual_peak_luminance));
    for (uint8_t i = 0; i < display.num_rows_targeted_system_display_actual_peak_luminance; i++)
    {
      for (uint8_t j = 0; j < display.num_cols_targeted_system_display_actual_peak_luminance; j++)
      {
        display.targeted_system_display_actual_peak_luminance[i][j] = br.ReadBits(4);
      }
    }
  }

  // Parse luminance info
  if (metadata.num_windows > 0)
  {

    metadata.luminance = std::vector<Luminance>(metadata.num_windows);

    for (uint8_t i = 0; i < metadata.num_windows; i++)
    {

      Luminance& luminance = metadata.luminance[i];
      // Parse maxscl and average_maxrgb
      for (int i = 0; i < 3; i++)
      {
        luminance.maxscl[i] = br.ReadBits(17);
      }
      luminance.average_maxrgb = br.ReadBits(17);

      // Parse distribution maxrgb
      luminance.num_distribution_maxrgb_percentiles = br.ReadBits(4);
      luminance.distribution_maxrgb.resize(luminance.num_distribution_maxrgb_percentiles);
      for (uint8_t i = 0; i < luminance.num_distribution_maxrgb_percentiles; i++)
      {
        luminance.distribution_maxrgb[i].percentage = br.ReadBits(7);
        luminance.distribution_maxrgb[i].percentile = br.ReadBits(17);
      }
      luminance.fraction_bright_pixels = br.ReadBits(10);
    }
  }

  // Parse mastering display info
  metadata.mastering_display_actual_peak_luminance_flag = br.ReadBits(1);
  if (metadata.mastering_display_actual_peak_luminance_flag)
  {
    ActualMasteringDisplay& display = metadata.actual_mastering_display;
    display.num_rows_mastering_display_actual_peak_luminance = br.ReadBits(5);
    display.num_cols_mastering_display_actual_peak_luminance = br.ReadBits(5);
    // ST 2094-40 allows 2..25 rows/cols
    if (display.num_rows_mastering_display_actual_peak_luminance < 2 ||
        display.num_rows_mastering_display_actual_peak_luminance > 25 ||
        display.num_cols_mastering_display_actual_peak_luminance < 2 ||
        display.num_cols_mastering_display_actual_peak_luminance > 25)
    {
      CLog::Log(LOGDEBUG, "{} - rejected: mastering grid {}x{}", __FUNCTION__,
                display.num_rows_mastering_display_actual_peak_luminance,
                display.num_cols_mastering_display_actual_peak_luminance);
      return std::nullopt;
    }
    display.mastering_display_actual_peak_luminance.resize(
        display.num_rows_mastering_display_actual_peak_luminance *
        display.num_cols_mastering_display_actual_peak_luminance);
    for (size_t i = 0; i < display.mastering_display_actual_peak_luminance.size(); i++)
    {
      display.mastering_display_actual_peak_luminance[i] = br.ReadBits(4);
    }
  }

  // Parse tone mapping and colour saturation info, per window
  metadata.tone_mapping = std::vector<ToneMapping>(metadata.num_windows);
  for (uint8_t w = 0; w < metadata.num_windows; w++)
  {
    ToneMapping& toneMapping = metadata.tone_mapping[w];
    toneMapping.tone_mapping_flag = br.ReadBits(1);
    if (toneMapping.tone_mapping_flag)
    {
      BezierCurve& curve = toneMapping.bezier_curve;
      curve.knee_point_x = br.ReadBits(12);
      curve.knee_point_y = br.ReadBits(12);
      curve.num_bezier_curve_anchors = br.ReadBits(4);
      curve.bezier_curve_anchors.resize(curve.num_bezier_curve_anchors);
      for (uint8_t i = 0; i < curve.num_bezier_curve_anchors; i++)
      {
        curve.bezier_curve_anchors[i] = br.ReadBits(10);
      }
    }

    toneMapping.color_saturation_mapping_flag = br.ReadBits(1);
    if (toneMapping.color_saturation_mapping_flag)
    {
      toneMapping.color_saturation_weight = br.ReadBits(6);
    }
  }

  return metadata;
}

std::optional<Hdr10PlusMetadata> ExtractHdr10Plus(const std::vector<CHevcSei>& messages,
                                                  const std::vector<uint8_t>& buf)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7 && sei.m_payloadOffset < buf.size())
    {
      const uint8_t* data = buf.data() + sei.m_payloadOffset;
      // clamp to the real buffer: a crafted payload size/offset must not read past it
      const size_t size = std::min<size_t>(sei.m_payloadSize, buf.size() - sei.m_payloadOffset);

      CBitstreamReader br(data, size);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // United States, Samsung Electronics America, ST 2094-40
      if (itu_t_t35_country_code == 0xB5 && itu_t_t35_terminal_provider_code == 0x003C &&
          itu_t_t35_terminal_provider_oriented_code == 0x0001)
      {
        const auto application_identifier = br.ReadBits(8);
        const auto application_version = br.ReadBits(8);

        if (application_identifier == 4 && application_version <= 1)
        {
          CBitstreamReader br2(data, size);
          const auto metadata = hdr10plus_sei_to_metadata(br2);
          // A payload that ran past its own end parsed garbage: reject it. Bail rather
          // than scan on - Find matches on the T.35 header, so Extract must agree.
          if (!metadata || br2.Position() > size * 8)
            return std::nullopt;
          return *metadata;
        }
      }
    }
  }

  return std::nullopt;
}

std::optional<MasteringDisplayColourVolume> ExtractMasteringDisplayColourVolume(
    const std::vector<CHevcSei>& messages, const std::vector<uint8_t>& buf)
{
  for (const auto& sei : messages)
  {
    // Mastering Display Colour Volume SEI (payload type 137)
    if (sei.m_payloadType == 137 && sei.m_payloadSize >= 24 && sei.m_payloadOffset < buf.size())
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset,
                          std::min<size_t>(sei.m_payloadSize, buf.size() - sei.m_payloadOffset));

      MasteringDisplayColourVolume metadata;

      for (int i = 0; i < 3; ++i)
      {
        metadata.displayPrimaries[i].x = br.ReadBits(16);
        metadata.displayPrimaries[i].y = br.ReadBits(16);
      }

      metadata.whitePoint.x = br.ReadBits(16);
      metadata.whitePoint.y = br.ReadBits(16);

      const uint32_t maxLuminanceRaw = br.ReadBits(32);
      const uint32_t minLuminanceRaw = br.ReadBits(32);

      // Convert to nits for max only (min stays in units of 0.0001 nits)
      metadata.maxLuminance = static_cast<uint32_t>(std::lround(maxLuminanceRaw / 10000.0));
      metadata.minLuminance = minLuminanceRaw;

      return metadata;
    }
  }
  return std::nullopt;
}

std::optional<ContentLightLevel> ExtractContentLightLevel(const std::vector<CHevcSei>& messages,
                                                          const std::vector<uint8_t>& buf)
{
  for (const auto& sei : messages)
  {
    // Content Light Level Information SEI (payload type 144)
    if (sei.m_payloadType == 144 && sei.m_payloadSize >= 4 && sei.m_payloadOffset < buf.size())
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset,
                          std::min<size_t>(sei.m_payloadSize, buf.size() - sei.m_payloadOffset));

      const uint16_t maxCLL = br.ReadBits(16);
      const uint16_t maxFALL = br.ReadBits(16);

      return ContentLightLevel{maxCLL, maxFALL};
    }
  }
  return std::nullopt;
}
} // namespace KODI::AML::HDR
