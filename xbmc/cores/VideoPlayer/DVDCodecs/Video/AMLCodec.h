/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/IPlayer.h"
#include "windowing/Resolution.h"
#include "rendering/RenderSystem.h"
#include "utils/BitstreamConverter.h"
#include "utils/Geometry.h"

#include <deque>
#include <mutex>
#include <atomic>

typedef struct am_private_t am_private_t;

class DllLibAmCodec;

class PosixFile;
typedef std::shared_ptr<PosixFile> PosixFilePtr;

class CProcessInfo;

struct vpp_pq_ctrl_s {
	unsigned int length;
	union {
		void *ptr;/*point to pq_ctrl_s*/
		long long ptr_length;
	};
};

struct pq_ctrl_s {
	unsigned char sharpness0_en;
	unsigned char sharpness1_en;
	unsigned char dnlp_en;
	unsigned char cm_en;
	unsigned char vadj1_en;
	unsigned char vd1_ctrst_en;
	unsigned char vadj2_en;
	unsigned char post_ctrst_en;
	unsigned char wb_en;
	unsigned char gamma_en;
	unsigned char lc_en;
	unsigned char black_ext_en;
	unsigned char chroma_cor_en;
	unsigned char reserved;
};

#define _VE_CM  'C'
#define AMVECM_IOC_S_PQ_CTRL  _IOW(_VE_CM, 0x69, struct vpp_pq_ctrl_s)
#define AMVECM_IOC_G_PQ_CTRL  _IOR(_VE_CM, 0x6a, struct vpp_pq_ctrl_s)

#define AMLVIDEO_IOC_GET_VFQ     _IOR('V', 0x01, int)

class CAMLCodec
{
public:
  CAMLCodec(CProcessInfo &processInfo);
  virtual ~CAMLCodec();

  bool          OpenDecoder(CDVDStreamInfo &hints, bool doviIsFEL);
  bool          Enable_vadj1();
  void          CloseDecoder();
  void          Reset();

  bool          AddData(uint8_t *pData, size_t size, double dts, double pts);
  int           AddHDR10PData(uint8_t *pData, size_t iSize);
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture);

  void          SetSpeed(int speed);
  void          SetDrain(bool drain){m_drain = drain;};
  void          SetDoviZeroLevel5(bool value);
  void          SetVideoRect(const CRect &SrcRect, const CRect &DestRect);
  void          SetVideoRate(int videoRate);
  uint64_t      GetOMXPts() const { return m_cur_pts; }
  uint32_t      GetBufferIndex() const { return m_bufferIndex; };
  float         GetBufferLevel(int new_chunk, int &data_len, int &free_len, int &size);
  static float  OMXPtsToSeconds(int omxpts);
  static int    OMXDurationToNs(int duration);
  int           GetAmlDuration() const;
  int           ReleaseFrame(const uint32_t index, bool bDrop = false);
  bool          IsOpen() const { return m_opened; }

  static int    PollFrame();
  static void   SetPollDevice(int device);

  enum class VsyncWake
  {
    VSYNC,
    STEP,
    TIMEOUT,
    NO_DEVICE,
    HANDOVER,
    INTERRUPTED,
    STOPPED,
  };
  //! Waits for the next amvideo vsync, interruptible by StopVsyncWait(),
  //! RequestVsyncStep() and a poll device change. Only one thread may wait while armed.
  //! After VSYNC, STEP or TIMEOUT the caller may pick until its next call, and a device
  //! change waits for that; a vsync that comes with a device change is HANDOVER, no pick.
  //! NO_DEVICE comes at once when the device goes, then once per bound without one.
  static VsyncWake PollVsync();
  //! Clears a vsync flag latched since the last wait; true if one was set
  static bool   ConsumeVsyncFlag();
  static bool   ArmVsyncWait();
  //! Ends the current or next wait with STEP; dropped without a poll device or in a change
  static void   RequestVsyncStep();
  static void   StopVsyncWait();
  //! Signals the end of a vsync thread step; `vsync` if it followed a real vsync
  static void   PublishPresentStep(bool vsync);
  static uint64_t PresentSteps();
  //! Waits for `fenceFd`, the out-fence of the GUI flip just issued (-1 for none), and for a
  //! step after `seen`. False while the vsync thread runs without a vsync to pace on (no poll
  //! device, missed vsyncs): the caller then waits for the vblank itself
  static bool   WaitPresentStep(uint64_t& seen, int fenceFd);

private:
  void          ShowMainVideo(const bool show);
  void          SetVideoZoom(const float zoom);
  void          SetVideoContrast(const int contrast);
  void          SetVideoBrightness(const int brightness);
  void          SetVideoSaturation(const int saturation);
  bool          OpenAmlVideo(const CDVDStreamInfo &hints);
  void          CloseAmlVideo();
  std::string   GetVfmMap(const std::string &name);
  void          SetVfmMap(const std::string &name, const std::string &map);
  int           DequeueBuffer();
  unsigned int  GetDecoderVideoRate();
  std::string   GetHDRStaticMetadata(bool dv_enable);

  DllLibAmCodec   *m_dll;
  std::atomic<bool> m_opened;
  bool             m_drain = false;
  am_private_t    *am_private;
  CDVDStreamInfo   m_hints;
  int              m_speed;
  uint64_t         m_cur_pts;
  uint64_t         m_last_pts;
  uint32_t         m_bufferIndex;

  CRect            m_dst_rect;
  CRect            m_display_rect;

  int              m_view_mode = -1;
  RenderStereoMode m_guiStereoMode = RenderStereoMode::OFF;
  RenderStereoView m_guiStereoView = RenderStereoView::OFF;
  float            m_zoom = -1.0f;
  int              m_contrast = -1;
  int              m_brightness = -1;
  bool             m_vadj1_enabled = false;
  RESOLUTION       m_video_res = RES_INVALID;

  static const unsigned int STATE_PREFILLED  = 1;
  static const unsigned int STATE_HASPTS     = 2;

  unsigned int m_state;

  PosixFilePtr     m_amlVideoFile;
  std::mutex       m_amlVideoFileMutex;
  std::string      m_defaultVfmMap;

  static std::atomic_flag  m_pollSync;
  static int m_pollDevice;
  static double m_ttd;
  CProcessInfo &m_processInfo;
  int m_decoder_timeout;
  std::chrono::time_point<std::chrono::system_clock> m_tp_last_frame;

  bool            m_buffer_level_ready;
  float           m_minimum_buffer_level;
};
