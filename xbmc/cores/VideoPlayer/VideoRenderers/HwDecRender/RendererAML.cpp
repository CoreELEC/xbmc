/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererAML.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Video/AMLCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

namespace
{
constexpr int AMDV_OUTPUT_MODE_BYPASS = 5;
} // namespace

CRendererAML::CRendererAML()
 : m_prevVPts(DVD_NOPTS_VALUE)
 , m_bConfigured(false)
{
  CLog::Log(LOGINFO, "Constructing CRendererAML");
}

CRendererAML::~CRendererAML()
{
  Reset();
  static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
      ->ReleaseHdrGuiSession(m_hdrGuiOwner);
}

CBaseRenderer* CRendererAML::Create(CVideoBuffer *buffer)
{
  if (buffer && dynamic_cast<CAMLVideoBuffer*>(buffer))
    return new CRendererAML();
  return nullptr;
}

bool CRendererAML::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("amlogic", CRendererAML::Create);
  return true;
}

bool CRendererAML::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  int color_transfer = 0;
  const bool dv_graphics =
      picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION && aml_dolby_vision_enabled();
  switch (picture.color_space)
  {
    case AVCOL_SPC_BT2020_NCL:
    {
      if (CServiceBroker::GetWinSystem()->IsHDRDisplay())
      {
        auto hdr_cap = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
        switch (picture.color_transfer)
        {
          case AVCOL_TRC_ARIB_STD_B67:
            if (hdr_cap.SupportsHLG())
            {
              color_transfer = AVCOL_TRC_ARIB_STD_B67;
              break;
            }
            [[fallthrough]];
          case AVCOL_TRC_SMPTE2084:
            if (hdr_cap.SupportsHDR10())
              color_transfer = AVCOL_TRC_SMPTE2084;
            break;
          default:
            break;
        }
      }
      break;
    }

    case AVCOL_SPC_IPT_C2:
    {
      auto hdr_cap = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
      switch (picture.color_transfer)
      {
        case AVCOL_TRC_SMPTE2084:
          if (hdr_cap.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE || hdr_cap.SupportsHDR10())
          {
            color_transfer = AVCOL_TRC_SMPTE2084;
          }
          break;
        default:
          break;
      }
      break;
    }
    default:
      break;
  }

  // dolby vision is PQ even when its stream color fields are unspecified
  if (dv_graphics)
    color_transfer = AVCOL_TRC_SMPTE2084;

  m_colorTransfer = color_transfer;
  m_dvGraphics = dv_graphics;
  m_dvCore = picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
             static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
                     ->GetAmlDisplay()
                     ->aml_get_drmProperty("dv_enable", DRM_MODE_OBJECT_CRTC) != 0;
  const int guiColorTransfer = GetGuiColorTransfer();
  UpdateHdrGuiSession(guiColorTransfer, GetGuiDvGraphics(guiColorTransfer));
  m_bConfigured = true;

  return true;
}

int CRendererAML::GetGuiColorTransfer() const
{
  // hdr2sdr tone maps the video and passes the GUI plane through unconverted,
  // unless the dolby vision core owns the output
  const auto winSystem = static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  return (m_dvCore || !winSystem->IsHdrToSdr()) ? m_colorTransfer : 0;
}

bool CRendererAML::GetGuiDvGraphics(int guiColorTransfer) const
{
  // kodi starts the dolby vision core for dolby vision titles only, so for any other
  // title an active core was forced on and converts the GUI plane by its declared format
  if (!m_dvGraphics && m_dvOutputActive)
    return guiColorTransfer == AVCOL_TRC_SMPTE2084;
  return m_dvGraphics;
}

