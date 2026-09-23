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
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

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

    case AVCOL_SPC_ICTCP:
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

  const auto winSystem = static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (color_transfer != 0)
  {
    m_hdrGuiOwner = winSystem->ConfigureHdrGuiSession(m_hdrGuiOwner, color_transfer, dv_graphics);
    if (m_hdrGuiOwner == 0)
      CLog::Log(LOGWARNING, "CRendererAML: HDR GUI composite unavailable; using normal GUI path");
  }
  else
  {
    winSystem->ReleaseHdrGuiSession(m_hdrGuiOwner);
    m_hdrGuiOwner = 0;
  }
  m_bConfigured = true;

  return true;
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

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  ManageRenderArea();

  CAMLVideoBuffer *amli = dynamic_cast<CAMLVideoBuffer *>(m_buffers[index].videoBuffer);
  if(amli && amli->m_amlCodec)
  {
    uint64_t pts = amli->m_omxPts;
    if (pts != m_prevVPts)
    {
      amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, m_prevVPts == DVD_NOPTS_VALUE);
      amli->m_amlCodec->SetVideoRect(m_sourceRect, m_destRect);
      amli->m_amlCodec = nullptr; //Mark frame as processed
      m_prevVPts = pts;
    }
  }
  CAMLCodec::PollFrame();
}
