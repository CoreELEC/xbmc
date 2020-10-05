/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OMXPlayerAudio.h"

#include <stdio.h>
#include <unistd.h>
#include <iomanip>

#include "platform/linux/XMemUtils.h"
#include "utils/BitstreamStats.h"

#include "DVDDemuxers/DVDDemuxUtils.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "utils/MathUtils.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "utils/TimeUtils.h"
#include "cores/VideoPlayer/Interface/Addon/TimingConstants.h"

#include "platform/linux/RBP.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/DataCacheCore.h"
#include "system.h"

#include <algorithm>
#include <iostream>
#include <sstream>

class COMXMsgAudioCodecChange : public CDVDMsg
{
public:
  COMXMsgAudioCodecChange(const CDVDStreamInfo &hints, COMXAudioCodecOMX* codec)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_hints(hints)
  {}
 ~COMXMsgAudioCodecChange()
  {
    delete m_codec;
  }
  COMXAudioCodecOMX   *m_codec;
  CDVDStreamInfo      m_hints;
};

OMXPlayerAudio::OMXPlayerAudio(OMXClock *av_clock, CDVDMessageQueue& parent, CProcessInfo &processInfo)
: CThread("OMXPlayerAudio"), IDVDStreamPlayerAudio(processInfo)
, m_messageQueue("audio")
, m_messageParent(parent)
, m_omxAudio(processInfo)
{
  m_av_clock      = av_clock;
  m_pAudioCodec   = NULL;
  m_speed         = DVD_PLAYSPEED_NORMAL;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_stalled       = false;
  m_audioClock    = DVD_NOPTS_VALUE;
  m_buffer_empty  = false;
  m_DecoderOpen   = false;
  m_bad_state     = false;
  m_hints_current.Clear();

  bool small_mem = g_RBP.GetArmMem() < 256;
  m_messageQueue.SetMaxDataSize((small_mem ? 3:6) * 1024 * 1024);

  m_messageQueue.SetMaxTimeSize(8.0);
  m_passthrough = false;
  m_flush = false;
}


OMXPlayerAudio::~OMXPlayerAudio()
{
  CloseStream(false);
}

bool OMXPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  m_bad_state = false;

  m_processInfo.ResetAudioCodecInfo();
  COMXAudioCodecOMX *codec = new COMXAudioCodecOMX(m_processInfo);

  if(!codec || !codec->Open(hints))
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    delete codec; codec = NULL;
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new COMXMsgAudioCodecChange(hints, codec), 0);
  else
  {
    OpenStream(hints, codec);
    m_messageQueue.Init();
    CLog::Log(LOGNOTICE, "Creating audio thread");
    Create();
  }

  return true;
}

void OMXPlayerAudio::OpenStream(CDVDStreamInfo &hints, COMXAudioCodecOMX *codec)
{
  SAFE_DELETE(m_pAudioCodec);

  m_hints           = hints;
  m_pAudioCodec     = codec;

  if(m_hints.bitspersample == 0)
    m_hints.bitspersample = 16;

  m_speed           = DVD_PLAYSPEED_NORMAL;
  m_audioClock      = DVD_NOPTS_VALUE;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_flush           = false;
  m_stalled         = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_format = GetDataFormat(m_hints);
  m_format.m_sampleRate    = 0;
  m_format.m_channelLayout = 0;

  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
}

void OMXPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0) m_messageQueue.WaitUntilEmpty();

  m_messageQueue.Abort();

  if(IsRunning())
    StopThread();

  m_messageQueue.End();

  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    delete m_pAudioCodec;
    m_pAudioCodec = NULL;
  }

  CloseDecoder();

  m_speed         = DVD_PLAYSPEED_NORMAL;
}

void OMXPlayerAudio::OnStartup()
{
}

void OMXPlayerAudio::OnExit()
{
  CLog::Log(LOGNOTICE, "thread end: OMXPlayerAudio::OnExit()");
}

