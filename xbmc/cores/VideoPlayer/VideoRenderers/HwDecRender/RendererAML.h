/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"

#include <atomic>
#include <memory>
#include <mutex>

class CAMLCodec;

class CRendererAML : public CBaseRenderer
{
public:
  CRendererAML();
  virtual ~CRendererAML();

  // Registration
  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static bool Register();

  virtual void AddVideoPicture(const VideoPicture &picture, int index) override;
  virtual void ReleaseBuffer(int idx) override;
  virtual bool Configure(const VideoPicture &picture, float fps, unsigned int orientation) override;
  virtual bool IsConfigured() override { return m_bConfigured; };
  virtual bool ConfigChanged(const VideoPicture &picture) { return false; };
  virtual CRenderInfo GetRenderInfo() override;
  virtual void UnInit() override {};
  virtual void Update() override {};
  virtual void RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  virtual bool SupportsMultiPassRendering()override { return false; };
  virtual bool Flush(bool saveBuffers) override;

  // Player functions
  virtual bool IsGuiLayer() override { return false; };
  virtual bool StartVsyncPresent() override;
  virtual void StopVsyncPresent() override;
  virtual bool WaitVsync() override;
  virtual void PresentFrame(int index) override;
  virtual void WakeVsyncPresent() override;

  // Feature support
  virtual bool Supports(ESCALINGMETHOD method) const override { return false; };
  virtual bool Supports(ERENDERFEATURE feature) const override;

private:
  void Reset();
  std::shared_ptr<CAMLCodec> QueueFrame(int index, bool setVideoRect);

  static const int m_numRenderBuffers = NUM_BUFFERS;

  struct BUFFER
  {
    BUFFER() : videoBuffer(nullptr) {};
    CVideoBuffer *videoBuffer;
    int duration;
  } m_buffers[m_numRenderBuffers];

  uint64_t m_prevVPts;
  uint64_t m_hdrGuiOwner{0};
  bool m_bConfigured;

  std::atomic<bool> m_vsyncPresent{false};
  bool m_vsyncWake{false};
  //! Codec of the frame last queued from the vsync thread, for the render loop to apply geometry
  std::mutex m_pendingGeometryLock;
  std::shared_ptr<CAMLCodec> m_pendingGeometry;
};