void CRendererAML::UpdateHdrGuiSession(int colorTransfer, bool dvGraphics)
{
  m_guiColorTransfer = colorTransfer;
  m_guiDvGraphics = dvGraphics;

  const auto winSystem = static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (colorTransfer != 0)
  {
    m_hdrGuiOwner = winSystem->ConfigureHdrGuiSession(m_hdrGuiOwner, colorTransfer, dvGraphics);
    if (m_hdrGuiOwner == 0)
      CLog::Log(LOGWARNING, "CRendererAML: HDR GUI composite unavailable; using normal GUI path");
  }
  else
  {
    winSystem->ReleaseHdrGuiSession(m_hdrGuiOwner);
    m_hdrGuiOwner = 0;
  }
}

CRenderInfo CRendererAML::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_numRenderBuffers;
  info.opaque_pointer = (void *)this;
  return info;
}

void CRendererAML::AddVideoPicture(const VideoPicture &picture, int index)
{
  ReleaseBuffer(index);

  BUFFER &buf(m_buffers[index]);
  if (picture.videoBuffer)
  {
    buf.videoBuffer = picture.videoBuffer;
    buf.videoBuffer->Acquire();
  }
}

void CRendererAML::ReleaseBuffer(int idx)
{
  BUFFER &buf(m_buffers[idx]);
  if (buf.videoBuffer)
  {
    CAMLVideoBuffer *amli(dynamic_cast<CAMLVideoBuffer*>(buf.videoBuffer));
    if (amli)
    {
      if (amli->m_amlCodec)
      {
        amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, true);
        amli->m_amlCodec = nullptr; // Released
      }
      amli->Release();
    }
    buf.videoBuffer = nullptr;
  }
}

bool CRendererAML::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_NONLINSTRETCH ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_ROTATION)
    return true;

  return false;
}

void CRendererAML::Reset()
{
  std::array<int, 2> reset_arr[m_numRenderBuffers];
  m_prevVPts = DVD_NOPTS_VALUE;

  {
    std::lock_guard<std::mutex> lock(m_pendingGeometryLock);
    m_pendingGeometry.reset();
  }

  for (int i = 0 ; i < m_numRenderBuffers ; ++i)
  {
    reset_arr[i][0] = i;

    if (m_buffers[i].videoBuffer)
      reset_arr[i][1] = dynamic_cast<CAMLVideoBuffer *>(m_buffers[i].videoBuffer)->m_bufferIndex;
    else
      reset_arr[i][1] = 0;
  }

  std::sort(std::begin(reset_arr), std::end(reset_arr),
    [](const std::array<int, 2>& u, const std::array<int, 2>& v)
    {
      return u[1] < v[1];
    });

  for (int i = 0; i < m_numRenderBuffers; ++i)
  {
    if (m_buffers[reset_arr[i][0]].videoBuffer)
    {
      m_buffers[reset_arr[i][0]].videoBuffer->Release();
      m_buffers[reset_arr[i][0]].videoBuffer = nullptr;
    }
  }
}

bool CRendererAML::Flush(bool saveBuffers)
{
  if (!saveBuffers)
    Reset();
  return saveBuffers;
};

std::shared_ptr<CAMLCodec> CRendererAML::QueueFrame(int index, bool setVideoRect, bool* drop)
{
  CAMLVideoBuffer *amli = dynamic_cast<CAMLVideoBuffer *>(m_buffers[index].videoBuffer);
  if(amli && amli->m_amlCodec)
  {
    uint64_t pts = amli->m_omxPts;
    if (pts != m_prevVPts)
    {
      const bool dropFrame =
          m_prevVPts == DVD_NOPTS_VALUE && amli->m_amlCodec->IsRealtimeStream();
      if (drop)
        *drop = dropFrame;

      amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, dropFrame);
      if (setVideoRect)
        amli->m_amlCodec->SetVideoRect(m_sourceRect, m_destRect);
      std::shared_ptr<CAMLCodec> codec = std::move(amli->m_amlCodec); //Mark frame as processed
      m_prevVPts = pts;
      return codec;
    }
  }
  return nullptr;
}

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  ManageRenderArea();
  FollowOutputMode();

  if (m_vsyncPresent)
  {
    std::shared_ptr<CAMLCodec> codec;
    {
      std::lock_guard<std::mutex> lock(m_pendingGeometryLock);
      codec.swap(m_pendingGeometry);
    }
    if (codec && codec->IsOpen())
      codec->SetVideoRect(m_sourceRect, m_destRect);
    // a pending codec means the vsync thread queued a new frame
    if (codec)
      FollowGuiColorTransfer();
    return;
  }

  if (QueueFrame(index, true))
    FollowGuiColorTransfer();
  CAMLCodec::PollFrame();
}

