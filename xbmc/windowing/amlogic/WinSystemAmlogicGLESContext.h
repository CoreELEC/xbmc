/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/FrameBufferObject.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/EGLUtils.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext();
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(const RenderStereoMode mode) const override;
  void PresentRender(bool rendered, bool videoLayer) override;

  // GUI compositing for HDR
  bool SetGuiCompositing(int colorTransfer) override;
  bool BeginGuiComposite(bool guiWillRender) override;
  void EndGuiComposite() override;
  void CompositeGui() override;
  bool IsHdrComposite() const override { return m_guiCompositing; }

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override {};

private:
  std::unique_ptr<CEGLContextUtils> m_pGLContext;
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;

  bool m_guiCompositing{false};
  CFrameBufferObject m_guiFbo;
  int m_guiFboWidth{0};
  int m_guiFboHeight{0};
  // True when the GUI FBO is empty (no draws this frame); CompositeGui skips composite when true.
  bool m_guiFboClean{false};
  // Whether the GUI render pass will run this frame; set by BeginGuiComposite.
  bool m_guiWillRender{true};

  std::unique_ptr<CGuiCompositeShaderGLES> m_compositeShader;
};

}
}
}
