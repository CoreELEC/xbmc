/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "WinSystemAmlogicGLESContext.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

extern "C"
{
#include <libavutil/pixfmt.h>
}

using namespace KODI;
using namespace KODI::WINDOWING::AML;

CWinSystemAmlogicGLESContext::CWinSystemAmlogicGLESContext()
: m_pGLContext(new CEGLContextUtils(EGL_PLATFORM_GBM_MESA, "EGL_EXT_platform_base"))
{
}

void CWinSystemAmlogicGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "aml");
}

std::unique_ptr<CWinSystemBase> CWinSystemAmlogicGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemAmlogicGLESContext>();
}

bool CWinSystemAmlogicGLESContext::InitWindowSystem()
{
  if (!CWinSystemAmlogic::InitWindowSystem())
  {
    return false;
  }

  if (!m_pGLContext->CreatePlatformDisplay(m_amlGBMUtils->GetDevice(), m_amlGBMUtils->GetDevice()))
  {
    m_pGLContext->Destroy();
    return false;
  }

  if (!m_pGLContext->InitializeDisplay(EGL_OPENGL_ES_API))
  {
    m_pGLContext->Destroy();
    return false;
  }

  EGLint renderableType{EGL_OPENGL_ES3_BIT};
  if (!m_pGLContext->ChooseConfig(renderableType))
  {
    renderableType = EGL_OPENGL_ES2_BIT;
    if (!m_pGLContext->ChooseConfig(renderableType))
    {
      m_pGLContext->Destroy();
      return false;
    }
  }

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, (renderableType == EGL_OPENGL_ES3_BIT) ? 3 : 2}});

  if (!m_pGLContext->CreateContext(contextAttribs))
  {
    m_pGLContext->Destroy();
    return false;
  }

  if (CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_KHR_fence_sync"))
  {
    m_eglFence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(GetEGLDisplay());
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  m_amlDisplay->aml_set_drmDevice_active(false);

  m_pGLContext->DestroyContext();
  m_pGLContext->Destroy();
  return CWinSystemAmlogic::DestroyWindowSystem();
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  const RenderStereoMode stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = m_amlDisplay->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR);

  bool nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  StreamHdrType hdrType = CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType();
  bool force_mode_switch_by_hdr = (m_hdrType != hdrType);
  bool force_mode_switch_by_hotplug = m_amlDisplay->GetHotPlug();

  // get current used resolution
  if (!m_amlDisplay->aml_get_native_resolution(&current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    return false;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(hdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(m_hdrType);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "m_bWindowCreated: {}, "
    "frac rate {:d}({:d}), "
    "hdrType: {}({}), force mode switch: {}",
    __FUNCTION__,
    m_bWindowCreated,
    fractional_rate, cur_fractional_rate,
    new_hdrStr.empty() ? "none" : new_hdrStr, old_hdrStr.empty() ? "none" : old_hdrStr,
    force_mode_switch_by_hdr ? "by HDR" : force_mode_switch_by_hotplug ? "by HotPlug" : "no");
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags, m_nativeGUI);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags, nativeGUI);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_stereo_mode == stereo_mode && m_bWindowCreated &&
      !force_mode_switch_by_hdr && !force_mode_switch_by_hotplug &&
      (fractional_rate == cur_fractional_rate) &&
      nativeGUI == m_nativeGUI)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();

  // check if a forced mode switch is required
  if (force_mode_switch_by_hotplug)
  {
    m_force_mode_switch = true;
  }
  else
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      MathUtils::FloatEquals(current_resolution.fRefreshRate, res.fRefreshRate, 0.06f))
  {
    // same resolution, check frac rate and other parameter
    if ((cur_fractional_rate != fractional_rate) || force_mode_switch_by_hdr || (m_stereo_mode != stereo_mode))
      m_force_mode_switch = true;
  }

  if (m_force_mode_switch)
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);

  // refresh backup data
  m_hdrType = hdrType;
  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;
  m_nativeGUI = nativeGUI;

  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  uint32_t format = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  if (!m_amlGBMUtils->CreateSurface(res.iWidth, res.iHeight, format))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create GBM surface", __FUNCTION__);
    return false;
  }

  if (!m_pGLContext->CreatePlatformSurface(
          m_amlGBMUtils->GetSurface(),
          reinterpret_cast<EGLNativeWindowType>(m_amlGBMUtils->GetSurface())))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create CreatePlatformSurface", __FUNCTION__);
    return false;
  }

  if (!m_pGLContext->BindContext())
  {
    return false;
  }

  if (!m_delayDispReset)
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  m_pGLContext->DestroySurface();
  return CWinSystemAmlogic::DestroyWindow();
}

bool CWinSystemAmlogicGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAmlogicGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  CreateNewWindow("", fullScreen, res);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAmlogicGLESContext::SetVSyncImpl(bool enable)
{
  if (!m_pGLContext->SetVSync(enable))
  {
    CLog::Log(LOGERROR, "{},Could not set egl vsync", __FUNCTION__);
  }
}

void CWinSystemAmlogicGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  SetVSync(true);
  if (rendered)
  {
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_amlDisplay->TakeOutFenceFd();
      if (fd != -1)
      {
        m_eglFence->CreateKMSFence(fd);
        m_eglFence->WaitSyncGPU();
      }

      m_eglFence->CreateGPUFence();
    }
