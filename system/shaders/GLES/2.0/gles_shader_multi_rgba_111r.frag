/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
uniform sampler2D m_samp1;
varying vec4 m_cord0;
varying vec4 m_cord1;
uniform float m_sdrPeak;
uniform float m_saturationBoost;

// Rec.709 luma coefficients
const vec3 lumaCoeff = vec3(0.2126, 0.7152, 0.0722);

float ForwardPQ(float L)
{
  const float m1 = 0.1593017578125;   // 2610 / 16384
  const float m2 = 78.84375;          // 2523 / 32
  const float c1 = 0.8359375;         // 3424 / 4096
  const float c2 = 18.8515625;        // 2413 / 128
  const float c3 = 18.6875;           // 2392 / 128

  float Lm1 = pow(L, m1);
  return pow((c1 + c2 * Lm1) / (1.0 + c3 * Lm1), m2);
}

// Apply luminance-only scaling while preserving saturation and hue
vec3 scaleLuminanceOnly(vec3 rgb, float multiplier, float maxSatBoost)
{
  float lum = dot(rgb, lumaCoeff);
  if (lum < 0.0001)
    return rgb * multiplier;

  float target = lum * multiplier;

  // Simple smooth compression (Reinhard-style on luminance only)
  float compressed = target / (1.0 + target * 0.55);
  compressed = min(compressed, 1.0);

  // Luminance-preserving scale
  vec3 outRGB = rgb * (compressed / lum);

  // --- PQ-based tone-dependent saturation ---
  // Convert luminance to PQ domain (perceptually uniform)
  float lumPQ = ForwardPQ(lum);

  // PQ thresholds (tune these)
  float lowPQ  = 0.05;   // ~dark tones
  float highPQ = 0.55;   // ~bright midtones

  // Smooth sigmoid-like weight in PQ space
  float w = smoothstep(lowPQ, highPQ, lumPQ);

  // Saturation factor transitions from 1.0 → maxSatBoost
  float sat = mix(1.0, maxSatBoost, w);

  // Apply luminance-preserving saturation
  float outLum = dot(outRGB, lumaCoeff);
  return mix(vec3(outLum), outRGB, sat);
}

void main()
{
  gl_FragColor = texture2D(m_samp0, m_cord0.xy);
  gl_FragColor.a *= texture2D(m_samp1, m_cord1.xy).r;

#if defined(KODI_LIMITED_RANGE)
  gl_FragColor.rgb *= (235.0 - 16.0) / 255.0;
  gl_FragColor.rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  // Apply luminance-only scaling
  rgb.rgb = scaleLuminanceOnly(rgb.rgb, m_sdrPeak, m_saturationBoost);

  // Final safety clamp
  rgb.rgb = clamp(rgb.rgb, 0.0, 1.0);
#endif
}