void CRendererAML::FollowGuiColorTransfer()
{
  // the driver applies hdr_mode on new frames only, so switch the GUI with it
  const int guiColorTransfer = GetGuiColorTransfer();
  const bool guiDvGraphics = GetGuiDvGraphics(guiColorTransfer);
  if (guiColorTransfer != m_guiColorTransfer || guiDvGraphics != m_guiDvGraphics)
  {
    UpdateHdrGuiSession(guiColorTransfer, guiDvGraphics);
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
  }
}

void CRendererAML::FollowOutputMode()
{
  // the dolby vision policy can force its output during playback, outside kodi
  const auto winSystem = static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  const int dvOutputMode = winSystem->GetDvOutputMode();
  if (dvOutputMode == m_dvOutputMode)
    return;

  m_dvOutputMode = dvOutputMode;
  m_dvOutputActive =
      dvOutputMode != -1 && dvOutputMode != AMDV_OUTPUT_MODE_BYPASS && aml_dolby_vision_enabled();

  const bool guiDvGraphics = GetGuiDvGraphics(m_guiColorTransfer);
  if (guiDvGraphics != m_guiDvGraphics)
  {
    UpdateHdrGuiSession(m_guiColorTransfer, guiDvGraphics);
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
  }
}

bool CRendererAML::StartVsyncPresent()
{
  m_lastWakeVsync = false;
  m_vsyncPresent = CAMLCodec::ArmVsyncWait();
  return m_vsyncPresent;
}

void CRendererAML::StopVsyncPresent()
{
  m_vsyncPresent = false;
  CAMLCodec::StopVsyncWait();
}

bool CRendererAML::WaitVsync()
{
  CAMLCodec::PublishPresentStep(m_lastWakeVsync);
  for (;;)
  {
    const CAMLCodec::VsyncWake wake = CAMLCodec::PollVsync();
    switch (wake)
    {
      case CAMLCodec::VsyncWake::STOPPED:
        return false;
      case CAMLCodec::VsyncWake::VSYNC:
      case CAMLCodec::VsyncWake::TIMEOUT:
        m_lastWakeVsync = wake == CAMLCodec::VsyncWake::VSYNC;
        return true;
      case CAMLCodec::VsyncWake::STEP:
        // a requested step leaves the GUI pacing as it is
        return true;
      case CAMLCodec::VsyncWake::HANDOVER:
      case CAMLCodec::VsyncWake::NO_DEVICE:
        // no frame goes out and the first vsync after the handover picks again; only a
        // real vsync keeps the GUI paced
        m_lastWakeVsync = wake == CAMLCodec::VsyncWake::HANDOVER;
        CAMLCodec::PublishPresentStep(m_lastWakeVsync);
        break;
      case CAMLCodec::VsyncWake::INTERRUPTED:
        break;
    }
  }
}

void CRendererAML::WakeVsyncPresent()
{
  CAMLCodec::RequestVsyncStep();
}

void CRendererAML::PresentFrame(int index)
{
  // the first frame after a reset is queued as a drop on live stream and never shows
  bool drop = false;
  std::shared_ptr<CAMLCodec> codec = QueueFrame(index, false, &drop);
  if (!codec)
    return;

  // a vsync that fired during this step must not wake the next one: hold, never double.
  // A drop never shows, so it does not occupy the next vsync.
  if (!drop)
  {
    CAMLCodec::ConsumeVsyncFlag();
    CAMLCodec::NotePresented();
  }

  std::lock_guard<std::mutex> lock(m_pendingGeometryLock);
  m_pendingGeometry.swap(codec);
}