#endif

    // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
    // there is probably nothing we can do about it
    m_pGLContext->TrySwapBuffers();

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_eglFence->FlushFence();
      m_amlDisplay->SetInFenceFd(fd);

      m_eglFence->WaitSyncCPU();
    }
#endif

    if (m_amlGBMUtils && m_amlGBMUtils->LockFrontBuffer(m_amlDisplay->aml_get_Device_handle()))
      m_amlDisplay->FlipPage(m_amlGBMUtils->GetFBId());
  }
  else if (!videoLayer)
  {
    m_amlDisplay->aml_drmDevice_vsync();
  }

  if (m_delayDispReset && m_dispResetTimer.IsTimePast())
  {
    m_delayDispReset = false;
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }
}

bool CWinSystemAmlogicGLESContext::SetGuiCompositing(int colorTransfer)
{
  m_guiCompositing = (colorTransfer != 0);

  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (UseLimitedColor())
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGLES>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to compile GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
        return false;
      }
    }

    if (!m_compositeShader->CreateLUTs(colorTransfer))
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to create LUTs");
      m_compositeShader.reset();
      m_guiCompositing = false;
      return false;
    }
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemAmlogicGLESContext::BeginGuiComposite(bool guiWillRender)
{
  if (!m_guiCompositing)
    return false;

  m_guiWillRender = guiWillRender;

  int width = m_nWidth;
  int height = m_nHeight;

  // create or recreate FBO if size changed
  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();

    if (!m_guiFbo.Initialize())
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to initialize GUI FBO");
      return false;
    }

    if (!m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to create GUI FBO texture {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    if (GetEnabledFrontToBackRendering() && !m_guiFbo.AttachDepthBuffer(width, height))
    {
      CLog::Log(LOGERROR,
                "CWinSystemAmlogicGLESContext: failed to attach depth buffer to GUI FBO {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false; // fresh FBO is undefined, force a clear
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext: created GUI FBO {}x{}", width, height);
  }

  // When GUI render is being skipped, leave the FBO bind/clear out: nothing
  // will draw into it this frame. The FBO's prior sRGB GUI content is
  // implicitly preserved across the skipped frame as a side effect.
  //! @todo The preserved sRGB FBO is currently not leveraged: D2P reuses the
  //! post-PQ GUI plane back buffer directly via display HW, and single-plane
  //! never reaches !guiWillRender (the dirty-driven skip is gated on
  //! IsRenderingVideoLayer). Future single-plane "gate, don't move" work
  //! lets the GUI walk skip while CompositeGui still runs each video frame,
  //! re-using this cached sRGB FBO as the composite source.
  if (!guiWillRender)
    return true;

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }

  return true;
}

void CWinSystemAmlogicGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

  // Clear the backbuffer before video renders. In the FBO compositing path,
  // video renders in the RenderEx pass with clear=false, so DrawBlackBars is
  // never called. Without this clear, letterbox areas retain stale content
  // from the swap chain when the display resolution doesn't change between
  // GUI and video playback.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// CompositeGui is the last GL operation in the frame (called just before EndRender).
// GL state (blend mode, active texture, vertex arrays) is not restored afterward;
// the next frame's rendering sets its own state.
void CWinSystemAmlogicGLESContext::CompositeGui()
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;

  // Only update m_guiFboClean when GUI render fired this frame; otherwise the
  // FBO is in the same state as the previous frame and the flag stays as-is.
  // m_guiFboClean meaning depends on context:
  //   single-plane: "FBO is empty/clean" (no composite work needed)
  //   D2P:          "FBO is empty/clean AND back buffer cache is invalid"
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() == 0);
    m_guiFboClean = guiEmpty;
    if (guiEmpty)
      return;
  }
  else if (m_guiFboClean)
  {
    return;
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // set up orthographic projection (screen coords, Y-down)
  float w = static_cast<float>(m_guiFboWidth);
  float h = static_cast<float>(m_guiFboHeight);

  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};

  m_compositeShader->SetProjection(proj);
  m_compositeShader->Enable();

  GLint posLoc = m_compositeShader->GetPosLoc();
  GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

  m_compositeShader->Disable();
}

EGLDisplay CWinSystemAmlogicGLESContext::GetEGLDisplay() const
{
  return m_pGLContext->GetEGLDisplay();
}

EGLSurface CWinSystemAmlogicGLESContext::GetEGLSurface() const
{
  return m_pGLContext->GetEGLSurface();
}

EGLContext CWinSystemAmlogicGLESContext::GetEGLContext() const
{
  return m_pGLContext->GetEGLContext();
}

EGLConfig  CWinSystemAmlogicGLESContext::GetEGLConfig() const
{
  return m_pGLContext->GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAmlogicGLESContext::GetVideoSync(CVideoReferenceClock *clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAML(clock));
  return pVSync;
}

bool CWinSystemAmlogicGLESContext::SupportsStereo(const RenderStereoMode mode) const
{
  if (aml_display_support_3d() &&
      mode == RenderStereoMode::HARDWAREBASED) {
    // yes, we support hardware based MVC decoding
    return true;
  }

  return CRenderSystemGLES::SupportsStereo(mode);
}