bool OMXPlayerAudio::CodecChange()
{
  unsigned int old_bitrate = m_hints.bitrate;
  unsigned int new_bitrate = m_hints_current.bitrate;

  if(m_pAudioCodec)
  {
    m_hints.channels = m_pAudioCodec->GetChannels();
    m_hints.samplerate = m_pAudioCodec->GetSampleRate();
    m_hints.bitspersample = m_pAudioCodec->GetBitsPerSample();
  }

  /* only check bitrate changes on AV_CODEC_ID_DTS, AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3 */
  if(m_hints.codec != AV_CODEC_ID_DTS && m_hints.codec != AV_CODEC_ID_AC3 && m_hints.codec != AV_CODEC_ID_EAC3)
    new_bitrate = old_bitrate = 0;

  // for passthrough we only care about the codec and the samplerate
  bool minor_change = m_hints_current.channels       != m_hints.channels ||
                      m_hints_current.bitspersample  != m_hints.bitspersample ||
                      old_bitrate                    != new_bitrate;

  if(m_hints_current.codec          != m_hints.codec ||
     m_hints_current.samplerate     != m_hints.samplerate ||
     (!m_passthrough && minor_change) || !m_DecoderOpen)
  {
    m_hints_current = m_hints;

    m_processInfo.SetAudioSampleRate(m_hints.samplerate);
    m_processInfo.SetAudioBitsPerSample(m_hints.bitspersample);

    CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
    return true;
  }

  return false;
}

bool OMXPlayerAudio::Decode(DemuxPacket *pkt, bool bDropPacket, bool bTrickPlay)
{
  if(!pkt || m_bad_state || !m_pAudioCodec)
    return false;

  if(pkt->dts != DVD_NOPTS_VALUE)
    m_audioClock = pkt->dts;

  bool settings_changed = false;
  const uint8_t *data_dec = pkt->pData;
  int            data_len = pkt->iSize;

  if (bTrickPlay)
  {
    settings_changed = true;
  }
  else if(m_format.m_dataFormat != AE_FMT_RAW && !bDropPacket)
  {
    double dts = pkt->dts, pts=pkt->pts;
    while(!m_bStop && data_len > 0)
    {
      int len = m_pAudioCodec->Decode((unsigned char*)data_dec, data_len, dts, pts);
      if( (len < 0) || (len >  data_len) )
      {
        m_pAudioCodec->Reset();
        break;
      }

      data_dec+= len;
      data_len -= len;

      uint8_t *decoded;
      int decoded_size = m_pAudioCodec->GetData(&decoded, dts, pts);

      if(decoded_size <=0)
        continue;

      int ret = 0;

      m_audioStats.AddSampleBytes(decoded_size);

      if(CodecChange())
      {
        m_DecoderOpen = OpenDecoder();
        if(!m_DecoderOpen)
          return false;
      }

      while(!m_bStop)
      {
        // discard if flushing as clocks may be stopped and we'll never submit it
        if(m_flush)
          break;

        if(m_omxAudio.GetSpace() < (unsigned int)decoded_size)
        {
          Sleep(10);
          continue;
        }

        if(!bDropPacket)
        {
          ret = m_omxAudio.AddPackets(decoded, decoded_size, dts, pts, m_pAudioCodec->GetFrameSize(), settings_changed);
          if(ret != decoded_size)
          {
            CLog::Log(LOGERROR, "error ret %d decoded_size %d\n", ret, decoded_size);
          }
        }

        break;

      }
    }
  }
  else if(!bDropPacket)
  {
    if(CodecChange())
    {
      m_DecoderOpen = OpenDecoder();
      if(!m_DecoderOpen)
        return false;
    }

    while(!m_bStop)
    {
      if(m_flush)
        break;

      if(m_omxAudio.GetSpace() < (unsigned int)pkt->iSize)
      {
        Sleep(10);
        continue;
      }

      if(!bDropPacket)
      {
        m_omxAudio.AddPackets(pkt->pData, pkt->iSize, m_audioClock, m_audioClock, 0, settings_changed);
      }

      m_audioStats.AddSampleBytes(pkt->iSize);

      break;
    }
  }

  if(bDropPacket || bTrickPlay)
    m_stalled = false;

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING && !bDropPacket && settings_changed)
  {
    m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
    SStartMsg msg;
    msg.player = VideoPlayer_AUDIO;
    msg.cachetotal = DVD_SEC_TO_TIME(m_omxAudio.GetCacheTotal());
    msg.cachetime = DVD_SEC_TO_TIME(m_omxAudio.GetCacheTime());
    msg.timestamp = m_audioClock;
    m_messageParent.Put(new CDVDMsgType<SStartMsg>(CDVDMsg::PLAYER_STARTED, msg));
  }

  return true;
}

