/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <list>
#include <utility>

#include "AudioSinkAE.h"
#include "DVDClock.h"
#include "DVDMessageQueue.h"
#include "DVDStreamInfo.h"
#include "IVideoPlayer.h"
#include "cores/VideoPlayer/Interface/Addon/TimingConstants.h"
#include "threads/Thread.h"
#include "utils/BitstreamStats.h"


class CVideoPlayer;
class CDVDAudioCodec;
class CDVDAudioCodec;

class CVideoPlayerAudio : public CThread, public IDVDStreamPlayerAudio
{
public:
  CVideoPlayerAudio(CDVDClock* pClock, CDVDMessageQueue& parent, CProcessInfo &processInfo);
  ~CVideoPlayerAudio() override;

  bool OpenStream(CDVDStreamInfo hints) override;
  void CloseStream(bool bWaitForBuffers) override;

  void SetSpeed(int speed) override;
  void Flush(bool sync) override;

  // waits until all available data has been rendered
  bool AcceptsData() const override;
  bool HasData() const override { return m_messageQueue.GetDataSize() > 0; }
  int  GetLevel() const override { return (m_pts == DVD_NOPTS_VALUE) || (m_pts < m_pClock->GetClock() + 80000) ?
                                           m_messageQueue.GetLevel() : m_messageQueue.GetLevel() + int((m_pts - m_pClock->GetClock() - 80000) / 80000);}
  bool IsInited() const override { return m_messageQueue.IsInited(); }
  void SendMessage(CDVDMsg* pMsg, int priority = 0) override { m_messageQueue.Put(pMsg, priority); }
  void FlushMessages() override { m_messageQueue.Flush(); }

  void SetDynamicRangeCompression(long drc) override { m_audioSink.SetDynamicRangeCompression(drc); }
  float GetDynamicRangeAmplification() const override { return 0.0f; }

  std::string GetPlayerInfo() override;
  int GetAudioChannels() override;

  double GetCurrentPts() override { CSingleLock lock(m_info_section); return m_info.pts; }

  bool IsStalled() const override { return m_stalled;  }
  bool IsPassthrough() const override;

protected:

  void OnStartup() override;
  void OnExit() override;
  void Process() override;

  bool ProcessDecoderOutput(DVDAudioFrame &audioframe);
  void UpdatePlayerInfo();
  void OpenStream(CDVDStreamInfo &hints, CDVDAudioCodec* codec);
  //! Switch codec if needed. Called when the sample rate gotten from the
  //! codec changes, in which case we may want to switch passthrough on/off.
  bool SwitchCodecIfNeeded();
  void SetSyncType(bool passthrough);

  CDVDMessageQueue m_messageQueue;
  CDVDMessageQueue& m_messageParent;

  // holds stream information for current playing stream
  CDVDStreamInfo m_streaminfo;

  double m_audioClock;

  CAudioSinkAE m_audioSink; // audio output device
  CDVDClock* m_pClock; // dvd master clock
  CDVDAudioCodec* m_pAudioCodec; // audio codec
  BitstreamStats m_audioStats;

  int m_speed;
  bool m_stalled;
  bool m_paused;
  IDVDStreamPlayer::ESyncState m_syncState;
  XbmcThreads::EndTime m_syncTimer;

  int m_synctype;
  int m_prevsynctype;

  bool   m_prevskipped;
  double m_maxspeedadjust;
  double	   m_pts;

  struct SInfo
  {
    std::string      info;
    double           pts = DVD_NOPTS_VALUE;
    bool             passthrough = false;
  };

  mutable CCriticalSection m_info_section;
  SInfo            m_info;
};

