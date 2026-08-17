/*
 *  Copyright (C) 2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
varying vec4 m_cord0;
uniform float m_sdrPeak;

void main ()
{
  vec3 rgb = texture2D(m_samp0, m_cord0.xy).rgb;

#if defined(KODI_LIMITED_RANGE)
  rgb *= (235.0 - 16.0) / 255.0;
  rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  rgb *= m_sdrPeak;
#elif defined(AML_TRANSFER_PQ)
  // BT.709 -> BT.2020 gamut; transfer (sRGB -> PQ) done by the OSD HDR core.
  const mat3 bt709_to_bt2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                                    0.0433, 0.0114, 0.8956);
  rgb = bt709_to_bt2020 * rgb;
#endif

  gl_FragColor = vec4(rgb, 1.0);
}
