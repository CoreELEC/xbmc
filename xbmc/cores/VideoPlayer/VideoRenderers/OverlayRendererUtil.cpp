/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OverlayRendererUtil.h"

#include <algorithm>
#include <cmath>

#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySSA.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySpu.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

namespace OVERLAY
{

static uint32_t build_rgba(int a, int r, int g, int b, bool mergealpha)
{
  if(mergealpha)
    return      a        << PIXEL_ASHIFT
         | (r * a / 255) << PIXEL_RSHIFT
         | (g * a / 255) << PIXEL_GSHIFT
         | (b * a / 255) << PIXEL_BSHIFT;
  else
    return a << PIXEL_ASHIFT
         | r << PIXEL_RSHIFT
         | g << PIXEL_GSHIFT
         | b << PIXEL_BSHIFT;
}

#define clamp(x) (x) > 255.0 ? 255 : ((x) < 0.0 ? 0 : (int)(x + 0.5))
static uint32_t build_rgba(const int yuv[3], int alpha, bool mergealpha)
{
  int    a = alpha + ( (alpha << 4) & 0xff );
  double r = 1.164 * (yuv[0] - 16)                          + 1.596 * (yuv[2] - 128);
  double g = 1.164 * (yuv[0] - 16) - 0.391 * (yuv[1] - 128) - 0.813 * (yuv[2] - 128);
  double b = 1.164 * (yuv[0] - 16) + 2.018 * (yuv[1] - 128);
  return build_rgba(a, clamp(r), clamp(g), clamp(b), mergealpha);
}
#undef clamp

void convert_rgba(const CDVDOverlayImage& o, bool mergealpha, std::vector<uint32_t>& rgba)
{
  convert_rgba(o, o.palette, mergealpha, rgba);
}

void convert_rgba(const CDVDOverlayImage& o,
                  const std::vector<uint32_t>& paletteIn,
                  bool mergealpha,
                  std::vector<uint32_t>& rgba)
{
  uint32_t palette[256] = {};
  for (size_t i = 0; i < paletteIn.size() && i < 256; i++)
    palette[i] = build_rgba(
        (paletteIn[i] >> PIXEL_ASHIFT) & 0xff, (paletteIn[i] >> PIXEL_RSHIFT) & 0xff,
        (paletteIn[i] >> PIXEL_GSHIFT) & 0xff, (paletteIn[i] >> PIXEL_BSHIFT) & 0xff, mergealpha);

  for (int row = 0; row < o.height; row++)
    for (int col = 0; col < o.width; col++)
      rgba[row * o.width + col] = palette[o.pixels[row * o.linesize + col]];
}

namespace
{
// ST.2084 (PQ) constants.
constexpr float ST2084_m1 = 0.1593017578125f; // 2610/16384
constexpr float ST2084_m2 = 78.84375f;        // 2523/4096 * 128
constexpr float ST2084_c1 = 0.8359375f;       // 3424/4096
constexpr float ST2084_c2 = 18.8515625f;      // 2413/4096 * 32
constexpr float ST2084_c3 = 18.6875f;         // 2392/4096 * 32

// PQ EOTF: normalized code (0..1 = 0..10000 nits) -> linear light.
float DecodePQ(float pq)
{
  float p = std::pow(pq, 1.0f / ST2084_m2);
  return std::pow(std::max(p - ST2084_c1, 0.0f) / (ST2084_c2 - ST2084_c3 * p),
                  1.0f / ST2084_m1);
}

// sRGB OETF: linear light -> sRGB code.
float EncodeSRGB(float l)
{
  return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

// BT.709 -> BT.2020 gamut matrix in linear light; the FFmpeg PGS decoder
// hardcodes the BT.709 matrix, UHD-BD PGS is BT.2020.
constexpr float k709To2020[9] = {0.6274f, 0.3293f, 0.0433f, 0.0691f, 0.9195f,
                                 0.0114f, 0.0164f, 0.0880f, 0.8956f};
} // namespace

std::vector<uint32_t> prebake_hdr_pgs_palette(const std::vector<uint32_t>& palette)
{
  // 203-nit UHD-BD reference white maps to sRGB 1.0.
  constexpr float kPeakScale = 10000.0f / 203.0f;
  std::vector<uint32_t> out = palette;

  for (auto& entry : out)
  {
    uint32_t a = (entry >> PIXEL_ASHIFT) & 0xff;
    float rgb[3] = {(entry >> PIXEL_RSHIFT) & 0xff, (entry >> PIXEL_GSHIFT) & 0xff,
                    (entry >> PIXEL_BSHIFT) & 0xff};

    // PQ -> linear, then BT.709 -> BT.2020.
    for (int i = 0; i < 3; i++)
      rgb[i] = DecodePQ(rgb[i] / 255.0f);

    float lin[3];
    for (int i = 0; i < 3; i++)
      lin[i] = k709To2020[i * 3] * rgb[0] + k709To2020[i * 3 + 1] * rgb[1] +
               k709To2020[i * 3 + 2] * rgb[2];

    // Peak scale, sRGB encode, clamp.
    for (int i = 0; i < 3; i++)
      lin[i] = std::max(lin[i] * kPeakScale, 0.0f);

    float srgb[3];
    for (int i = 0; i < 3; i++)
      srgb[i] = std::min(EncodeSRGB(lin[i]), 1.0f);

    entry = (a << PIXEL_ASHIFT) | (static_cast<uint32_t>(srgb[0] * 255.0f + 0.5f)
                                   << PIXEL_RSHIFT) |
            (static_cast<uint32_t>(srgb[1] * 255.0f + 0.5f) << PIXEL_GSHIFT) |
            (static_cast<uint32_t>(srgb[2] * 255.0f + 0.5f) << PIXEL_BSHIFT);
  }

  return out;
}

void convert_rgba(const CDVDOverlaySpu& o,
                  bool mergealpha,
                  int& min_x,
                  int& max_x,
                  int& min_y,
                  int& max_y,
                  std::vector<uint32_t>& rgba)
{
  uint32_t palette[8];
  for (int i = 0; i < 4; i++)
  {
    palette[i] = build_rgba(o.color[i], o.alpha[i], mergealpha);
    palette[i + 4] = build_rgba(o.highlight_color[i], o.highlight_alpha[i], mergealpha);
  }

  uint32_t  color;
  uint32_t* trg;
  uint16_t* src;

  int len, idx, draw;

  int btn_x_start = 0
    , btn_x_end   = 0
    , btn_y_start = 0
    , btn_y_end   = 0;

  if (o.bForced)
  {
    btn_x_start = o.crop_i_x_start - o.x;
    btn_x_end = o.crop_i_x_end - o.x;
    btn_y_start = o.crop_i_y_start - o.y;
    btn_y_end = o.crop_i_y_end - o.y;
  }

  min_x = o.width;
  max_x = 0;
  min_y = o.height;
  max_y = 0;

  trg = rgba.data();
  src = (uint16_t*)o.result;

  for (int y = 0; y < o.height; y++)
  {
    for (int x = 0; x < o.width; x += len)
    {
      /* Get the RLE part, then draw the line */
      idx = *src & 0x3;
      len = *src++ >> 2;

      while( len > 0 )
      {
        draw  = len;
        color = palette[idx];

        if (y >= btn_y_start && y <= btn_y_end)
        {
          if     ( x <  btn_x_start && x + len >= btn_x_start) // starts outside
            draw = btn_x_start - x;
          else if( x >= btn_x_start && x       <= btn_x_end)   // starts inside
          {
            color = palette[idx + 4];
            draw  = btn_x_end - x + 1;
          }
        }
        /* make sure we are not requested to draw to far */
        /* that part will be taken care of in next pass */
        if( draw > len )
          draw = len;

        /* calculate cropping */
        if(color & 0xff000000)
        {
          if(x < min_x)
            min_x = x;
          if(y < min_y)
            min_y = y;
          if(x + draw > max_x)
            max_x = x + draw;
          if(y + 1    > max_y)
            max_y = y + 1;
        }

        for(int i = 0; i < draw; i++)
          trg[x + i] = color;

        len -= draw;
        x   += draw;
      }
    }
    trg += o.width;
  }

  /* if nothing visible, just output a dummy pixel */
  if(max_x <= min_x
  || max_y <= min_y)
  {
    max_y = max_x = 1;
    min_y = min_x = 0;
  }
}

bool convert_quad(ASS_Image* images, SQuads& quads, int max_x)
{
  ASS_Image* img;
  int count = 0;

  if (!images)
    return false;

  // first calculate how many glyph we have and the total x length

  for(img = images; img; img = img->next)
  {
    // fully transparent or width or height is 0 -> not displayed
    if((img->color & 0xff) == 0xff || img->w == 0 || img->h == 0)
      continue;

    quads.size_x += img->w + 1;
    count++;
  }

  if (count == 0)
    return false;

  if (quads.size_x > max_x)
    quads.size_x = max_x;

  int curr_x = 0;
  int curr_y = 0;

  // calculate the y size of the texture

  for(img = images; img; img = img->next)
  {
    if((img->color & 0xff) == 0xff || img->w == 0 || img->h == 0)
      continue;

    // check if we need to split to new line
    if (curr_x + img->w >= quads.size_x)
    {
      quads.size_y += curr_y + 1;
      curr_x = 0;
      curr_y = 0;
    }

    curr_x += img->w + 1;

    if (img->h > curr_y)
      curr_y = img->h;
  }

  quads.size_y += curr_y + 1;

  // allocate space for the glyph positions and texturedata
  quads.quad.resize(count);
  quads.texture.resize(quads.size_x * quads.size_y);

  SQuad* v = quads.quad.data();
  uint8_t* data = quads.texture.data();

  int y = 0;

  curr_x = 0;
  curr_y = 0;

  for (img = images; img; img = img->next)
  {
    if ((img->color & 0xff) == 0xff || img->w == 0 || img->h == 0)
      continue;

    unsigned int color = img->color;
    unsigned int alpha = (color & 0xff);

    if (curr_x + img->w >= quads.size_x)
    {
      curr_y += y + 1;
      curr_x = 0;
      y = 0;
      data = quads.texture.data() + curr_y * quads.size_x;
    }

    unsigned int r = ((color >> 24) & 0xff);
    unsigned int g = ((color >> 16) & 0xff);
    unsigned int b = ((color >> 8 ) & 0xff);

    v->a = 255 - alpha;
    v->r = r;
    v->g = g;
    v->b = b;

    v->u = curr_x;
    v->v = curr_y;

    v->x = img->dst_x;
    v->y = img->dst_y;

    v->w = img->w;
    v->h = img->h;

    v++;

    for (int i = 0; i < img->h; i++)
      memcpy(data + quads.size_x * i, img->bitmap + img->stride * i, img->w);

    if (img->h > y)
      y = img->h;

    curr_x += img->w + 1;
    data   += img->w + 1;
  }
  return true;
}

int GetStereoscopicDepth(bool isPgs, int subtitleDepth)
{
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode() != RenderStereoMode::MONO &&
      CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode() != RenderStereoMode::OFF)
  {
    // 2D display, so there's no subtitle depth
    return 0;
  }

  // get configured depth
  int depth = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_SUBTITLES_STEREOSCOPICDEPTH);

  // in case of MVC playback and PGS subtitles, use the subtitle depth info additionally to the configured one
  if(CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode() == RenderStereoMode::HARDWAREBASED && isPgs)
  {
    depth += subtitleDepth;
  }

  // correct depth according to the current left/right eye view
  return depth * (CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoView() == RenderStereoView::LEFT
               ? 1
               : -1);
}

}