void OMXPlayerAudio::Process()
{
  m_audioStats.Start();

  while(!m_bStop)
  {
    CDVDMsg* pMsg;
    int timeout = 1000;

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_speed == DVD_PLAYSPEED_NORMAL || /* when playing normally */
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
       (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_av_clock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    // consider stream stalled if queue is empty
    // we can't sync audio to clock with an empty queue
    if (m_speed == DVD_PLAYSPEED_NORMAL)
    {
      timeout = 0;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, timeout, priority);

    if (ret == MSGQ_TIMEOUT)
    {
      Sleep(10);
      continue;
    }

    if (MSGQ_IS_ERROR(ret) || ret == MSGQ_ABORT)
    {
      Sleep(10);
      continue;
    }

    if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacket();
      bool bPacketDrop     = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacketDrop();

      #ifdef _DEBUG
      CLog::Log(LOGINFO, "Audio: dts:%.0f pts:%.0f size:%d (s:%d f:%d d:%d l:%d) s:%d %d/%d late:%d,%d", pPacket->dts, pPacket->pts,
           (int)pPacket->iSize, m_syncState, m_flush, bPacketDrop, m_stalled, m_speed, 0, 0, (int)m_omxAudio.GetAudioRenderingLatency(), (int)m_hints_current.samplerate);
      #endif
      if(Decode(pPacket, bPacketDrop, m_speed > DVD_PLAYSPEED_NORMAL || m_speed < 0))
      {
        // we are not running until something is cached in output device
        if(m_stalled && m_omxAudio.GetCacheTime() > 0.0)
        {
          CLog::Log(LOGINFO, "COMXPlayerAudio - Switching to normal playback");
          m_stalled = false;
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if(((CDVDMsgGeneralSynchronize*)pMsg)->Wait( 100, SYNCSOURCE_AUDIO ))
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg->Acquire(), 1); /* push back as prio message, to process other prio messages */
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = static_cast<CDVDMsgDouble*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f)", pts);

      m_audioClock = pts;
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_RESET");
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
      m_audioClock = DVD_NOPTS_VALUE;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_FLUSH(%d)", sync);
      m_omxAudio.Flush();
      m_stalled   = true;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_audioClock = DVD_NOPTS_VALUE;
      m_flush = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_EOF");
      SubmitEOS();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      if (m_speed != static_cast<CDVDMsgInt*>(pMsg)->m_value)
      {
        m_speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::PLAYER_SETSPEED %d", m_speed);
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      COMXMsgAudioCodecChange* msg(static_cast<COMXMsgAudioCodecChange*>(pMsg));
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_STREAMCHANGE");
      OpenStream(msg->m_hints, msg->m_codec);
      msg->m_codec = NULL;
    }

    pMsg->Release();
  }
}

void OMXPlayerAudio::Flush(bool sync)
{
  m_flush = true;
  m_messageQueue.Flush();
  m_messageQueue.Flush(CDVDMsg::GENERAL_EOF);
  m_messageQueue.Put( new CDVDMsgBool(CDVDMsg::GENERAL_FLUSH, sync), 1);
}

bool OMXPlayerAudio::IsPassthrough() const
{
  return m_passthrough;
}

AEAudioFormat OMXPlayerAudio::GetDataFormat(CDVDStreamInfo hints)
{
  AEAudioFormat format;
  format.m_dataFormat = AE_FMT_RAW;
  format.m_sampleRate = hints.samplerate;
  switch (hints.codec)
  {
    case AV_CODEC_ID_AC3:
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
      format.m_streamInfo.m_sampleRate = hints.samplerate;
      break;

    case AV_CODEC_ID_EAC3:
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
      format.m_streamInfo.m_sampleRate = hints.samplerate * 4;
      break;

    case AV_CODEC_ID_DTS:
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_DTSHD;
      format.m_streamInfo.m_sampleRate = hints.samplerate;
      break;

    case AV_CODEC_ID_TRUEHD:
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
      format.m_streamInfo.m_sampleRate = hints.samplerate;
      break;

    default:
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
  }

  m_passthrough = CServiceBroker::GetActiveAE()->SupportsRaw(format);

  if (!m_passthrough && hints.codec == AV_CODEC_ID_DTS)
  {
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
    m_passthrough = CServiceBroker::GetActiveAE()->SupportsRaw(format);
  }

  if(!m_passthrough)
  {
    if (m_pAudioCodec && m_pAudioCodec->GetBitsPerSample() == 16)
      format.m_dataFormat = AE_FMT_S16NE;
    else
      format.m_dataFormat = AE_FMT_FLOAT;
  }

  return format;
}

bool OMXPlayerAudio::OpenDecoder()
{
  m_passthrough = false;

  if(m_DecoderOpen)
  {
    m_omxAudio.Deinitialize();
    m_DecoderOpen = false;
  }

  /* setup audi format for audio render */
  m_format = GetDataFormat(m_hints);

  CAEChannelInfo channelMap;
  if (m_pAudioCodec && !m_passthrough)
  {
    channelMap = m_pAudioCodec->GetChannelMap();
  }
  else if (m_passthrough)
  {
    // we just want to get the channel count right to stop OMXAudio.cpp rejecting stream
    // the actual layout is not used
    channelMap = AE_CH_LAYOUT_5_1;

    if (m_hints.codec == AV_CODEC_ID_AC3)
      m_processInfo.SetAudioDecoderName("PT_AC3");
    else if (m_hints.codec == AV_CODEC_ID_EAC3)
      m_processInfo.SetAudioDecoderName("PT_EAC3");
    else
      m_processInfo.SetAudioDecoderName("PT_DTS");
  }
  m_processInfo.SetAudioChannels(channelMap);
  bool bAudioRenderOpen = m_omxAudio.Initialize(m_format, m_av_clock, m_hints, channelMap, m_passthrough);

  m_codec_name = "";
  m_bad_state  = !bAudioRenderOpen;

  if(!bAudioRenderOpen)
  {
    CLog::Log(LOGERROR, "OMXPlayerAudio : Error open audio output");
    m_omxAudio.Deinitialize();
  }
  else
  {
    CLog::Log(LOGINFO, "Audio codec %s channels %d samplerate %d bitspersample %d\n",
      m_codec_name.c_str(), m_hints.channels, m_hints.samplerate, m_hints.bitspersample);
  }

  return bAudioRenderOpen;
}

void OMXPlayerAudio::CloseDecoder()
{
  m_omxAudio.Deinitialize();
  m_DecoderOpen = false;
}

void OMXPlayerAudio::SubmitEOS()
{
  if(!m_bad_state)
    m_omxAudio.SubmitEOS();
}

bool OMXPlayerAudio::IsEOS()
{
  return m_bad_state || m_omxAudio.IsEOS();
}

void OMXPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

int OMXPlayerAudio::GetAudioChannels()
{
  return m_hints.channels;
}

std::string OMXPlayerAudio::GetPlayerInfo()
{
  std::ostringstream s;
  s << "aq:"     << std::setw(2) << std::min(99,m_messageQueue.GetLevel() + MathUtils::round_int(100.0/8.0*m_omxAudio.GetCacheTime())) << "%";
  s << ", Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;
  s << ", ac:"   << m_processInfo.GetAudioDecoderName().c_str();
  if (!m_passthrough)
    s << ", chan:" << m_processInfo.GetAudioChannels().c_str();
  s << ", " << m_processInfo.GetAudioSampleRate()/1000 << " kHz";

  return s.str();
}
