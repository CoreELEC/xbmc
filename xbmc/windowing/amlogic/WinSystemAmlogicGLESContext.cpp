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
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

using namespace KODI;
using namespace KODI::WINDOWING::AML;

CWinSystemAmlogicGLESContext::CWinSystemAmlogicGLESContext()
:  m_cs(0)
,  m_cd(0)
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

  if (!m_pGLContext.CreateDisplay(m_nativeDisplay))
  {
    return false;
  }

  if (!m_pGLContext.InitializeDisplay(EGL_OPENGL_ES_API))
  {
    return false;
  }

  if (!m_pGLContext.ChooseConfig(EGL_OPENGL_ES2_BIT))
  {
    return false;
  }

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

  if (!m_pGLContext.CreateContext(contextAttribs))
  {
    return false;
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  m_pGLContext.DestroyContext();
  m_pGLContext.Destroy();
  return CWinSystemAmlogic::DestroyWindowSystem();
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  RENDER_STEREO_MODE stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = fractional_rate;
  if (aml_has_frac_rate_policy())
  {
    CSysfsPath amhdmitx0_frac_rate_policy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
    cur_fractional_rate = amhdmitx0_frac_rate_policy.Get<int>().value();

    if (cur_fractional_rate != fractional_rate)
      amhdmitx0_frac_rate_policy.Set(fractional_rate);
  }

  // check for colour subsampling/depth change
  CSysfsPath amhdmitx0_cs{"/sys/class/amhdmitx/amhdmitx0/cs"};
  CSysfsPath amhdmitx0_cd{"/sys/class/amhdmitx/amhdmitx0/cd"};
  int cs = 0;
  int cd = 0;
  if (amhdmitx0_cs.Exists() && amhdmitx0_cd.Exists())
  {
    cs = amhdmitx0_cs.Get<int>().value();
    cd = amhdmitx0_cd.Get<int>().value();
  }

  // get current used resolution
  if (!aml_get_native_resolution(&current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "m_bWindowCreated: {}, "
    "frac rate {:d}({:d}), "
    "cs: {:d}({:d}), cd: {:d}({:d})",
    __FUNCTION__,
    m_bWindowCreated,
    fractional_rate, cur_fractional_rate,
    cs, m_cs, cd, m_cd);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_stereo_mode == stereo_mode && m_bWindowCreated &&
      ((m_cs != 0 && m_cd != 0) && (m_cs == cs && m_cd == cd)) &&
      (fractional_rate == cur_fractional_rate))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();

  // check if a forced mode switch is required
  if ((current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
       current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
       current_resolution.fRefreshRate == res.fRefreshRate) &&
       (((m_cs != 0 && m_cd != 0) && (m_cs != cs || m_cd != cd)) ||
       (fractional_rate != cur_fractional_rate)))
  {
    CSysfsPath crtc_force_hint{"/sys/module/aml_drm/parameters/crtc_force_hint"};
    if (crtc_force_hint.Exists())
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);
      crtc_force_hint.Set(1);
    }
  }

  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  if (!m_pGLContext.CreateSurface(static_cast<EGLNativeWindowType>(m_nativeWindow)))
  {
    return false;
  }

  if (!m_pGLContext.BindContext())
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

  // backup data after mode switch
  if (amhdmitx0_cs.Exists() && amhdmitx0_cd.Exists())
  {
    m_cs = amhdmitx0_cs.Get<int>().value();
    m_cd = amhdmitx0_cd.Get<int>().value();
  }

  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  m_pGLContext.DestroySurface();
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
  if (!m_pGLContext.SetVSync(enable))
  {
    CLog::Log(LOGERROR, "{},Could not set egl vsync", __FUNCTION__);
  }
}

void CWinSystemAmlogicGLESContext::PresentRenderImpl(bool rendered)
{
  if (m_delayDispReset && m_dispResetTimer.IsTimePast())
  {
    m_delayDispReset = false;
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }
  if (!rendered)
    return;

  // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
  // there is probably nothing we can do about it
  m_pGLContext.TrySwapBuffers();
}

EGLDisplay CWinSystemAmlogicGLESContext::GetEGLDisplay() const
{
  return m_pGLContext.GetEGLDisplay();
}

EGLSurface CWinSystemAmlogicGLESContext::GetEGLSurface() const
{
  return m_pGLContext.GetEGLSurface();
}

EGLContext CWinSystemAmlogicGLESContext::GetEGLContext() const
{
  return m_pGLContext.GetEGLContext();
}

EGLConfig  CWinSystemAmlogicGLESContext::GetEGLConfig() const
{
  return m_pGLContext.GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAmlogicGLESContext::GetVideoSync(void *clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAML(clock));
  return pVSync;
}
