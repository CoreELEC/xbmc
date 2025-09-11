/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  Copyright (C) 2020-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "ScreenshotSurfaceAML.h"
#include "ServiceBroker.h"
#include "system_gl.h"
#include "threads/SingleLock.h"
#include "utils/Screenshot.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

void CScreenshotSurfaceAML::Register()
{
  CScreenShot::Register(CScreenshotSurfaceAML::CreateSurface);
}

std::unique_ptr<IScreenshotSurface> CScreenshotSurfaceAML::CreateSurface()
{
  return std::unique_ptr<CScreenshotSurfaceAML>(new CScreenshotSurfaceAML());
}

bool CScreenshotSurfaceAML::Capture()
{
  std::unique_lock<CCriticalSection> lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  CServiceBroker::GetGUI()->GetWindowManager().Render();

#ifndef HAS_GLES
  glReadBuffer(GL_BACK);
#endif

  //get current viewport
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);

  m_width  = viewport[2];
  m_height = viewport[3];
  m_stride = m_width * 4;
  unsigned char* surface = new unsigned char[m_stride * m_height];

  //read pixels from the backbuffer
#if HAS_GLES >= 2
  glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)surface);
#else
  glReadPixels(0, 0, m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE, (GLvoid*)surface);
#endif

  if (glGetError() != GL_NO_ERROR)
  {
    CLog::Log(LOGERROR, "CScreenshotSurfaceAML: glReadPixels failed");
    delete[] surface;
    return false;
  }

  m_buffer = new unsigned char[m_stride * m_height];
  for (int y = 0; y < m_height; y++)
  {
#ifdef HAS_GLES
    // we need to save in BGRA order so XOR Swap RGBA -> BGRA
    unsigned char* swap_pixels = surface + (m_height - y - 1) * m_stride;
    for (int x = 0; x < m_width; x++, swap_pixels += 4)
      std::swap(swap_pixels[0], swap_pixels[2]);
#endif
    memcpy(m_buffer + y * m_stride, surface + (m_height - y - 1) * m_stride, m_stride);
  }

  delete[] surface;
  return true;
}

void CScreenshotSurfaceAML::CaptureVideo(bool blendToBuffer)
{
  // Captures the current visible videobuffer and blend it into m_buffer (captured overlay)
  CScreenshotAML::CaptureVideoFrame(m_buffer, m_width, m_height);
}
