/*
 *  Copyright (C) 2005-2021 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

enum class DolbyVisionFormat
{
  DOLBYVISION_TYPE_NONE,
  DOLBYVISION_TYPE_4K30,
  DOLBYVISION_TYPE_4K60
};

class CHDRCapabilities
{
public:
  CHDRCapabilities() = default;
  ~CHDRCapabilities() = default;

  bool SupportsHDR10() const { return m_hdr10; }
  bool SupportsHLG() const { return m_hlg; }
  bool SupportsHDR10Plus() const { return m_hdr10_plus; }
  DolbyVisionFormat SupportsDolbyVision() const { return m_dolby_vision; }
  bool SupportsDVTVLED() const { return m_dv_tv_led_mode; }
  bool SupportsDVPlayerLED() const { return m_dv_player_led_mode; }

  void SetHDR10() { m_hdr10 = true; }
  void SetHLG() { m_hlg = true; }
  void SetHDR10Plus() { m_hdr10_plus = true; }
  void SetDolbyVision() { m_dolby_vision = DolbyVisionFormat::DOLBYVISION_TYPE_4K30; }
  void SetDolbyVision4k60() { m_dolby_vision = DolbyVisionFormat::DOLBYVISION_TYPE_4K60; }
  void SetDolbyVisionTVLED() { m_dv_tv_led_mode = true; }
  void SetDolbyVisionPlayerLED() { m_dv_player_led_mode = true; }

private:
  bool m_hdr10 = false;
  bool m_hlg = false;
  bool m_hdr10_plus = false;
  DolbyVisionFormat m_dolby_vision = DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
  bool m_dv_tv_led_mode = false;
  bool m_dv_player_led_mode = false;
};
