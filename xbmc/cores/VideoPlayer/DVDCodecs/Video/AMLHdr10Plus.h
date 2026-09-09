/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/BitstreamReader.h"

#include <cstdint>
#include <optional>
#include <vector>

class CHevcSei;

namespace KODI::AML::HDR
{

// Static HDR metadata from the mastering display and content light level SEIs.
struct DisplayPrimary
{
  uint16_t x;
  uint16_t y;
};

struct MasteringDisplayColourVolume
{
  // HEVC D.3.27: index 0 is GREEN, 1 is BLUE, 2 is RED - not R,G,B.
  DisplayPrimary displayPrimaries[3];
  DisplayPrimary whitePoint;
  uint32_t maxLuminance; // nits
  uint32_t minLuminance; // units of 0.0001 nits
};

struct ContentLightLevel
{
  uint16_t maxContentLightLevel;
  uint16_t maxFrameAverageLightLevel;
};

struct ProcessingWindow
{
  uint16_t window_upper_left_corner_x;
  uint16_t window_upper_left_corner_y;
  uint16_t window_lower_right_corner_x;
  uint16_t window_lower_right_corner_y;

  uint16_t center_of_ellipse_x;
  uint16_t center_of_ellipse_y;
  uint8_t rotation_angle;

  uint16_t semimajor_axis_internal_ellipse;
  uint16_t semimajor_axis_external_ellipse;
  uint16_t semiminor_axis_external_ellipse;

  bool overlap_process_option;
};

struct DistributionMaxRgb
{
  uint8_t percentage;
  uint32_t percentile;
};

struct ActualTargetedSystemDisplay
{
  uint8_t num_rows_targeted_system_display_actual_peak_luminance;
  uint8_t num_cols_targeted_system_display_actual_peak_luminance;
  std::vector<std::vector<uint8_t>> targeted_system_display_actual_peak_luminance;
};

struct ActualMasteringDisplay
{
  uint8_t num_rows_mastering_display_actual_peak_luminance = 0;
  uint8_t num_cols_mastering_display_actual_peak_luminance = 0;
  std::vector<uint8_t> mastering_display_actual_peak_luminance;
};

struct BezierCurve
{
  uint16_t knee_point_x = 0;
  uint16_t knee_point_y = 0;
  uint8_t num_bezier_curve_anchors = 0;
  std::vector<uint16_t> bezier_curve_anchors;
};

struct Luminance
{
  uint32_t maxscl[3];
  uint32_t average_maxrgb;
  uint16_t num_distribution_maxrgb_percentiles;
  std::vector<DistributionMaxRgb> distribution_maxrgb;
  uint16_t fraction_bright_pixels;
};

// ST 2094-40 carries tone mapping and colour saturation per processing window.
struct ToneMapping
{
  bool tone_mapping_flag = false;
  BezierCurve bezier_curve;
  bool color_saturation_mapping_flag = false;
  uint8_t color_saturation_weight = 0;
};

struct Hdr10PlusMetadata
{

  uint8_t itu_t_t35_country_code;
  uint16_t itu_t_t35_terminal_provider_code;
  uint16_t itu_t_t35_terminal_provider_oriented_code;

  uint8_t application_identifier;
  uint8_t application_version;

  uint8_t num_windows;
  std::vector<ProcessingWindow> processing_windows;

  uint32_t targeted_system_display_maximum_luminance;

  bool targeted_system_display_actual_peak_luminance_flag;
  ActualTargetedSystemDisplay actual_targeted_system_display;

  std::vector<Luminance> luminance;

  bool mastering_display_actual_peak_luminance_flag;
  ActualMasteringDisplay actual_mastering_display;

  std::vector<ToneMapping> tone_mapping;
};

std::optional<Hdr10PlusMetadata> hdr10plus_sei_to_metadata(CBitstreamReader& br);

// Static HDR metadata (from container hints, refreshed from MDCV/CLL SEIs),
// used when generating DV RPUs from HDR10+ dynamic metadata.
struct HDRStaticMetadataInfo
{
  uint32_t max_lum = 0; // nits
  uint32_t min_lum = 0; // units of 0.0001 nits

  uint16_t max_cll = 0;
  uint16_t max_fall = 0;

  // MDCV chromaticities, 0.00002 units, in the SEI's G,B,R order.
  uint16_t display_primaries_x[3] = {};
  uint16_t display_primaries_y[3] = {};
  uint16_t white_point_x = 0;
  uint16_t white_point_y = 0;
  bool has_mdcv = false;
};

// Read-only extraction from SEI messages already parsed by CHevcSei.
std::optional<Hdr10PlusMetadata> ExtractHdr10Plus(const std::vector<CHevcSei>& messages,
                                                  const std::vector<uint8_t>& buf);
std::optional<MasteringDisplayColourVolume> ExtractMasteringDisplayColourVolume(
    const std::vector<CHevcSei>& messages, const std::vector<uint8_t>& buf);
std::optional<ContentLightLevel> ExtractContentLightLevel(const std::vector<CHevcSei>& messages,
                                                          const std::vector<uint8_t>& buf);

} // namespace KODI::AML::HDR
