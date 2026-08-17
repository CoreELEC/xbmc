/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <vector>

class CDVDOverlayImage;
class CDVDOverlaySpu;
class CDVDOverlaySSA;
typedef struct ass_image ASS_Image;

namespace OVERLAY
{

struct SQuad
{
  int u, v;
  unsigned char r, g, b, a;
  int x, y;
  int w, h;
};

struct SQuads
{
  int size_x{0};
  int size_y{0};
  std::vector<uint8_t> texture;
  std::vector<SQuad> quad;
};

void convert_rgba(const CDVDOverlayImage& o, bool mergealpha, std::vector<uint32_t>& rgba);
void convert_rgba(const CDVDOverlayImage& o,
                  const std::vector<uint32_t>& palette,
                  bool mergealpha,
                  std::vector<uint32_t>& rgba);
void convert_rgba(const CDVDOverlaySpu& o,
                  bool mergealpha,
                  int& min_x,
                  int& max_x,
                  int& min_y,
                  int& max_y,
                  std::vector<uint32_t>& rgba);
bool convert_quad(ASS_Image* images, SQuads& quads, int max_x);
int GetStereoscopicDepth(bool isPgs, int subtitleDepth);

// Pre-bakes an HDR PGS palette (PQ BT.2020 RGB from the FFmpeg decoder, which
// hardcodes BT.709) to sRGB BT.2020 with the fixed 203-nit peak scale. Runs
// once per texture (256 entries); alpha preserved.
std::vector<uint32_t> prebake_hdr_pgs_palette(const std::vector<uint32_t>& palette);

} // namespace OVERLAY
