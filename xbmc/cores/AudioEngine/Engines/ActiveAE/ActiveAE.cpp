/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ActiveAE.h"

#include "ActiveAEDeviceChange.h"
#include "ActiveAESettings.h"
#include "ActiveAESound.h"
#include "ActiveAEStream.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/AEResampleFactory.h"
#include "cores/AudioEngine/Encoders/AEEncoderFFmpeg.h"
#include "cores/AudioEngine/Interfaces/IAudioCallback.h"
#include "cores/AudioEngine/Utils/AEPassthroughSilence.h"
#include "cores/AudioEngine/Utils/AEStreamData.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/DataCacheCore.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>

using namespace AE;
using namespace ActiveAE;

using namespace std::chrono_literals;

namespace
{
constexpr float MAX_CACHE_LEVEL = 0.4f; // total cache time of stream in seconds;
constexpr float MAX_WATER_LEVEL = 0.2f; // buffered time after stream stages in seconds;
constexpr float MIN_WATER_LEVEL = 0.02f; // min buffer time to prevent underrun
constexpr float MIN_WATER_LEVEL_RESAMPLE = 0.1f; // min buffer time in resample mode
constexpr float BUFFER_LEVEL_INCREMENT = 0.0001f; // increment step for ramp-up
constexpr double MAX_BUFFER_TIME = 0.1; // max time of a buffer in seconds;
constexpr double MAX_LATE_START_MS = 500.0; // late start of audio left to the player's clock
// the spread of the start sync measurement over many starts on every SoC; a start on the frame grid
// aims no closer
constexpr double START_GRID_TOLERANCE_MS = 2.0;
// the longest picture hold that brings a start on the frame grid within the tolerance at any
// standard display rate (21 vsyncs at 29.97 Hz)
constexpr double MAX_START_HOLD_MS = 701.0;

//! \brief Retry interval for a sink that will not open, doubling to a ceiling.
//! A device that returns announces itself, so only a fault that clears by itself needs the
//! interval to stay short.
std::chrono::milliseconds ErrorRetryDelay(unsigned int consecutiveFailures)
{
  constexpr std::chrono::milliseconds FIRST{500};
  constexpr std::chrono::milliseconds CEILING{30000};

  const unsigned int doublings = std::min(consecutiveFailures ? consecutiveFailures - 1 : 0u, 8u);
  const std::chrono::milliseconds delay{FIRST.count() * (1LL << doublings)};
  return std::min(delay, CEILING);
}

bool IsDefaultDevice(const AESinkDevice& device)
{
  return StringUtils::EqualsNoCase(device.name, "default");
}

bool IsSameDevice(const std::string& driver, const std::string& name, const AESinkDevice& device)
{
  return driver == device.driver && name == device.name;
}

bool IsPreferredDeviceAvailable(const AESinkDevice& configured, const AESinkDevice& validated)
{
  if (configured.driver != validated.driver)
    return false;

  if (configured.name == validated.name)
    return true;

  return !configured.friendlyName.empty() && !validated.friendlyName.empty() &&
         configured.friendlyName == validated.friendlyName;
}
} // unnamed namespace

void CEngineStats::Reset(unsigned int sampleRate, bool pcm)
{
  std::unique_lock lock(m_lock);
  m_sinkSampleRate = sampleRate;
  m_bufferedSamples = 0;
  m_bufferedPauseMs = 0;
  m_suspended = false;
  m_pcmOutput = pcm;
}

void CEngineStats::UpdateSinkDelay(const AEDelayStatus& status, int samples, int pauseMs)
{
  std::unique_lock lock(m_lock);
  m_sinkDelay = status;
  if (samples > 0 || pauseMs > 0)
    m_sinkReports++;
  if (samples > m_bufferedSamples || pauseMs > m_bufferedPauseMs)
  {
    CLog::Log(LOGERROR, "CEngineStats::UpdateSinkDelay - inconsistency in buffer time");
  }
  else
  {
    m_bufferedSamples -= samples;
    m_bufferedPauseMs -= pauseMs;
  }
}

void CEngineStats::AddSamples(int samples, const std::list<CActiveAEStream*>& streams, int pauseMs)
{
  std::unique_lock lock(m_lock);
  m_bufferedSamples += samples;
  m_bufferedPauseMs += pauseMs;

  for (auto stream : streams)
  {
    UpdateStream(stream);
  }
}

void CEngineStats::GetDelay(AEDelayStatus& status)
{
  std::unique_lock lock(m_lock);
  status = m_sinkDelay;
  if (m_pcmOutput)
    status.delay += static_cast<double>(m_bufferedSamples) / m_sinkSampleRate;
  else
    status.delay +=
        static_cast<double>(m_bufferedSamples) * m_sinkFormat.m_streamInfo.GetDuration() / 1000 +
        static_cast<double>(m_bufferedPauseMs) / 1000;
}

unsigned int CEngineStats::GetSinkReports()
{
  return m_sinkReports;
}

void CEngineStats::AddStream(unsigned int streamid)
{
  StreamStats stream;
  stream.m_streamId = streamid;
  stream.m_bufferedTime = 0;
  stream.m_resampleRatio = 1.0;
  stream.m_syncError = 0;
  stream.m_syncState = CAESyncInfo::AESyncState::SYNC_OFF;
  m_streamStats.push_back(stream);
}

void CEngineStats::RemoveStream(unsigned int streamid)
{
  for (auto it = m_streamStats.begin(); it != m_streamStats.end(); ++it)
  {
    if (it->m_streamId == streamid)
    {
      m_streamStats.erase(it);
      return;
    }
  }
}

void CEngineStats::UpdateStream(CActiveAEStream *stream)
{
  std::unique_lock lock(m_lock);
  for (auto &str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      float delay = 0;
      // a stream paused by its player is not in sync before the engine restarted its sync
      if (!str.m_syncPaused)
        str.m_syncState = stream->m_syncState;
      str.m_lateStartHandedOver = stream->m_lateStartHandedOver;
      str.m_exactStart = stream->m_exactStart;
      str.m_syncError = stream->m_syncError.GetLastError(str.m_errorTime);
      if (stream->m_processingBuffers)
      {
        str.m_resampleRatio = stream->m_processingBuffers->GetRR();
        delay += stream->m_processingBuffers->GetDelay();
      }
      else
      {
        str.m_resampleRatio = 1.0;
      }

      std::unique_lock lock(stream->m_statsLock);
      std::deque<CSampleBuffer*>::iterator itBuf;
      for(itBuf=stream->m_processingSamples.begin(); itBuf!=stream->m_processingSamples.end(); ++itBuf)
      {
        if (m_pcmOutput)
          delay += (float)(*itBuf)->pkt->nb_samples / (*itBuf)->pkt->config.sample_rate;
        else if ((*itBuf)->pkt->nb_samples > 0)
          delay += static_cast<float>(m_sinkFormat.m_streamInfo.GetDuration() / 1000.0);
      }
      str.m_bufferedTime = static_cast<double>(delay);
      stream->m_bufferedTime = 0;
      break;
    }
  }
}

void CEngineStats::PauseStream(CActiveAEStream* stream)
{
  std::unique_lock lock(m_lock);
  for (auto& str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      str.m_syncState = CAESyncInfo::AESyncState::SYNC_START;
      str.m_syncPaused = true;
      return;
    }
  }
}

bool CEngineStats::TakeSyncPaused(CActiveAEStream* stream)
{
  std::unique_lock lock(m_lock);
  for (auto& str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      const bool paused = str.m_syncPaused;
      str.m_syncPaused = false;
      return paused;
    }
  }
  return false;
}

// this is used to sync a/v so we need to add sink latency here
void CEngineStats::GetDelay(AEDelayStatus& status, CActiveAEStream *stream)
{
  std::unique_lock lock(m_lock);
  status = m_sinkDelay;
  status.delay += static_cast<double>(m_sinkLatency);
  if (m_pcmOutput)
    status.delay += static_cast<double>(m_bufferedSamples) / m_sinkSampleRate;
  else
    status.delay +=
        static_cast<double>(m_bufferedSamples) * m_sinkFormat.m_streamInfo.GetDuration() / 1000 +
        static_cast<double>(m_bufferedPauseMs) / 1000;

  for (auto &str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      std::unique_lock lock(stream->m_statsLock);
      float buffertime = static_cast<float>(str.m_bufferedTime) + stream->m_bufferedTime;
      status.delay += static_cast<double>(buffertime) * str.m_resampleRatio;
      return;
    }
  }
}

// this is used to sync a/v so we need to add sink latency here
void CEngineStats::GetSyncInfo(CAESyncInfo& info, CActiveAEStream *stream)
{
  std::unique_lock lock(m_lock);
  AEDelayStatus status;
  status = m_sinkDelay;
  if (m_pcmOutput)
    status.delay += static_cast<double>(m_bufferedSamples) / m_sinkSampleRate;
  else
    status.delay +=
        static_cast<double>(m_bufferedSamples) * m_sinkFormat.m_streamInfo.GetDuration() / 1000 +
        static_cast<double>(m_bufferedPauseMs) / 1000;

  status.delay += static_cast<double>(m_sinkLatency);

  for (auto &str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      std::unique_lock lock(stream->m_statsLock);
      float buffertime = static_cast<float>(str.m_bufferedTime) + stream->m_bufferedTime;
      status.delay += static_cast<double>(buffertime) * str.m_resampleRatio;
      info.delay = status.GetDelay();
      info.error = str.m_syncError;
      info.errortime = str.m_errorTime;
      info.state = str.m_syncState;
      info.lateStartHandedOver = str.m_lateStartHandedOver;
      info.exactStart = str.m_exactStart;
      info.stamped = m_outputStamped;
      info.rr = str.m_resampleRatio;
      str.m_errorTimeTaken = str.m_errorTime;
      return;
    }
  }
}

bool CEngineStats::IsSyncErrorTaken(CActiveAEStream* stream)
{
  std::unique_lock lock(m_lock);
  for (const auto& str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
      return str.m_errorTimeTaken == str.m_errorTime;
  }
  return true;
}

float CEngineStats::GetCacheTime(CActiveAEStream *stream)
{
  std::unique_lock lock(m_lock);
  float delay = 0;

  for (auto &str : m_streamStats)
  {
    if (str.m_streamId == stream->m_id)
    {
      std::unique_lock lock(stream->m_statsLock);
      float buffertime = static_cast<float>(str.m_bufferedTime) + stream->m_bufferedTime;
      delay += buffertime * static_cast<float>(str.m_resampleRatio);
      break;
    }
  }
  return delay;
}

float CEngineStats::GetCacheTotal()
{
  return MAX_CACHE_LEVEL;
}

float CEngineStats::GetMaxDelay() const
{
  return MAX_CACHE_LEVEL + MAX_WATER_LEVEL + m_sinkCacheTotal;
}

float CEngineStats::GetWaterLevel()
{
  std::unique_lock lock(m_lock);
  if (m_pcmOutput)
    return static_cast<float>(m_bufferedSamples) / m_sinkSampleRate;
  else
    return static_cast<float>(static_cast<double>(m_bufferedSamples) *
                                  m_sinkFormat.m_streamInfo.GetDuration() / 1000.0 +
                              static_cast<double>(m_bufferedPauseMs) / 1000.0);
}

void CEngineStats::SetSuspended(bool state)
{
  std::unique_lock lock(m_lock);
  m_suspended = state;
}

bool CEngineStats::IsSuspended()
{
  std::unique_lock lock(m_lock);
  return m_suspended;
}

void CEngineStats::SetCurrentSinkFormat(const AEAudioFormat& SinkFormat)
{
  std::unique_lock lock(m_lock);
  m_sinkFormat = SinkFormat;
}

AEAudioFormat CEngineStats::GetCurrentSinkFormat()
{
  std::unique_lock lock(m_lock);
  return m_sinkFormat;
}

CActiveAE::CActiveAE() :
  CThread("ActiveAE"),
  m_controlPort("OutputControlPort", &m_inMsgEvent, &m_outMsgEvent),
  m_dataPort("OutputDataPort", &m_inMsgEvent, &m_outMsgEvent),
  m_sink(&m_outMsgEvent)
{
  m_volume = 1.0;
  m_volumeScaled = 1.0;
  m_aeVolume = 1.0;
  m_muted = false;
  m_aeMuted = false;
  m_mode = MODE_PCM;
  m_encoder = NULL;
  m_vizInitialized = false;
  m_sinkHasVolume = false;
  m_aeGUISoundForce = false;
  m_stats.Reset(48000, true);
  m_streamIdGen = 0;

  m_settingsHandler = std::make_unique<CActiveAESettings>(*this);
}

CActiveAE::~CActiveAE()
{
  m_settingsHandler.reset();

  Dispose();
}

void CActiveAE::Dispose()
{
  if (m_isWinSysReg)
  {
    CWinSystemBase *winsystem = CServiceBroker::GetWinSystem();
    if (winsystem)
      winsystem->Unregister(this);
    m_isWinSysReg = false;
  }

  m_bStop = true;
  m_outMsgEvent.Set();
  StopThread();
  m_controlPort.Purge();
  m_dataPort.Purge();
  m_sink.Dispose();
}

//-----------------------------------------------------------------------------
// Behavior
//-----------------------------------------------------------------------------

enum AE_STATES
{
  AE_TOP = 0,                      // 0
  AE_TOP_WAIT_PRECOND,             // 1
  AE_TOP_ERROR,                    // 2
  AE_TOP_UNCONFIGURED,             // 3
  AE_TOP_RECONFIGURING,            // 4
  AE_TOP_CONFIGURED,               // 5
  AE_TOP_CONFIGURED_SUSPEND,       // 6
  AE_TOP_CONFIGURED_IDLE,          // 6
  AE_TOP_CONFIGURED_PLAY,          // 7
};

int AE_parentStates[] = {
    -1,
    0, //TOP_ERROR
    0, //AE_TOP_WAIT_PRECOND
    0, //TOP_UNCONFIGURED
    0, //TOP_CONFIGURED
    0, //TOP_RECONFIGURING
    5, //TOP_CONFIGURED_SUSPEND
    5, //TOP_CONFIGURED_IDLE
    5, //TOP_CONFIGURED_PLAY
};

void CActiveAE::StateMachine(int signal, Protocol *port, Message *msg)
{
  for (int state = m_state; ; state = AE_parentStates[state])
  {
    switch (state)
    {
    case AE_TOP: // TOP
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::GETSTATE:
          msg->Reply(CActiveAEControlProtocol::ACC, &m_state, sizeof(m_state));
          return;
        case CActiveAEControlProtocol::VOLUME:
          m_volume = *(float*)msg->data;
          m_volumeScaled = CAEUtil::GainToScale(CAEUtil::PercentToGain(m_volume));
          if (m_sinkHasVolume)
            m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::VOLUME, &m_volume, sizeof(float));
          return;
        case CActiveAEControlProtocol::MUTE:
          m_muted = *(bool*)msg->data;
          return;
        case CActiveAEControlProtocol::KEEPCONFIG:
          m_extKeepConfig = std::chrono::milliseconds(*reinterpret_cast<unsigned int*>(msg->data));
          return;
        case CActiveAEControlProtocol::DISPLAYRESET:
          return;
        case CActiveAEControlProtocol::APPFOCUSED:
          m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::APPFOCUSED, msg->data, sizeof(bool));
          return;
        case CActiveAEControlProtocol::YIELDDEVICE:
        {
          Message* sinkReply = nullptr;
          bool success = false;
          if (m_sink.m_controlPort.SendOutMessageSync(CSinkControlProtocol::RESERVE, &sinkReply, 1s,
                                                      msg->data, sizeof(bool)))
          {
            success = sinkReply->signal == CSinkControlProtocol::ACC;
            sinkReply->Release();
          }
          if (!success)
            CLog::LogF(LOGERROR, "sink failed to handle the yield request");
          msg->Reply(success ? CActiveAEControlProtocol::ACC : CActiveAEControlProtocol::ERR);
          return;
        }
        case CActiveAEControlProtocol::STREAMRESAMPLEMODE:
          MsgStreamParameter *par;
          par = reinterpret_cast<MsgStreamParameter*>(msg->data);
          if (par->stream)
          {
            par->stream->m_resampleMode = par->parameter.int_par;
            par->stream->m_resampleIntegral = 0.0;
          }
          ConfigureLowLatency();
          return;
        case CActiveAEControlProtocol::RESUMESTREAM:
        {
          // a resume dropped while not configured still ends the player's pause
          CActiveAEStream* stream = *reinterpret_cast<CActiveAEStream**>(msg->data);
          if (m_stats.TakeSyncPaused(stream) || stream->m_paused)
            stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
          stream->m_paused = false;
          break;
        }
        default:
          break;
        }
      }
      else if (port == &m_dataPort)
      {
        switch (signal)
        {
        case CActiveAEDataProtocol::NEWSOUND:
          CActiveAESound *sound;
          sound = *(CActiveAESound**)msg->data;
          if (sound)
          {
            m_sounds.push_back(sound);
            ResampleSounds();
          }
          return;
        case CActiveAEDataProtocol::FREESTREAM:
          MsgStreamFree *msgStreamFree;
          msgStreamFree = reinterpret_cast<MsgStreamFree*>(msg->data);
          DiscardStream(msgStreamFree->stream);
          msg->Reply(CActiveAEDataProtocol::ACC);
          return;
        case CActiveAEDataProtocol::FREESOUND:
          sound = *(CActiveAESound**)msg->data;
          DiscardSound(sound);
          return;
        case CActiveAEDataProtocol::DRAINSTREAM:
          CActiveAEStream *stream;
          stream = *(CActiveAEStream**)msg->data;
          stream->m_drain = true;
          stream->m_processingBuffers->SetDrain(true);
          msg->Reply(CActiveAEDataProtocol::ACC);
          stream->m_streamPort->SendInMessage(CActiveAEDataProtocol::STREAMDRAINED);
          return;
        default:
          break;
        }
      }
      else if (port == &m_sink.m_dataPort)
      {
        switch (signal)
        {
        case CSinkDataProtocol::RETURNSAMPLE:
          CSampleBuffer **buffer;
          buffer = (CSampleBuffer**)msg->data;
          if (buffer)
          {
            (*buffer)->Return();
          }
          return;
        default:
          break;
        }
      }
      {
        std::string portName = port == NULL ? "timer" : port->portName;
        CLog::Log(LOGWARNING, "CActiveAE::{} - signal: {} from port: {} not handled for state: {}",
                  __FUNCTION__, signal, portName, m_state);
      }
      return;

    case AE_TOP_WAIT_PRECOND:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
          case CActiveAEControlProtocol::INIT:
            LoadSettings();
            if (!m_settings.device.empty() && CAESinkFactory::HasSinks())
            {
              m_state = AE_TOP_UNCONFIGURED;
              m_bStateMachineSelfTrigger = true;
            }
            else
            {
              // Application can't handle error case and work without an AE
              msg->Reply(CActiveAEControlProtocol::ACC);
            }
            return;

          case CActiveAEControlProtocol::DEVICECHANGE:
          case CActiveAEControlProtocol::DEFAULTDEVICECHANGE:
          case CActiveAEControlProtocol::DEVICECOUNTCHANGE:
            LoadSettings();
            if (!m_settings.device.empty() && CAESinkFactory::HasSinks())
            {
              m_controlPort.SendOutMessage(CActiveAEControlProtocol::INIT);
            }
            return;

          default:
            break;
        }
      }
      break;

    case AE_TOP_ERROR:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
          case CActiveAEControlProtocol::DEVICECHANGE:
          case CActiveAEControlProtocol::DEFAULTDEVICECHANGE:
          case CActiveAEControlProtocol::DEVICECOUNTCHANGE:
            if (m_extTimeout != 0ms)
            {
              m_sink.EnumerateSinkList(true, "");
              m_extErrorRetries = 0;
              m_extTimeout = 0ms;
            }
            return;
          case CActiveAEControlProtocol::RECONFIGURE:
            m_extErrorRetries = 0;
            m_extTimeout = 0ms;
            return;
          default:
            break;
        }
      }
      else if (port == NULL) // timeout
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::TIMEOUT:
          m_extError = false;
          LoadSettings();
          Configure();
          if (!m_extError)
          {
            m_extErrorRetries = 0;
            m_state = AE_TOP_CONFIGURED_PLAY;
            m_extTimeout = 0ms;
          }
          else
          {
            m_state = AE_TOP_ERROR;
            m_extTimeout = ErrorRetryDelay(++m_extErrorRetries);
          }
          return;
        default:
          break;
        }
      }
      break;

    case AE_TOP_UNCONFIGURED:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::INIT:
          m_extError = false;
          m_sink.EnumerateSinkList(false, "");
          LoadSettings();
          ValidateOutputDevices(true);
          Configure();
          if (!m_isWinSysReg)
          {
            CWinSystemBase *winsystem = CServiceBroker::GetWinSystem();
            if (winsystem)
            {
              winsystem->Register(this);
              m_isWinSysReg = true;
            }
          }
          msg->Reply(CActiveAEControlProtocol::ACC);
          if (!m_extError)
          {
            m_state = AE_TOP_CONFIGURED_IDLE;
            m_extTimeout = 0ms;
          }
          else
          {
            m_state = AE_TOP_ERROR;
            m_extTimeout = 500ms;
          }
          return;

        default:
          break;
        }
      }
      break;

    case AE_TOP_RECONFIGURING:
      if (port == NULL) // timeout
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::TIMEOUT:
          // drain
          if (RunStages())
          {
            m_extTimeout = 0ms;
            return;
          }
          if (!m_sinkBuffers->m_inputSamples.empty() || !m_sinkBuffers->m_outputSamples.empty())
          {
            m_extTimeout = 100ms;
            return;
          }
          if (NeedReconfigureSink())
            DrainSink();

          if (!m_extError)
            Configure();
          if (!m_extError)
          {
            m_state = AE_TOP_CONFIGURED_PLAY;
            m_extTimeout = 0ms;
          }
          else
          {
            EndStartTargets();
            m_state = AE_TOP_ERROR;
            m_extTimeout = 500ms;
          }
          m_extDeferData = false;
          return;
        default:
          break;
        }
      }
      break;

    case AE_TOP_CONFIGURED:
      if (port == &m_controlPort)
      {
        bool streaming;
        switch (signal)
        {
        case CActiveAEControlProtocol::RECONFIGURE:
        {
          if (m_streams.empty())
          {
            streaming = false;
            m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::STREAMING, &streaming, sizeof(bool));
          }
          LoadSettings();
          m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::SETNOISETYPE, &m_settings.streamNoise, sizeof(bool));
          m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::SETSILENCETIMEOUT,
                                              &m_settings.silenceTimeoutMinutes, sizeof(int));
          ChangeResamplers();
          if (!NeedReconfigureBuffers() && !NeedReconfigureSink())
            return;
          m_state = AE_TOP_RECONFIGURING;
          m_extTimeout = 0ms;
          // don't accept any data until we are reconfigured
          m_extDeferData = true;
          return;
        }
        case CActiveAEControlProtocol::SUSPEND:
          UnconfigureSink();
          m_stats.SetSuspended(true);
          m_state = AE_TOP_CONFIGURED_SUSPEND;
          m_extDeferData = true;
          m_extSuspended = true;
          return;
        case CActiveAEControlProtocol::DISPLAYLOST:
          if (m_sink.GetDeviceType(m_mode == MODE_PCM ? m_settings.device : m_settings.passthroughdevice) == AE_DEVTYPE_HDMI)
          {
            UnconfigureSink();
            m_stats.SetSuspended(true);
            m_state = AE_TOP_CONFIGURED_SUSPEND;
            m_extDeferData = true;
          }
          msg->Reply(CActiveAEControlProtocol::ACC);
          return;
        case CActiveAEControlProtocol::DEVICECHANGE:
          time_t now;
          time(&now);
          CLog::Log(LOGDEBUG,"CActiveAE - device change event");
          while (!m_extLastDeviceChange.empty() && (now - m_extLastDeviceChange.front() > 0))
          {
            m_extLastDeviceChange.pop();
          }
          if (m_extLastDeviceChange.size() > 2)
          {
            CLog::Log(LOGWARNING, "CActiveAE - received {} device change events within one second",
                      m_extLastDeviceChange.size());
            return;
          }
          m_extLastDeviceChange.push(now);
          UnconfigureSink();
          m_controlPort.PurgeOut(CActiveAEControlProtocol::DEVICECHANGE);
          m_sink.EnumerateSinkList(true, "");
          LoadSettings();
          ValidateOutputDevices(false);
          m_extErrorRetries = 0;
          m_extError = false;
          Configure();
          if (!m_extError)
          {
            m_state = AE_TOP_CONFIGURED_PLAY;
            m_extTimeout = 0ms;
          }
          else
          {
            m_state = AE_TOP_ERROR;
            m_extTimeout = 500ms;
          }
          return;
        case CActiveAEControlProtocol::DEVICECOUNTCHANGE:
          HandleDeviceCountChange(reinterpret_cast<const char*>(msg->data), false);
          return;
        case CActiveAEControlProtocol::DEFAULTDEVICECHANGE:
          HandleDeviceCountChange("", true);
          return;
        case CActiveAEControlProtocol::PAUSESTREAM:
          CActiveAEStream *stream;
          stream = *(CActiveAEStream**)msg->data;
          if (!stream->m_paused && m_streams.size() == 1)
          {
            // A RAW AudioTrack would otherwise play its queued audio on resume, ahead of the sync
            // correction. Discarding it makes resume start on an empty sink, as after a seek. As
            // that lengthens the gap on resume, only where the queued audio disturbs it.
            const CAEStreamInfo::DataType type{m_sinkFormat.m_streamInfo.m_type};
            const bool discard =
                m_mode == MODE_TRANSCODE ||
                (m_mode == MODE_RAW && (type == CAEStreamInfo::STREAM_TYPE_TRUEHD ||
                                        type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
                                        type == CAEStreamInfo::STREAM_TYPE_DTSHD));
            FlushEngine(discard);
            streaming = false;
            m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::STREAMING, &streaming, sizeof(bool));
          }
          stream->m_paused = true;
          return;
        case CActiveAEControlProtocol::RESUMESTREAM:
          stream = *(CActiveAEStream**)msg->data;
          if (m_stats.TakeSyncPaused(stream) || stream->m_paused)
            stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
          stream->m_paused = false;
          streaming = true;
          m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::STREAMING, &streaming, sizeof(bool));
          m_extTimeout = 0ms;
          return;
        case CActiveAEControlProtocol::FLUSHSTREAM:
          stream = *(CActiveAEStream**)msg->data;
          SFlushStream(stream);
          msg->Reply(CActiveAEControlProtocol::ACC);
          m_extTimeout = 0ms;
          return;
        case CActiveAEControlProtocol::STREAMAMP:
          MsgStreamParameter *par;
          par = reinterpret_cast<MsgStreamParameter*>(msg->data);
          par->stream->m_limiter.SetAmplification(par->parameter.float_par);
          par->stream->m_amplify = par->parameter.float_par;
          return;
        case CActiveAEControlProtocol::STREAMVOLUME:
          par = reinterpret_cast<MsgStreamParameter*>(msg->data);
          par->stream->m_volume = par->parameter.float_par;
          return;
        case CActiveAEControlProtocol::STREAMRGAIN:
          par = reinterpret_cast<MsgStreamParameter*>(msg->data);
          par->stream->m_rgain = par->parameter.float_par;
          return;
        case CActiveAEControlProtocol::STREAMRESAMPLERATIO:
          par = reinterpret_cast<MsgStreamParameter*>(msg->data);
          if (par->stream->m_processingBuffers)
          {
            par->stream->m_processingBuffers->SetRR(par->parameter.double_par, m_settings.atempoThreshold);
          }
          return;
        case CActiveAEControlProtocol::STREAMFFMPEGINFO:
          MsgStreamFFmpegInfo *info;
          info = reinterpret_cast<MsgStreamFFmpegInfo*>(msg->data);
          info->stream->m_profile = info->profile;
          info->stream->m_matrixEncoding = info->matrix_encoding;
          info->stream->m_audioServiceType = info->audio_service_type;
          return;
        case CActiveAEControlProtocol::STREAMFADE:
          MsgStreamFade *fade;
          fade = reinterpret_cast<MsgStreamFade*>(msg->data);
          fade->stream->m_fadingBase = fade->from;
          fade->stream->m_fadingTarget = fade->target;
          fade->stream->m_fadingTime = fade->millis;
          fade->stream->m_fadingSamples = -1;
          return;
        case CActiveAEControlProtocol::STOPSOUND:
          CActiveAESound *sound;
          sound = *(CActiveAESound**)msg->data;
          SStopSound(sound);
          return;
        default:
          break;
        }
      }
      else if (port == &m_dataPort)
      {
        switch (signal)
        {
        case CActiveAEDataProtocol::PLAYSOUND:
          CActiveAESound *sound;
          sound = *(CActiveAESound**)msg->data;
          if (sound)
          {
            if (m_settings.guisoundmode == AE_SOUND_OFF ||
               (m_settings.guisoundmode == AE_SOUND_IDLE && !m_streams.empty()))
              return;

            SoundState st = {sound, 0};
            m_sounds_playing.push_back(st);
            m_extTimeout = 0ms;
            m_state = AE_TOP_CONFIGURED_PLAY;
          }
          return;
        case CActiveAEDataProtocol::NEWSTREAM:
          MsgStreamNew *streamMsg;
          CActiveAEStream *stream;
          streamMsg = reinterpret_cast<MsgStreamNew*>(msg->data);
          stream = CreateStream(streamMsg);
          if(stream)
          {
            msg->Reply(CActiveAEDataProtocol::ACC, &stream, sizeof(CActiveAEStream*));
            LoadSettings(&streamMsg->format);
            Configure();
            if (!m_extError)
            {
              m_state = AE_TOP_CONFIGURED_PLAY;
              m_extTimeout = 0ms;
            }
            else
            {
              m_state = AE_TOP_ERROR;
              m_extTimeout = 500ms;
            }
          }
          else
            msg->Reply(CActiveAEDataProtocol::ERR);
          return;
        case CActiveAEDataProtocol::STREAMSAMPLE:
          MsgStreamSample *msgData;
          CSampleBuffer *samples;
          msgData = reinterpret_cast<MsgStreamSample*>(msg->data);
          samples = msgData->stream->m_processingSamples.front();
          msgData->stream->m_processingSamples.pop_front();
          if (samples != msgData->buffer)
            CLog::Log(LOGERROR, "CActiveAE - inconsistency in stream sample message");
          if (msgData->buffer->pkt->nb_samples == 0)
            msgData->buffer->Return();
          else
            msgData->stream->m_processingBuffers->m_inputSamples.push_back(msgData->buffer);
          m_extTimeout = 0ms;
          m_state = AE_TOP_CONFIGURED_PLAY;
          return;
        case CActiveAEDataProtocol::FREESTREAM:
          MsgStreamFree *msgStreamFree;
          msgStreamFree = reinterpret_cast<MsgStreamFree*>(msg->data);
          DiscardStream(msgStreamFree->stream);
          msg->Reply(CActiveAEDataProtocol::ACC);
          if (m_streams.empty())
          {
            if (m_extKeepConfig > 0ms)
              m_extDrainTimer.Set(m_extKeepConfig);
            else
            {
              AEDelayStatus status;
              m_stats.GetDelay(status);
              if (msgStreamFree->finish)
                m_extDrainTimer.Set(
                    std::chrono::milliseconds(static_cast<int>(status.GetDelay() * 1000)));
              else
                m_extDrainTimer.Set(
                    std::chrono::milliseconds(static_cast<int>(status.GetDelay() * 1000)) + 1s);
            }
            m_extDrain = true;
          }
          m_extTimeout = 0ms;
          m_state = AE_TOP_CONFIGURED_PLAY;
          return;
        case CActiveAEDataProtocol::DRAINSTREAM:
          stream = *(CActiveAEStream**)msg->data;
          stream->m_drain = true;
          stream->m_processingBuffers->SetDrain(true);
          m_extTimeout = 0ms;
          m_state = AE_TOP_CONFIGURED_PLAY;
          msg->Reply(CActiveAEDataProtocol::ACC);
          return;
        default:
          break;
        }
      }
      else if (port == &m_sink.m_dataPort)
      {
        switch (signal)
        {
        case CSinkDataProtocol::RETURNSAMPLE:
          CSampleBuffer **buffer;
          buffer = (CSampleBuffer**)msg->data;
          if (buffer)
          {
            (*buffer)->Return();
          }
          m_extTimeout = 0ms;
          m_state = AE_TOP_CONFIGURED_PLAY;
          return;
        default:
          break;
        }
      }
      break;

    case AE_TOP_CONFIGURED_SUSPEND:
      if (port == &m_controlPort)
      {
        bool displayReset = false;
        switch (signal)
        {
        case CActiveAEControlProtocol::DISPLAYRESET:
          if (m_extSuspended)
            return;
          CLog::Log(LOGDEBUG,"CActiveAE - display reset event");
          displayReset = true;
          [[fallthrough]];
        case CActiveAEControlProtocol::INIT:
          m_extError = false;
          m_extSuspended = false;
          if (!displayReset)
          {
            m_controlPort.PurgeOut(CActiveAEControlProtocol::DEVICECHANGE);
            m_controlPort.PurgeOut(CActiveAEControlProtocol::DEFAULTDEVICECHANGE);
            m_controlPort.PurgeOut(CActiveAEControlProtocol::DEVICECOUNTCHANGE);
            m_sink.EnumerateSinkList(true, "");
            LoadSettings();
            ValidateOutputDevices(false);
          }
          Configure();
          if (!displayReset)
            msg->Reply(CActiveAEControlProtocol::ACC);
          if (!m_extError)
          {
            m_state = AE_TOP_CONFIGURED_PLAY;
            m_extTimeout = 0ms;
          }
          else
          {
            m_state = AE_TOP_ERROR;
            m_extTimeout = 500ms;
          }
          m_stats.SetSuspended(false);
          m_extDeferData = false;
          return;
        case CActiveAEControlProtocol::DEVICECHANGE:
        case CActiveAEControlProtocol::DEFAULTDEVICECHANGE:
        case CActiveAEControlProtocol::DEVICECOUNTCHANGE:
          return;
        default:
          break;
        }
      }
      else if (port == &m_sink.m_dataPort)
      {
        switch (signal)
        {
        case CSinkDataProtocol::RETURNSAMPLE:
          CSampleBuffer **buffer;
          buffer = (CSampleBuffer**)msg->data;
          if (buffer)
          {
            (*buffer)->Return();
          }
          return;
        default:
          break;
        }
      }
      else if (port == NULL) // timeout
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::TIMEOUT:
          m_extTimeout = 1000ms;
          return;
        default:
          break;
        }
      }
      break;

    case AE_TOP_CONFIGURED_IDLE:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::RESUMESTREAM:
          CActiveAEStream *stream;
          stream = *(CActiveAEStream**)msg->data;
          m_stats.TakeSyncPaused(stream);
          stream->m_paused = false;
          stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
          m_state = AE_TOP_CONFIGURED_PLAY;
          m_extTimeout = 0ms;
          return;
        case CActiveAEControlProtocol::FLUSHSTREAM:
          stream = *(CActiveAEStream**)msg->data;
          SFlushStream(stream);
          msg->Reply(CActiveAEControlProtocol::ACC);
          m_state = AE_TOP_CONFIGURED_PLAY;
          m_extTimeout = 0ms;
          return;
        default:
          break;
        }
      }
      else if (port == NULL) // timeout
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::TIMEOUT:
          ResampleSounds();
          ClearDiscardedBuffers();
          if (m_extDrain)
          {
            if (m_extDrainTimer.IsTimePast())
            {
              Configure();
              if (!m_extError)
              {
                m_state = AE_TOP_CONFIGURED_PLAY;
                m_extTimeout = 0ms;
              }
              else
              {
                m_state = AE_TOP_ERROR;
                m_extTimeout = 500ms;
              }
            }
            else
              m_extTimeout = m_extDrainTimer.GetTimeLeft();
          }
          else
            m_extTimeout = 5000ms;
          return;
        default:
          break;
        }
      }
      break;

    case AE_TOP_CONFIGURED_PLAY:
      if (port == NULL) // timeout
      {
        switch (signal)
        {
        case CActiveAEControlProtocol::TIMEOUT:
          if (m_extError)
          {
            EndStartTargets();
            m_state = AE_TOP_ERROR;
            m_extTimeout = 100ms;
            return;
          }
          if (RunStages())
          {
            m_extTimeout = 0ms;
            return;
          }
          if (!m_extDrain && HasWork())
          {
            ClearDiscardedBuffers();
            m_extTimeout = 100ms;
            return;
          }
          m_extTimeout = 0ms;
          m_state = AE_TOP_CONFIGURED_IDLE;
          return;
        default:
          break;
        }
      }
      break;

    default: // we are in no state, should not happen
      CLog::Log(LOGERROR, "CActiveAE::{} - no valid state: {}", __FUNCTION__, m_state);
      return;
    }
  } // for
}

void CActiveAE::Process()
{
  Message *msg = NULL;
  Protocol *port = NULL;
  bool gotMsg;
  XbmcThreads::EndTime<> timer;

  m_state = AE_TOP_WAIT_PRECOND;
  m_extTimeout = 1000ms;
  m_bStateMachineSelfTrigger = false;
  m_extDrain = false;
  m_extDeferData = false;
  m_extKeepConfig = 0ms;

  // start sink
  m_sink.Start();

  while (!m_bStop)
  {
    gotMsg = false;
    timer.Set(m_extTimeout);

    if (m_bStateMachineSelfTrigger)
    {
      m_bStateMachineSelfTrigger = false;
      // self trigger state machine
      StateMachine(msg->signal, port, msg);
      if (!m_bStateMachineSelfTrigger)
      {
        msg->Release();
        msg = NULL;
      }
      continue;
    }
    // check control port
    else if (m_controlPort.ReceiveOutMessage(&msg))
    {
      gotMsg = true;
      port = &m_controlPort;
    }
    // check sink data port
    else if (m_sink.m_dataPort.ReceiveInMessage(&msg))
    {
      gotMsg = true;
      port = &m_sink.m_dataPort;
    }
    else if (!m_extDeferData)
    {
      // check data port
      if (m_dataPort.ReceiveOutMessage(&msg))
      {
        gotMsg = true;
        port = &m_dataPort;
      }
      // stream data ports
      else
      {
        std::list<CActiveAEStream*>::iterator it;
        for(it=m_streams.begin(); it!=m_streams.end(); ++it)
        {
          if((*it)->m_streamPort->ReceiveOutMessage(&msg))
          {
            gotMsg = true;
            port = &m_dataPort;
            break;
          }
        }
      }
    }

    if (gotMsg)
    {
      StateMachine(msg->signal, port, msg);
      if (!m_bStateMachineSelfTrigger)
      {
        msg->Release();
        msg = NULL;
      }
      continue;
    }

    // wait for message
    else if (m_outMsgEvent.Wait(m_extTimeout))
    {
      m_extTimeout = timer.GetTimeLeft();
      continue;
    }
    // time out
    else
    {
      msg = m_controlPort.GetMessage();
      msg->signal = CActiveAEControlProtocol::TIMEOUT;
      port = 0;
      // signal timeout to state machine
      StateMachine(msg->signal, port, msg);
      if (!m_bStateMachineSelfTrigger)
      {
        msg->Release();
        msg = NULL;
      }
    }
  }
}

AEAudioFormat CActiveAE::GetInputFormat(AEAudioFormat *desiredFmt)
{
  AEAudioFormat inputFormat;

  if (m_streams.empty())
  {
    inputFormat.m_dataFormat    = AE_FMT_FLOAT;
    inputFormat.m_sampleRate    = 48000;
    inputFormat.m_channelLayout = AE_CH_LAYOUT_2_0;
    inputFormat.m_frames        = 0;
    inputFormat.m_frameSize     = 0;
  }
  // force input format after unpausing slave
  else if (desiredFmt != NULL)
  {
    inputFormat = *desiredFmt;
  }
  // keep format when having multiple streams
  else if (m_streams.size() > 1 && m_silenceBuffers == NULL)
  {
    inputFormat = m_inputFormat;
  }
  else
  {
    inputFormat = m_streams.front()->m_format;
    m_inputFormat = inputFormat;
  }

  return inputFormat;
}

void CActiveAE::Configure(AEAudioFormat *desiredFmt)
{
  bool initSink = false;

  AEAudioFormat sinkInputFormat, inputFormat;
  AEAudioFormat oldInternalFormat = m_internalFormat;
  AEAudioFormat oldSinkRequestFormat = m_sinkRequestFormat;

  inputFormat = GetInputFormat(desiredFmt);

  m_sinkRequestFormat = inputFormat;
  ApplySettingsToFormat(m_sinkRequestFormat, m_settings, (int*)&m_mode);
  m_extKeepConfig = 0ms;

  std::string device = (m_sinkRequestFormat.m_dataFormat == AE_FMT_RAW) ? m_settings.passthroughdevice : m_settings.device;

  const AESinkDevice dev = CAESinkFactory::ParseDevice(device);
  const bool requestedDefaultDevice = IsDefaultDevice(dev);

  if ((!CompareFormat(m_sinkRequestFormat, m_sinkFormat) &&
       !CompareFormat(m_sinkRequestFormat, oldSinkRequestFormat)) ||
      m_currDevice.compare(dev.name) != 0 || m_settings.driver.compare(dev.driver) != 0)
  {
    FlushEngine();
    if (!InitSink())
      return;
    m_settings.driver = dev.driver;
    m_currDevice = dev.name;
    m_currentDeviceFollowsDefault =
        requestedDefaultDevice || !IsSameDevice(m_openedDriver, m_openedDevice, dev);
    initSink = true;
    m_stats.Reset(m_sinkFormat.m_sampleRate, m_mode == MODE_PCM);
    m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::VOLUME, &m_volume, sizeof(float));

    if (m_sinkRequestFormat.m_dataFormat != AE_FMT_RAW)
    {
      // limit buffer size in case of sink returns large buffer
      double buffertime = (double)m_sinkFormat.m_frames / m_sinkFormat.m_sampleRate;
      if (buffertime > MAX_BUFFER_TIME)
      {
        CLog::Log(LOGWARNING,
                  "ActiveAE::{} - sink returned large period time of {} ms, reducing to {} ms",
                  __FUNCTION__, (int)(buffertime * 1000), (int)(MAX_BUFFER_TIME * 1000));
        m_sinkFormat.m_frames = MAX_BUFFER_TIME * m_sinkFormat.m_sampleRate;
      }
    }
  }

  if (m_silenceBuffers)
  {
    m_discardBufferPools.push_back(std::move(m_silenceBuffers));
    m_silenceBuffers = NULL;
  }

  // buffers for driving gui sounds if no streams are active
  if (m_streams.empty())
  {
    inputFormat = m_sinkFormat;
    if (m_sinkFormat.m_channelLayout.Count() > m_sinkRequestFormat.m_channelLayout.Count())
    {
      inputFormat.m_channelLayout = m_sinkRequestFormat.m_channelLayout;
      inputFormat.m_channelLayout.ResolveChannels(m_sinkFormat.m_channelLayout);
    }
    inputFormat.m_dataFormat = AE_FMT_FLOAT;
    inputFormat.m_frameSize = inputFormat.m_channelLayout.Count() *
                              (CAEUtil::DataFormatToBits(inputFormat.m_dataFormat) >> 3);
    m_silenceBuffers = std::make_unique<CActiveAEBufferPool>(inputFormat);
    m_silenceBuffers->Create(MAX_WATER_LEVEL*1000);
    sinkInputFormat = inputFormat;
    m_internalFormat = inputFormat;

    bool streaming = false;
    m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::STREAMING, &streaming, sizeof(bool));

    delete m_encoder;
    m_encoder = NULL;

    if (m_encoderBuffers)
    {
      m_discardBufferPools.push_back(std::move(m_encoderBuffers));
    }
    if (m_vizBuffers)
    {
      m_discardBufferPools.push_back(std::move(m_vizBuffers));
    }
    if (m_vizBuffersInput)
    {
      m_discardBufferPools.push_back(std::move(m_vizBuffersInput));
    }
  }
  // resample buffers for streams
  else
  {
    bool streaming = true;
    m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::STREAMING, &streaming, sizeof(bool));

    AEAudioFormat outputFormat;
    if (m_mode == MODE_RAW)
    {
      inputFormat.m_frames = m_sinkFormat.m_frames;
      outputFormat = inputFormat;
      sinkInputFormat = m_sinkFormat;
    }
    // transcode everything with more than 2 channels
    else if (m_mode == MODE_TRANSCODE)
    {
      outputFormat = inputFormat;
      outputFormat.m_dataFormat = AE_FMT_FLOATP;
      outputFormat.m_sampleRate = 48000;
      outputFormat.m_streamInfo.m_type = m_sinkRequestFormat.m_streamInfo.m_type;

      // setup encoder
      if (!m_encoder)
      {
        m_encoder = new CAEEncoderFFmpeg();
        m_encoder->Initialize(outputFormat, true);
        m_encoderFormat = outputFormat;
      }
      else
        outputFormat = m_encoderFormat;

      outputFormat.m_channelLayout = m_encoderFormat.m_channelLayout;
      outputFormat.m_frames = m_encoderFormat.m_frames;

      // encoder buffer
      if (m_encoder->GetCodecID() == AV_CODEC_ID_AC3 || m_encoder->GetCodecID() == AV_CODEC_ID_EAC3)
      {
        AEAudioFormat format;
        format.m_channelLayout += AE_CH_FC;
        format.m_dataFormat = AE_FMT_RAW;
        format.m_sampleRate = 48000;
        format.m_channelLayout = AE_CH_LAYOUT_2_0;
        format.m_streamInfo.m_type = (m_encoder->GetCodecID() == AV_CODEC_ID_EAC3)
                                         ? CAEStreamInfo::STREAM_TYPE_EAC3
                                         : CAEStreamInfo::STREAM_TYPE_AC3;
        format.m_streamInfo.m_channels = 2;
        format.m_streamInfo.m_sampleRate = 48000;
        format.m_streamInfo.m_frameSize = m_encoderFormat.m_frames;
        //! @todo implement
        if (m_encoderBuffers && initSink)
        {
          m_discardBufferPools.push_back(std::move(m_encoderBuffers));
        }
        if (!m_encoderBuffers)
        {
          m_encoderBuffers = std::make_unique<CActiveAEBufferPool>(format);
          m_encoderBuffers->Create(MAX_WATER_LEVEL*1000);
        }
      }

      sinkInputFormat = m_sinkFormat;
    }
    else
    {
      outputFormat = m_sinkFormat;
      outputFormat.m_dataFormat = AE_IS_PLANAR(outputFormat.m_dataFormat) ? AE_FMT_FLOATP : AE_FMT_FLOAT;
      outputFormat.m_frameSize = outputFormat.m_channelLayout.Count() *
                                 (CAEUtil::DataFormatToBits(outputFormat.m_dataFormat) >> 3);

      // due to channel ordering of the driver, a sink may return more channels than
      // requested, i.e. 2.1 request returns FL,FR,BL,BR,FC,LFE for ALSA
      // in this case we need to downmix to requested format
      if (m_sinkFormat.m_channelLayout.Count() > m_sinkRequestFormat.m_channelLayout.Count())
      {
        outputFormat.m_channelLayout = m_sinkRequestFormat.m_channelLayout;
        outputFormat.m_channelLayout.ResolveChannels(m_sinkFormat.m_channelLayout);
      }

      // internally we use ffmpeg layouts, means that layout won't change in resample
      // stage. preserve correct layout for sink stage where remapping is done
      uint64_t avlayout = CAEUtil::GetAVChannelLayout(outputFormat.m_channelLayout);
      outputFormat.m_channelLayout = CAEUtil::GetAEChannelLayout(avlayout);

      //! @todo adjust to decoder
      sinkInputFormat = outputFormat;
    }
    m_internalFormat = outputFormat;

    std::list<CActiveAEStream*>::iterator it;
    for(it=m_streams.begin(); it!=m_streams.end(); ++it)
    {
      if (!(*it)->m_inputBuffers)
      {
        // align input buffers with period of sink or encoder
        (*it)->m_format.m_frames = m_internalFormat.m_frames * ((float)(*it)->m_format.m_sampleRate / m_internalFormat.m_sampleRate);

        // create buffer pool
        (*it)->m_inputBuffers = std::make_unique<CActiveAEBufferPool>((*it)->m_format);
        (*it)->m_inputBuffers->Create(MAX_CACHE_LEVEL*1000);
        (*it)->m_streamSpace = (*it)->m_format.m_frameSize * (*it)->m_format.m_frames;

        // if input format does not follow ffmpeg channel mask, we may need to remap channels
        (*it)->InitRemapper();
      }
      double resampleRatio = 1.0;
      if (initSink && (*it)->m_processingBuffers)
      {
        resampleRatio = (*it)->m_processingBuffers->GetRR();
        (*it)->m_processingBuffers->Flush();
        m_discardBufferPools.push_back((*it)->m_processingBuffers->GetResampleBuffers());
        m_discardBufferPools.push_back((*it)->m_processingBuffers->GetAtempoBuffers());
        (*it)->m_processingBuffers.reset();
        // a start cut was measured against the flushed buffers
        if ((*it)->m_pClock && !(*it)->m_freed)
          (*it)->m_pClock->TakeStartCut();
        (*it)->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
      }
      if (!(*it)->m_processingBuffers)
      {
        (*it)->m_processingBuffers = std::make_unique<CActiveAEStreamBuffers>(
            (*it)->m_inputBuffers->m_format, outputFormat, m_settings.resampleQuality);
        (*it)->m_processingBuffers->ForceResampler((*it)->m_forceResampler);

        (*it)->m_processingBuffers->Create(MAX_CACHE_LEVEL * 1000, false, m_settings.stereoupmix,
                                           m_settings.normalizelevels, m_settings.mixSubLevel);

        // A stream only sends its resample ratio when it changes, so keep it
        // across a sink change
        (*it)->m_processingBuffers->SetRR(resampleRatio, m_settings.atempoThreshold);
      }
      if (m_mode == MODE_TRANSCODE || m_streams.size() > 1)
        (*it)->m_processingBuffers->FillBuffer();

      // amplification
      (*it)->m_limiter.SetSamplerate(outputFormat.m_sampleRate);
    }

    // update buffered time of streams
    m_stats.AddSamples(0, m_streams);

    // buffers for viz
    if (!(inputFormat.m_dataFormat == AE_FMT_RAW))
    {
      if (initSink && m_vizBuffers)
      {
        m_discardBufferPools.push_back(std::move(m_vizBuffers));
        m_discardBufferPools.push_back(std::move(m_vizBuffersInput));
      }
      if (!m_vizBuffers && !m_audioCallback.empty())
      {
        AEAudioFormat vizFormat = m_internalFormat;
        vizFormat.m_channelLayout = AE_CH_LAYOUT_2_0;
        vizFormat.m_dataFormat = AE_FMT_FLOAT;
        vizFormat.m_sampleRate = 44100;
        vizFormat.m_frames =
            m_internalFormat.m_frames *
            (static_cast<float>(vizFormat.m_sampleRate) / m_internalFormat.m_sampleRate);

        // input buffers
        m_vizBuffersInput = std::make_unique<CActiveAEBufferPool>(m_internalFormat);
        m_vizBuffersInput->Create(2000 + m_stats.GetMaxDelay() * 1000);

        // resample buffers
        m_vizBuffers = std::make_unique<CActiveAEBufferPoolResample>(m_internalFormat, vizFormat,
                                                                     m_settings.resampleQuality);
        //! @todo use cache of sync + water level
        m_vizBuffers->Create(2000 + m_stats.GetMaxDelay() * 1000, false, false);
        m_vizInitialized = false;
      }
    }

    // buffers need to sync
    m_silenceBuffers = std::make_unique<CActiveAEBufferPool>(outputFormat);
    m_silenceBuffers->Create(500);
  }

  // resample buffers for sink
  if (m_sinkBuffers &&
     (!CompareFormat(m_sinkBuffers->m_format,m_sinkFormat) ||
      !CompareFormat(m_sinkBuffers->m_inputFormat, sinkInputFormat) ||
      m_sinkBuffers->m_format.m_frames != m_sinkFormat.m_frames))
  {
    m_discardBufferPools.push_back(std::move(m_sinkBuffers));
  }
  if (!m_sinkBuffers)
  {
    m_sinkBuffers = std::make_unique<CActiveAEBufferPoolResample>(sinkInputFormat, m_sinkFormat,
                                                                  m_settings.resampleQuality);
    m_sinkBuffers->Create(MAX_WATER_LEVEL*1000, true, false);
  }

  ConfigureLowLatency();

  // reset gui sounds
  if (!CompareFormat(oldInternalFormat, m_internalFormat))
  {
    CServiceBroker::GetDataCacheCore().ResetAudioCache();
    if (m_settings.guisoundmode == AE_SOUND_ALWAYS ||
       (m_settings.guisoundmode == AE_SOUND_IDLE && m_streams.empty()) ||
       m_aeGUISoundForce)
    {
      std::vector<CActiveAESound*>::iterator it;
      for (it = m_sounds.begin(); it != m_sounds.end(); ++it)
      {
        (*it)->SetConverted(false);
      }
    }
    m_sounds_playing.clear();
  }

  ClearDiscardedBuffers();
  m_extDrain = false;
}

CActiveAEStream* CActiveAE::CreateStream(MsgStreamNew *streamMsg)
{
  // we only can handle a single pass through stream
  bool hasRawStream = false;
  bool hasStream = false;
  std::list<CActiveAEStream*>::iterator it;
  for(it = m_streams.begin(); it != m_streams.end(); ++it)
  {
    if ((*it)->IsDrained())
      continue;
    if ((*it)->m_format.m_dataFormat == AE_FMT_RAW)
      hasRawStream = true;
    hasStream = true;
  }
  if (hasRawStream || (hasStream && (streamMsg->format.m_dataFormat == AE_FMT_RAW)))
  {
    return NULL;
  }

  // create the stream
  CActiveAEStream *stream;
  stream = new CActiveAEStream(&streamMsg->format, m_streamIdGen++, this);
  stream->m_streamPort =
      std::make_unique<CActiveAEDataProtocol>("stream", &stream->m_inMsgEvent, &m_outMsgEvent);

  // create buffer pool
  stream->m_inputBuffers = NULL; // create in Configure when we know the sink format
  stream->m_fadingSamples = 0;
  stream->m_started = false;
  stream->m_resampleMode = 0;
  stream->m_syncState = CAESyncInfo::AESyncState::SYNC_OFF;

  if (streamMsg->options & AESTREAM_PAUSED)
  {
    stream->m_paused = true;
    stream->m_streamIsBuffering = true;
  }

  if (streamMsg->options & AESTREAM_FORCE_RESAMPLE)
    stream->m_forceResampler = true;

  stream->m_pClock = streamMsg->clock;

  m_streams.push_back(stream);
  m_stats.AddStream(stream->m_id);

  return stream;
}

void CActiveAE::DiscardStream(CActiveAEStream *stream)
{
  std::list<CActiveAEStream*>::iterator it;
  for (it=m_streams.begin(); it!=m_streams.end(); )
  {
    if (stream == (*it))
    {
      while (!(*it)->m_processingSamples.empty())
      {
        (*it)->m_processingSamples.front()->Return();
        (*it)->m_processingSamples.pop_front();
      }
      if ((*it)->m_inputBuffers)
        m_discardBufferPools.push_back(std::move((*it)->m_inputBuffers));
      if ((*it)->m_processingBuffers)
      {
        (*it)->m_processingBuffers->Flush();
        m_discardBufferPools.push_back((*it)->m_processingBuffers->GetResampleBuffers());
        m_discardBufferPools.push_back((*it)->m_processingBuffers->GetAtempoBuffers());
      }
      CLog::Log(LOGDEBUG, "CActiveAE::DiscardStream - audio stream deleted");
      m_stats.RemoveStream((*it)->m_id);
      delete (*it);
      it = m_streams.erase(it);
    }
    else
      ++it;
  }

  ClearDiscardedBuffers();
}

void CActiveAE::SFlushStream(CActiveAEStream *stream)
{
  while (!stream->m_processingSamples.empty())
  {
    stream->m_processingSamples.front()->Return();
    stream->m_processingSamples.pop_front();
  }
  stream->m_processingBuffers->Flush();
  stream->m_streamPort->Purge();
  stream->m_bufferedTime = 0.0;
  m_stats.TakeSyncPaused(stream);
  stream->m_paused = false;
  stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
  stream->m_syncError.Flush();
  stream->ResetFreeBuffers();

  // Reset Logic State Variables to revive Servo
  stream->m_lastPts = 0.0;
  stream->m_lastPtsJump = 0.0;
  stream->m_errorInterval = std::chrono::milliseconds(1000);

  // Synchronously reset servo state to prevent stale cache interaction.
  // This MUST be done before m_stats.UpdateStream(stream) is called.
  stream->m_resampleIntegral = 0.0;
  if (stream->m_processingBuffers)
  {
    stream->m_processingBuffers->SetRR(1.0, m_settings.atempoThreshold);
  }

  // flush the engine if we only have a single stream
  if (m_streams.size() == 1)
  {
    FlushEngine(true);
  }

  m_stats.UpdateStream(stream);
}

void CActiveAE::FlushEngine(bool discardOutput)
{
  if (m_sinkBuffers)
    m_sinkBuffers->Flush();
  if (m_vizBuffers)
    m_vizBuffers->Flush();

  // The AC3/EAC3 encoder keeps internal state (MDCT overlap, partially filled input frame).
  // If it survives a flush, the first encoded frame after seek/pause mixes stale data from the
  // old position with the new audio, which results in an audible pop. Start from a clean encoder.
  if (m_mode == MODE_TRANSCODE && m_encoder)
  {
    AEAudioFormat encFormat = m_encoderFormat;
    delete m_encoder;
    m_encoder = new CAEEncoderFFmpeg();
    if (m_encoder->Initialize(encFormat, true))
    {
      m_encoderFormat = encFormat;
    }
    else
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - failed to re-initialize encoder on flush", __FUNCTION__);
      delete m_encoder;
      m_encoder = nullptr;
      m_extError = true;
    }
  }

  // send message to sink
  Message *reply;
  if (m_sink.m_controlPort.SendOutMessageSync(CSinkControlProtocol::FLUSH, &reply, 2s,
                                              &discardOutput, sizeof(discardOutput)))
  {
    bool success = reply->signal == CSinkControlProtocol::ACC;
    if (!success)
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error on flush", __FUNCTION__);
      m_extError = true;
    }
    reply->Release();
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to flush", __FUNCTION__);
    m_extError = true;
  }
  m_stats.Reset(m_sinkFormat.m_sampleRate, m_mode == MODE_PCM);
}

void CActiveAE::ClearDiscardedBuffers()
{
  auto it = m_discardBufferPools.begin();
  while (it != m_discardBufferPools.end())
  {
    CActiveAEBufferPoolResample* rbuf = dynamic_cast<CActiveAEBufferPoolResample*>((*it).get());
    if (rbuf)
    {
      rbuf->Flush();
    }
    // if all buffers have returned, we can delete the buffer pool
    if ((*it)->m_allSamples.size() == (*it)->m_freeSamples.size())
    {
      CLog::Log(LOGDEBUG, "CActiveAE::ClearDiscardedBuffers - buffer pool deleted");
      it = m_discardBufferPools.erase(it);
    }
    else
      ++it;
  }
}

void CActiveAE::SStopSound(CActiveAESound *sound)
{
  // A sound played again before it finished is in the list once for each play.
  const auto removed = std::erase_if(m_sounds_playing, [sound](const SoundState& state)
                                     { return state.sound == sound; });
  if (removed > 0 && sound->GetChannel() != AE_CH_NULL)
    m_aeGUISoundForce = false;
}

void CActiveAE::DiscardSound(CActiveAESound *sound)
{
  SStopSound(sound);

  std::vector<CActiveAESound*>::iterator it;
  for (it=m_sounds.begin(); it!=m_sounds.end(); ++it)
  {
    if ((*it) == sound)
    {
      m_sounds.erase(it);
      delete sound;
      return;
    }
  }
}

void CActiveAE::ChangeResamplers()
{
  std::list<CActiveAEStream*>::iterator it;
  for(it=m_streams.begin(); it!=m_streams.end(); ++it)
  {
    (*it)->m_processingBuffers->ConfigureResampler(
        m_settings.normalizelevels, m_settings.stereoupmix, m_settings.resampleQuality,
        m_settings.mixSubLevel);
  }
}

// Configure buffer level ramp-up when low latency mode is enabled
// Resample OFF --> from ~20ms to ~200ms
// Resample ON --> from ~100ms to ~200ms
// Low latency OFF --> constant to ~200ms (no ramp-up)
// The increment is added to avoid ambiguous float comparisons (0.0199999 instead of 0.020001)
void CActiveAE::ConfigureLowLatency()
{
  bool resample{false};

  if (m_settings.lowLatencyMode)
  {
    if (!m_streams.empty())
    {
      for (const auto& stream : m_streams)
      {
        if (stream->m_streamResampleMode != 0 || stream->m_resampleMode != 0)
        {
          resample = true;
          break;
        }
      }
    }
    if (resample)
      m_initialTargetBufferLevel = MIN_WATER_LEVEL_RESAMPLE + BUFFER_LEVEL_INCREMENT;
    else
      m_initialTargetBufferLevel = MIN_WATER_LEVEL + BUFFER_LEVEL_INCREMENT;
  }
  else
  {
    m_initialTargetBufferLevel = MAX_WATER_LEVEL + BUFFER_LEVEL_INCREMENT;
  }

  m_targetBufferLevel = m_initialTargetBufferLevel;

  CLog::LogF(LOGDEBUG, "Low latency mode: {} - Initial buffer level: {:.0f}ms (resample: {})",
             m_settings.lowLatencyMode, m_initialTargetBufferLevel * 1000.0f, resample);
}

void CActiveAE::ApplySettingsToFormat(AEAudioFormat& format,
                                      const AudioSettings& settings,
                                      int* mode)
{
  int oldMode = m_mode;
  if (mode)
    *mode = MODE_PCM;

  // raw pass through
  if (format.m_dataFormat == AE_FMT_RAW)
  {
    if (mode)
      *mode = MODE_RAW;
  }
  // transcode
  else if (settings.channels <= AE_CH_LAYOUT_2_0 && // no multichannel pcm
           settings.passthrough &&
           settings.ac3passthrough &&
           settings.ac3transcode &&
           !m_streams.empty() &&
           (format.m_channelLayout.Count() > 2 || settings.stereoupmix))
  {
    format.m_dataFormat = AE_FMT_RAW;
    format.m_sampleRate = 48000;
    format.m_channelLayout = AE_CH_LAYOUT_2_0;
    format.m_streamInfo.m_channels = 2;
    format.m_streamInfo.m_sampleRate = 48000;
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    if (!settings.eac3passthrough || !m_sink.SupportsFormat(settings.passthroughdevice, format))
      format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    if (mode)
      *mode = MODE_TRANSCODE;
  }
  else
  {
    format.m_dataFormat = AE_IS_PLANAR(format.m_dataFormat) ? AE_FMT_FLOATP : AE_FMT_FLOAT;
    // consider user channel layout for those cases
    // 1. input stream is multichannel
    // 2. stereo upmix is selected
    // 3. fixed mode
    if ((format.m_channelLayout.Count() > 2) ||
         settings.stereoupmix ||
         (settings.config == AE_CONFIG_FIXED))
    {
      CAEChannelInfo stdLayout;
      switch (settings.channels)
      {
        default:
        case  0: stdLayout = AE_CH_LAYOUT_2_0; break;
        case  1: stdLayout = AE_CH_LAYOUT_2_0; break;
        case  2: stdLayout = AE_CH_LAYOUT_2_1; break;
        case  3: stdLayout = AE_CH_LAYOUT_3_0; break;
        case  4: stdLayout = AE_CH_LAYOUT_3_1; break;
        case  5: stdLayout = AE_CH_LAYOUT_4_0; break;
        case  6: stdLayout = AE_CH_LAYOUT_4_1; break;
        case  7: stdLayout = AE_CH_LAYOUT_5_0; break;
        case  8: stdLayout = AE_CH_LAYOUT_5_1; break;
        case  9: stdLayout = AE_CH_LAYOUT_7_0; break;
        case 10: stdLayout = AE_CH_LAYOUT_7_1; break;
      }

      if (m_settings.config == AE_CONFIG_FIXED || (settings.stereoupmix && format.m_channelLayout.Count() <= 2))
        format.m_channelLayout = stdLayout;
      else if ((m_extKeepConfig > 0ms) && (settings.config == AE_CONFIG_AUTO) &&
               (oldMode != MODE_RAW))
        format.m_channelLayout = m_internalFormat.m_channelLayout;
      else
      {
        if (stdLayout == AE_CH_LAYOUT_5_0 || stdLayout == AE_CH_LAYOUT_5_1)
        {
          std::vector<CAEChannelInfo> alts;
          alts.push_back(stdLayout);
          stdLayout.ReplaceChannel(AE_CH_BL, AE_CH_SL);
          stdLayout.ReplaceChannel(AE_CH_BR, AE_CH_SR);
          alts.push_back(stdLayout);
          int bestMatch = format.m_channelLayout.BestMatch(alts);
          stdLayout = alts[bestMatch];
        }
        format.m_channelLayout.ResolveChannels(stdLayout);
      }
    }
    // don't change from multi to stereo in AUTO mode
    else if ((settings.config == AE_CONFIG_AUTO) &&
              m_stats.GetWaterLevel() > 0 && m_internalFormat.m_channelLayout.Count() > 2)
    {
      format.m_channelLayout = m_internalFormat.m_channelLayout;
    }

    if (m_sink.GetDeviceType(m_settings.device) == AE_DEVTYPE_IEC958)
    {
      if (format.m_sampleRate > m_settings.samplerate)
      {
        format.m_sampleRate = m_settings.samplerate;
        CLog::Log(LOGINFO, "CActiveAE::ApplySettings - limit samplerate for SPDIF to {}",
                  format.m_sampleRate);
      }
      format.m_channelLayout = AE_CH_LAYOUT_2_0;
    }

    if (m_settings.config == AE_CONFIG_FIXED)
    {
      format.m_sampleRate = m_settings.samplerate;
      format.m_dataFormat = AE_FMT_FLOAT;
      CLog::Log(LOGINFO, "CActiveAE::ApplySettings - Forcing samplerate to {}",
                format.m_sampleRate);
    }

    // sinks may not support mono
    if (format.m_channelLayout.Count() == 1)
    {
      format.m_channelLayout = AE_CH_LAYOUT_2_0;
    }
  }
}

bool CActiveAE::NeedReconfigureBuffers()
{
  AEAudioFormat newFormat = GetInputFormat();
  ApplySettingsToFormat(newFormat, m_settings);

  return newFormat.m_dataFormat != m_sinkRequestFormat.m_dataFormat ||
      newFormat.m_channelLayout != m_sinkRequestFormat.m_channelLayout ||
      newFormat.m_sampleRate != m_sinkRequestFormat.m_sampleRate;
}

bool CActiveAE::NeedReconfigureSink()
{
  AEAudioFormat newFormat = GetInputFormat();
  ApplySettingsToFormat(newFormat, m_settings);

  std::string device = (newFormat.m_dataFormat == AE_FMT_RAW) ? m_settings.passthroughdevice : m_settings.device;

  const AESinkDevice dev = CAESinkFactory::ParseDevice(device);

  return !CompareFormat(newFormat, m_sinkFormat) || m_currDevice.compare(dev.name) != 0 ||
         m_settings.driver.compare(dev.driver) != 0;
}

bool CActiveAE::InitSink()
{
  SinkConfig config;
  config.format = m_sinkRequestFormat;
  config.stats = &m_stats;
  config.device = (m_sinkRequestFormat.m_dataFormat == AE_FMT_RAW) ? &m_settings.passthroughdevice :
                                                                     &m_settings.device;
  config.streaming = !m_streams.empty();
  config.fill = config.streaming;
  for (const auto* stream : m_streams)
  {
    if (!stream->m_paused && stream->m_processingBuffers && stream->m_processingBuffers->HasWork())
      config.fill = false;
  }
  m_stats.SetOutputStamped(false);
  EndStartTargets();

  // send message to sink
  m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::SETNOISETYPE, &m_settings.streamNoise, sizeof(bool));
  m_sink.m_controlPort.SendOutMessage(CSinkControlProtocol::SETSILENCETIMEOUT,
                                      &m_settings.silenceTimeoutMinutes, sizeof(int));

  Message *reply;
  if (m_sink.m_controlPort.SendOutMessageSync(CSinkControlProtocol::CONFIGURE, &reply, 5s, &config,
                                              sizeof(config)))
  {
    bool success = reply->signal == CSinkControlProtocol::ACC;
    if (!success)
    {
      reply->Release();
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error", __FUNCTION__);
      m_extError = true;
      return false;
    }
    SinkReply *data;
    data = reinterpret_cast<SinkReply*>(reply->data);
    if (data)
    {
      m_sinkFormat = data->format;
      if (data->device)
      {
        const AESinkDevice openedDevice = CAESinkFactory::ParseDevice(*data->device);
        m_openedDriver = openedDevice.driver;
        m_openedDevice = openedDevice.name;
      }
      m_sinkHasVolume = data->hasVolume;
      m_stats.SetSinkCacheTotal(data->cacheTotal);
      m_stats.SetSinkLatency(data->latency);
      m_stats.SetCurrentSinkFormat(m_sinkFormat);
      m_stats.SetSinkNeedIec(m_sink.NeedIecPack());
    }
    reply->Release();
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to init", __FUNCTION__);
    m_stats.SetSinkCacheTotal(0);
    m_stats.SetSinkLatency(0);
    AEAudioFormat invalidFormat;
    invalidFormat.m_dataFormat = AE_FMT_INVALID;
    m_stats.SetCurrentSinkFormat(invalidFormat);
    m_extError = true;
    return false;
  }

  m_inMsgEvent.Reset();
  return true;
}

void CActiveAE::DrainSink()
{
  // send message to sink
  Message *reply;
  if (m_sink.m_dataPort.SendOutMessageSync(CSinkDataProtocol::DRAIN, &reply, 2s))
  {
    bool success = reply->signal == CSinkDataProtocol::ACC;
    if (!success)
    {
      reply->Release();
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error on drain", __FUNCTION__);
      m_extError = true;
      return;
    }
    reply->Release();
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to drain", __FUNCTION__);
    m_extError = true;
    return;
  }
}

void CActiveAE::EndStartTargets()
{
  for (CActiveAEStream* stream : m_streams)
  {
    if (stream->m_pClock && !stream->m_freed)
      stream->m_pClock->EndStartTarget();
  }
}

void CActiveAE::UnconfigureSink()
{
  // send message to sink
  Message *reply;
  if (m_sink.m_controlPort.SendOutMessageSync(CSinkControlProtocol::UNCONFIGURE, &reply, 2s))
  {
    bool success = reply->signal == CSinkControlProtocol::ACC;
    if (!success)
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error", __FUNCTION__);
      m_extError = true;
    }
    reply->Release();
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to unconfigure", __FUNCTION__);
    m_extError = true;
  }

  // make sure we open sink on next configure
  m_currDevice = "";
  m_openedDevice = "";
  m_openedDriver = "";
  m_currentDeviceFollowsDefault = false;
  EndStartTargets();

  m_inMsgEvent.Reset();
}


bool CActiveAE::RunStages()
{
  bool busy = false;

  // serve input streams
  std::list<CActiveAEStream*>::iterator it;
  for (it = m_streams.begin(); it != m_streams.end(); ++it)
  {
    if ((*it)->m_processingBuffers && !(*it)->m_paused)
      busy = (*it)->m_processingBuffers->ProcessBuffers();

    if ((*it)->m_streamIsBuffering &&
        (*it)->m_processingBuffers &&
        ((*it)->m_processingBuffers->HasInputLevel(50)))
    {
      std::unique_lock lock((*it)->m_streamLock);
      (*it)->m_streamIsBuffering = false;
    }

    // provide buffers to stream
    float time = m_stats.GetCacheTime((*it));
    CSampleBuffer *buffer;
    if (!(*it)->m_drain)
    {
      float buftime = (float)(*it)->m_inputBuffers->m_format.m_frames / (*it)->m_inputBuffers->m_format.m_sampleRate;
      if ((*it)->m_inputBuffers->m_format.m_dataFormat == AE_FMT_RAW)
        buftime = (*it)->m_inputBuffers->m_format.m_streamInfo.GetDuration() / 1000;
      while ((time < MAX_CACHE_LEVEL || (*it)->m_streamIsBuffering) &&
             !(*it)->m_inputBuffers->m_freeSamples.empty())
      {
        buffer = (*it)->m_inputBuffers->GetFreeBuffer();
        (*it)->m_processingSamples.push_back(buffer);
        (*it)->m_streamPort->SendInMessage(CActiveAEDataProtocol::STREAMBUFFER, &buffer, sizeof(CSampleBuffer*));
        (*it)->IncFreeBuffers();
        time += buftime;
      }
    }
    else
    {
      if ((*it)->m_processingBuffers->IsDrained() &&
          (*it)->m_processingSamples.empty())
      {
        (*it)->m_streamPort->SendInMessage(CActiveAEDataProtocol::STREAMDRAINED);
        (*it)->m_drain = false;
        (*it)->m_processingBuffers->SetDrain(false);
        (*it)->m_started = false;

        // set variables being polled via stream interface
        std::unique_lock lock((*it)->m_streamLock);
        if ((*it)->m_streamSlave)
        {
          CActiveAEStream *slave = (CActiveAEStream*)((*it)->m_streamSlave);
          slave->m_paused = false;

          //! @todo find better solution for this gapless bites audiophile
          if (m_settings.config == AE_CONFIG_MATCH)
            Configure(&slave->m_format);

          (*it)->m_streamSlave = NULL;
        }
        (*it)->m_streamDrained = true;
        (*it)->m_streamDraining = false;
        (*it)->m_streamFading = false;
      }
    }
  }

  if (m_settings.lowLatencyMode)
  {
    // m_targetBufferLevel grows progressively from ~20ms (virtual zero buffer and zero latency)
    // to ~200 ms (nominal buffer and nominal latency), same as before.
    // In resample mode is from ~100ms to ~200ms to prevent buffer underrun.
    if (m_targetBufferLevel < MAX_WATER_LEVEL)
      m_targetBufferLevel += BUFFER_LEVEL_INCREMENT; // 2000 iterations -> ramp-up of ~10 seconds
  }

  // The buffer level "GetWaterLevel()" always tries to follow m_targetBufferLevel because when it
  // is lower, audio samples are added, and when it is higher, audio samples stop being added
  // and the level goes down.
  if (m_stats.GetWaterLevel() < m_targetBufferLevel &&
      (m_mode != MODE_TRANSCODE || (m_encoderBuffers && !m_encoderBuffers->m_freeSamples.empty())))
  {
    // calculate sync error
    for (it = m_streams.begin(); it != m_streams.end(); ++it)
    {
      // reset target buffer level at pause (but not initial start pause)
      if ((*it)->m_paused && (*it)->m_started && m_settings.lowLatencyMode)
        m_targetBufferLevel = m_initialTargetBufferLevel;

      if ((*it)->m_paused || !(*it)->m_started || !(*it)->m_processingBuffers || !(*it)->m_pClock ||
          (*it)->m_freed)
        continue;

      // a stream with nothing queued has no start to decide
      double first{0.0};
      double last{0.0};
      int epoch{0};
      if ((*it)->m_processingBuffers->IsDrained() &&
          (*it)->m_pClock->GetStartTarget(first, last, epoch))
        (*it)->m_pClock->EndStartTarget();

      if ((*it)->m_processingBuffers->m_outputSamples.empty())
        continue;

      // the silence an idle sink writes ahead of a start counts from its next buffer report
      if ((*it)->m_syncState != CAESyncInfo::SYNC_INSYNC &&
          m_stats.GetSinkReports() == (*it)->m_syncSinkReports)
      {
        (*it)->m_syncError.Flush(100ms);
        continue;
      }

      CSampleBuffer *buf = (*it)->m_processingBuffers->m_outputSamples.front();
      if (buf->timestamp)
      {
        AEDelayStatus status;
        m_stats.GetDelay(status);
        double pts =
            buf->timestamp - (buf->pkt_start_offset * 1000.0 / buf->pkt->config.sample_rate);
        double delay = status.GetDelay() * 1000;
        double playingPts = pts - delay;
        double maxError = ((*it)->m_syncState == CAESyncInfo::SYNC_INSYNC) ? 1000 : 5000;
        double error = playingPts - (*it)->m_pClock->GetClock();

        if (error > maxError)
        {
          CLog::Log(LOGWARNING, "ActiveAE - large audio sync error: {:f}", error);
          error = maxError;
        }
        else if (error < -maxError)
        {
          CLog::Log(LOGWARNING, "ActiveAE - large audio sync error: {:f}", error);
          error = -maxError;
        }
        (*it)->m_syncError.Add(error);
      }
    }

    // mix streams and sounds sounds
    if (m_mode != MODE_RAW)
    {
      CSampleBuffer *out = NULL;
      if (!m_sounds_playing.empty() && m_streams.empty())
      {
        if (m_silenceBuffers && !m_silenceBuffers->m_freeSamples.empty())
        {
          out = m_silenceBuffers->GetFreeBuffer();
          for (int i=0; i<out->pkt->planes; i++)
          {
            memset(out->pkt->data[i], 0, out->pkt->linesize);
          }
          out->pkt->nb_samples = out->pkt->max_nb_samples;
        }
      }

      // mix streams
      std::list<CActiveAEStream*>::iterator it;

      // if we deal with more than a single stream, all streams
      // must provide samples for mixing
      bool allStreamsReady = true;
      for (it = m_streams.begin(); it != m_streams.end(); ++it)
      {
        if ((*it)->m_paused || !(*it)->m_started || !(*it)->m_processingBuffers)
          continue;

        if ((*it)->m_processingBuffers->m_outputSamples.empty())
          allStreamsReady = false;
      }

      bool needClamp = false;
      for (it = m_streams.begin(); it != m_streams.end() && allStreamsReady; ++it)
      {
        if ((*it)->m_paused || !(*it)->m_processingBuffers)
          continue;

        if (!(*it)->m_processingBuffers->m_outputSamples.empty())
        {
          CSampleBuffer *tmp = SyncStream(*it);
          m_stats.UpdateStream(*it);
          if (tmp)
          {
            if (!out)
              out = tmp;
            continue;
          }

          (*it)->m_started = true;

          if (!out)
          {
            out = (*it)->m_processingBuffers->m_outputSamples.front();
            (*it)->m_processingBuffers->m_outputSamples.pop_front();

            int nb_floats = out->pkt->nb_samples * out->pkt->config.channels / out->pkt->planes;
            int nb_loops = 1;
            float fadingStep = 0.0f;

            // fading
            if ((*it)->m_fadingSamples == -1)
            {
              (*it)->m_fadingSamples = m_internalFormat.m_sampleRate * (float)(*it)->m_fadingTime / 1000.0f;
              if ((*it)->m_fadingSamples > 0)
                (*it)->m_volume = (*it)->m_fadingBase;
              else
              {
                (*it)->m_volume = (*it)->m_fadingTarget;
                std::unique_lock lock((*it)->m_streamLock);
                (*it)->m_streamFading = false;
              }
            }
            if ((*it)->m_fadingSamples > 0)
            {
              nb_floats = out->pkt->config.channels / out->pkt->planes;
              nb_loops = out->pkt->nb_samples;
              float delta = (*it)->m_fadingTarget - (*it)->m_fadingBase;
              int samples = m_internalFormat.m_sampleRate * (float)(*it)->m_fadingTime / 1000.0f;
              fadingStep = delta / samples;
            }

            // for stream amplification,
            // turned off downmix normalization,
            // or if sink format is float (in order to prevent from clipping)
            // we need to run on a per sample basis
            if ((*it)->m_amplify != 1.0f || !(*it)->m_processingBuffers->DoesNormalize() ||
                (m_sinkFormat.m_dataFormat == AE_FMT_FLOAT))
            {
              nb_floats = out->pkt->config.channels / out->pkt->planes;
              nb_loops = out->pkt->nb_samples;
            }

            for(int i=0; i<nb_loops; i++)
            {
              if ((*it)->m_fadingSamples > 0)
              {
                (*it)->m_volume += fadingStep;
                (*it)->m_fadingSamples--;

                if ((*it)->m_fadingSamples == 0)
                {
                  // set variables being polled via stream interface
                  std::unique_lock lock((*it)->m_streamLock);
                  (*it)->m_streamFading = false;
                }
              }

              // volume for stream
              float volume = (*it)->m_volume * (*it)->m_rgain;
              if(nb_loops > 1)
                volume *= (*it)->m_limiter.Run((float**)out->pkt->data, out->pkt->config.channels, i*nb_floats, out->pkt->planes > 1);

              for(int j=0; j<out->pkt->planes; j++)
              {
#if defined(HAVE_SSE) && defined(__SSE__)
                CAEUtil::SSEMulArray((float*)out->pkt->data[j]+i*nb_floats, volume, nb_floats);
#else
                float* fbuffer = (float*) out->pkt->data[j]+i*nb_floats;
                for (int k = 0; k < nb_floats; ++k)
                {
                  fbuffer[k] *= volume;
                }
#endif
              }
            }
          }
          else
          {
            CSampleBuffer *mix = NULL;
            mix = (*it)->m_processingBuffers->m_outputSamples.front();
            (*it)->m_processingBuffers->m_outputSamples.pop_front();

            int nb_floats = mix->pkt->nb_samples * mix->pkt->config.channels / mix->pkt->planes;
            int nb_loops = 1;
            float fadingStep = 0.0f;

            // fading
            if ((*it)->m_fadingSamples == -1)
            {
              (*it)->m_fadingSamples = m_internalFormat.m_sampleRate * (float)(*it)->m_fadingTime / 1000.0f;
              (*it)->m_volume = (*it)->m_fadingBase;
            }
            if ((*it)->m_fadingSamples > 0)
            {
              nb_floats = mix->pkt->config.channels / mix->pkt->planes;
              nb_loops = mix->pkt->nb_samples;
              float delta = (*it)->m_fadingTarget - (*it)->m_fadingBase;
              int samples = m_internalFormat.m_sampleRate * (float)(*it)->m_fadingTime / 1000.0f;
              fadingStep = delta / samples;
            }

            // for streams amplification of turned off downmix normalization
            // we need to run on a per sample basis
            if ((*it)->m_amplify != 1.0f || !(*it)->m_processingBuffers->DoesNormalize())
            {
              nb_floats = out->pkt->config.channels / out->pkt->planes;
              nb_loops = out->pkt->nb_samples;
            }

            for(int i=0; i<nb_loops; i++)
            {
              if ((*it)->m_fadingSamples > 0)
              {
                (*it)->m_volume += fadingStep;
                (*it)->m_fadingSamples--;

                if ((*it)->m_fadingSamples == 0)
                {
                  // set variables being polled via stream interface
                  std::unique_lock lock((*it)->m_streamLock);
                  (*it)->m_streamFading = false;
                }
              }

              // volume for stream
              float volume = (*it)->m_volume * (*it)->m_rgain;
              if(nb_loops > 1)
                volume *= (*it)->m_limiter.Run((float**)mix->pkt->data, mix->pkt->config.channels, i*nb_floats, mix->pkt->planes > 1);

              for(int j=0; j<out->pkt->planes && j<mix->pkt->planes; j++)
              {
                float *dst = (float*)out->pkt->data[j]+i*nb_floats;
                float *src = (float*)mix->pkt->data[j]+i*nb_floats;
#if defined(HAVE_SSE) && defined(__SSE__)
                CAEUtil::SSEMulAddArray(dst, src, volume, nb_floats);
                for (int k = 0; k < nb_floats; ++k)
                {
                  if (fabs(dst[k]) > 1.0f)
                  {
                    needClamp = true;
                    break;
                  }
                }
#else
                for (int k = 0; k < nb_floats; ++k)
                {
                  dst[k] += src[k] * volume;
                  if (fabs(dst[k]) > 1.0f)
                    needClamp = true;
                }
#endif
              }
            }
            mix->Return();
          }
          busy = true;
        }
      }// for

      // finally clamp samples
      if (out && needClamp)
      {
        int nb_floats = out->pkt->nb_samples * out->pkt->config.channels / out->pkt->planes;
        for (int i=0; i<out->pkt->planes; i++)
        {
          CAEUtil::ClampArray((float*)out->pkt->data[i], nb_floats);
        }
      }

      // process output buffer, gui sounds, encode, viz
      if (out)
      {
        // viz
        {
          std::unique_lock lock(m_vizLock);
          if (!m_audioCallback.empty() && !m_streams.empty())
          {
            if (!m_vizInitialized || !m_vizBuffers)
            {
              Configure();
              for (auto& it : m_audioCallback)
                it->OnInitialize(2, m_vizBuffers->m_format.m_sampleRate, 32);
              m_vizInitialized = true;
            }

            if (!m_vizBuffersInput->m_freeSamples.empty())
            {
              // copy the samples into the viz input buffer
              CSampleBuffer *viz = m_vizBuffersInput->GetFreeBuffer();
              int samples = out->pkt->nb_samples;
              int bytes = samples * out->pkt->config.channels / out->pkt->planes * out->pkt->bytes_per_sample;
              for(int i= 0; i < out->pkt->planes; i++)
              {
                memcpy(viz->pkt->data[i], out->pkt->data[i], bytes);
              }
              viz->pkt->nb_samples = samples;
              m_vizBuffers->m_inputSamples.push_back(viz);
            }
            else
              CLog::Log(LOGWARNING, "ActiveAE::{} - viz ran out of free buffers", __FUNCTION__);
            AEDelayStatus status;
            m_stats.GetDelay(status);
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
            int64_t timestamp = now + status.GetDelay() * 1000;
            busy |= m_vizBuffers->ResampleBuffers(timestamp);
            while(!m_vizBuffers->m_outputSamples.empty())
            {
              CSampleBuffer *buf = m_vizBuffers->m_outputSamples.front();
              if ((now - buf->timestamp) < 0)
                break;
              else
              {
                unsigned int samples = static_cast<unsigned int>(buf->pkt->nb_samples) *
                                       buf->pkt->config.channels / buf->pkt->planes;
                for (auto& it : m_audioCallback)
                  it->OnAudioData((float*)(buf->pkt->data[0]), samples);
                buf->Return();
                m_vizBuffers->m_outputSamples.pop_front();
              }
            }
          }
          else if (m_vizBuffers)
            m_vizBuffers->Flush();
        }

        // mix gui sounds
        MixSounds(*(out->pkt));
        if (!m_sinkHasVolume || m_muted)
          Deamplify(*(out->pkt));

        if (m_mode == MODE_TRANSCODE && m_encoder)
        {
          CSampleBuffer *buf = nullptr;
          if (out->pkt->nb_samples)
          {
            buf = m_encoderBuffers->GetFreeBuffer();
            buf->pkt->nb_samples = m_encoder->Encode(out->pkt->data[0], out->pkt->planes*out->pkt->linesize,
                                                     buf->pkt->data[0], buf->pkt->planes*buf->pkt->linesize);

            // set pts of last sample
            buf->pkt_start_offset = buf->pkt->nb_samples;
            buf->timestamp = out->timestamp;
          }

          out->Return();
          out = buf;
        }
        busy = true;
      }

      // update stats
      if(out)
      {
        int samples =
            (m_mode == MODE_TRANSCODE) ? (out->pkt->nb_samples > 0 ? 1 : 0) : out->pkt->nb_samples;
        m_stats.AddSamples(samples, m_streams);
        m_sinkBuffers->m_inputSamples.push_back(out);
      }
    }
    // pass through
    else
    {
      std::list<CActiveAEStream*>::iterator it;
      CSampleBuffer *buffer;
      for (it = m_streams.begin(); it != m_streams.end(); ++it)
      {
        if (!(*it)->m_processingBuffers->m_outputSamples.empty() && !(*it)->m_paused)
        {
          (*it)->m_started = true;
          // gaps are filled with the stream's silent frame once one of its frames is on the wire,
          // by a sink that packs IEC 61937 itself
          CSampleBuffer* front = (*it)->m_processingBuffers->m_outputSamples.front();
          const AEPassthroughSilentFrame* silentFrame{
              m_stats.GetSinkNeedIec()
                  ? CAEPassthroughSilence::Find((*it)->m_format.m_streamInfo.m_type,
                                                front->pkt->data[0], front->pkt->nb_samples)
                  : nullptr};
          front->pkt->silent_frame = m_stats.IsOutputStamped() ? silentFrame : nullptr;
          buffer = SyncStream(*it);
          m_stats.UpdateStream(*it);
          // content that waits for a start decision with no pad free stays for the next pass
          if (!buffer && m_mode == MODE_RAW &&
              (*it)->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE &&
              front->pkt->nb_samples > 0)
            continue;
          if (!buffer)
          {
            buffer = (*it)->m_processingBuffers->m_outputSamples.front();
            (*it)->m_processingBuffers->m_outputSamples.pop_front();
          }
          if (silentFrame && buffer->pkt->nb_samples > 0)
          {
            m_stats.SetOutputStamped(true);
            buffer->pkt->silent_frame = silentFrame;
          }
          if (buffer->pkt->nb_samples > 0)
            m_stats.AddSamples(1, m_streams);
          else
            m_stats.AddSamples(0, m_streams, buffer->pkt->pause_burst_ms);
          m_sinkBuffers->m_inputSamples.push_back(buffer);
        }
      }
    }
  }

  // serve sink buffers
  busy |= m_sinkBuffers->ResampleBuffers();
  while(!m_sinkBuffers->m_outputSamples.empty())
  {
    CSampleBuffer *out = NULL;
    out = m_sinkBuffers->m_outputSamples.front();
    m_sinkBuffers->m_outputSamples.pop_front();
    m_sink.m_dataPort.SendOutMessage(CSinkDataProtocol::SAMPLE,
        &out, sizeof(CSampleBuffer*));
    busy = true;
  }

  return busy;
}

bool CActiveAE::HasWork()
{
  if (!m_sounds_playing.empty())
    return true;
  if (!m_sinkBuffers->m_inputSamples.empty())
    return true;
  if (!m_sinkBuffers->m_outputSamples.empty())
    return true;

  std::list<CActiveAEStream*>::iterator it;
  for (it = m_streams.begin(); it != m_streams.end(); ++it)
  {
    if (!(*it)->m_processingBuffers->HasWork())
      return true;
    if (!(*it)->m_processingSamples.empty())
      return true;
  }

  return false;
}

int CActiveAE::TakeFramePause(CActiveAEStream* stream, double frame)
{
  const double pause = frame + stream->m_pauseBurstCarry;
  const int pauseMs = static_cast<int>(std::lround(pause));
  stream->m_pauseBurstCarry = pause - pauseMs;
  return pauseMs;
}

void CActiveAE::MuteSamples(CActiveAEStream* stream, CSampleBuffer* buf)
{
  if (m_mode == MODE_RAW)
  {
    buf->pkt->nb_samples = 0;
    buf->pkt->pause_burst_ms = TakeFramePause(
        stream, stream->m_processingBuffers->m_inputFormat.m_streamInfo.GetDuration());
  }
  else
  {
    for (int i = 0; i < buf->pkt->planes; i++)
      memset(buf->pkt->data[i], 0, buf->pkt->linesize);
  }
}

double CActiveAE::FillSyncPad(CActiveAEStream* stream, CSampleBuffer* buf, double ms)
{
  buf->pkt->nb_samples = 0;
  buf->pkt->pause_burst_ms = 0;
  if (m_mode == MODE_RAW)
  {
    const double frame = stream->m_format.m_streamInfo.GetDuration();
    buf->pkt->silent_frame =
        stream->m_processingBuffers->m_outputSamples.front()->pkt->silent_frame;
    // a pause shorter than a frame only before anything was heard, and only without a silent frame
    if (stream->m_silentSinceSyncStart && !buf->pkt->silent_frame)
      buf->pkt->pause_burst_ms = static_cast<int>(std::lround(std::min(ms, frame)));
    else if (ms > frame / 2)
      buf->pkt->pause_burst_ms = TakeFramePause(stream, frame);
    return buf->pkt->pause_burst_ms;
  }

  int framesToDelay = static_cast<int>(std::lround(ms / 1000 * buf->pkt->config.sample_rate));
  if (framesToDelay > buf->pkt->max_nb_samples)
    framesToDelay = buf->pkt->max_nb_samples;
  if (m_mode == MODE_TRANSCODE)
  {
    if (framesToDelay > static_cast<int>(m_encoderFormat.m_frames / 2))
      framesToDelay = m_encoderFormat.m_frames;
    else
      framesToDelay = 0;
  }
  buf->pkt->nb_samples = framesToDelay;
  for (int i = 0; i < buf->pkt->planes; i++)
    memset(buf->pkt->data[i], 0, buf->pkt->linesize);
  return static_cast<double>(framesToDelay) * 1000 / buf->pkt->config.sample_rate;
}

double CActiveAE::SkipSyncSamples(CActiveAEStream* stream, CSampleBuffer* buf, double ms)
{
  if (m_mode == MODE_RAW)
  {
    // before anything was heard, the next calls pause for what a skip took too much
    const double frame = stream->m_format.m_streamInfo.GetDuration();
    if (stream->m_silentSinceSyncStart && !buf->pkt->silent_frame ? std::lround(ms) <= 0
                                                                  : ms <= frame / 2)
      return 0.0;
    buf->pkt->nb_samples = 0;
    return frame;
  }

  int framesToSkip = static_cast<int>(std::lround(ms / 1000 * buf->pkt->config.sample_rate));
  if (framesToSkip > buf->pkt->nb_samples)
    framesToSkip = buf->pkt->nb_samples;
  if (m_mode == MODE_TRANSCODE)
  {
    if (framesToSkip > static_cast<int>(m_encoderFormat.m_frames / 2))
      framesToSkip = buf->pkt->nb_samples;
    else
      framesToSkip = 0;
  }
  const int bytesToSkip =
      framesToSkip * buf->pkt->bytes_per_sample * buf->pkt->config.channels / buf->pkt->planes;
  for (int i = 0; i < buf->pkt->planes; i++)
    memmove(buf->pkt->data[i], buf->pkt->data[i] + bytesToSkip, buf->pkt->linesize - bytesToSkip);
  buf->pkt->nb_samples -= framesToSkip;
  return static_cast<double>(framesToSkip) * 1000 / buf->pkt->config.sample_rate;
}

CSampleBuffer* CActiveAE::SyncStream(CActiveAEStream *stream)
{
  CSampleBuffer *ret = NULL;

  if (!stream->m_pClock)
    return ret;

  // a freed stream reads no clock, and what it holds until the engine discards it is muted
  if (stream->m_freed)
  {
    MuteSamples(stream, stream->m_processingBuffers->m_outputSamples.front());
    return ret;
  }

  // passthrough follows a phase shift only through a start sync, which measures it once;
  // a commit made while the stream is not in sync waits for it
  if (m_mode != MODE_PCM && stream->m_syncState == CAESyncInfo::AESyncState::SYNC_INSYNC &&
      stream->m_pClock->TakeSyncShiftCommit())
  {
    stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
    CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - phase shift committed");
  }

  // a stamped stream corrects its error only through a start target
  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_ADJUST &&
      stream->m_processingBuffers->m_outputSamples.front()->pkt->silent_frame &&
      stream->m_pClock->ArmStartTarget())
    stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;

  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_START)
  {
    stream->m_syncState = CAESyncInfo::AESyncState::SYNC_MUTE;
    stream->m_syncError.Flush(100ms);
    stream->m_processingBuffers->SetRR(1.0, m_settings.atempoThreshold);
    stream->m_resampleIntegral = 0;
    stream->m_lateStartHandedOver = false;
    stream->m_pauseBurstCarry = 0.0;
    stream->m_silentSinceSyncStart = true;
    stream->m_startCut = stream->m_pClock->TakeStartCut();
    stream->m_startPad = 0.0;
    stream->m_exactStart = false;
    double period{0.0};
    stream->m_syncPhaseId = stream->m_pClock->GetSyncPhaseId(stream->m_syncShift, period);
    stream->m_syncSinkReports = m_stats.GetSinkReports();
    CLog::Log(LOGDEBUG,"ActiveAE - start sync of audio stream");
  }

  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE && stream->m_startCut > 0)
  {
    CSampleBuffer* buf = stream->m_processingBuffers->m_outputSamples.front();
    const double skipped = SkipSyncSamples(stream, buf, stream->m_startCut);
    stream->m_startCut = skipped > 0 ? stream->m_startCut - skipped : 0.0;
    stream->m_syncError.Flush(100ms);
    CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - start cut, skip {:.3f}ms", skipped);
    if (buf->pkt->nb_samples == 0)
      return ret;
  }

  double phasePeriod{0.0};
  bool startTarget{false};
  bool stampedStart{false};
  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE)
  {
    double first{0.0};
    double last{0.0};
    int targetEpoch{0};
    startTarget = stream->m_pClock->GetStartTarget(first, last, targetEpoch);
    // only a stream that keeps its rhythm on the wire waits for its decision, and it starts only
    // through one, which a running clock arms
    stampedStart =
        stream->m_processingBuffers->m_outputSamples.front()->pkt->silent_frame != nullptr;
    if (startTarget && !stampedStart)
    {
      stream->m_pClock->EndStartTarget();
      startTarget = false;
    }
    else if (!startTarget && stampedStart && stream->m_pClock->ArmStartTarget())
      startTarget = stream->m_pClock->GetStartTarget(first, last, targetEpoch);

    // the start sync measures against one published clock sync phase, and a start target's
    // decision takes only errors sampled between two reads of its epoch
    double shift{0.0};
    double startError{0.0};
    const int syncPhaseId = stream->m_pClock->GetSyncPhaseId(shift, phasePeriod);
    const bool targetMoved{startTarget && targetEpoch != stream->m_startTargetEpoch};
    stream->m_startTargetEpoch = startTarget ? targetEpoch : -1;
    if (syncPhaseId == -1 || syncPhaseId != stream->m_syncPhaseId || targetMoved)
    {
      stream->m_syncPhaseId = syncPhaseId;
      stream->m_syncShift = shift;
      stream->m_syncError.Flush(100ms);
      CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - start sync held, phase id {:d}",
                syncPhaseId);
    }
    else if (startTarget && stream->m_syncError.GetSoFar(startError))
    {
      // decided once, on the first error, so nothing moves the picture after it starts: the clock
      // takes what whole frames of padding cannot
      const double frame = stream->m_format.m_streamInfo.GetDuration();
      int frames{0};
      double move{0.0};
      if (phasePeriod <= 0)
      {
        frames = static_cast<int>(std::ceil((startError - first) / frame - 0.5));
        if (startError - frames * frame > last)
          frames++;
        move = startError - frames * frame;
      }
      else
      {
        // within the tolerance counts as one step, so a closer start is not bought with a longer
        // hold
        double closest{0.0};
        double distance{0.0};
        for (int periods = static_cast<int>(std::ceil((first - MAX_START_HOLD_MS) / phasePeriod));
             periods * phasePeriod <= last; periods++)
        {
          const double candidate = periods * phasePeriod;
          const double whole = std::round((startError - candidate) / frame);
          const double steps =
              std::max(1.0, std::ceil(std::abs(startError - candidate - whole * frame) /
                                      START_GRID_TOLERANCE_MS));
          if (closest == 0 || steps < closest ||
              (steps == closest && std::abs(candidate - first) <= distance))
          {
            closest = steps;
            move = candidate;
            distance = std::abs(candidate - first);
            frames = static_cast<int>(whole);
          }
        }
      }
      CLog::Log(LOGDEBUG, LOGAUDIO,
                "ActiveAE::SyncStream - start error {:f}, clock moved {:f}, {:d} frames pad",
                startError, move, frames);
      if (!stream->m_pClock->SetStartTarget(move, targetEpoch))
      {
        stream->m_syncState = CAESyncInfo::AESyncState::SYNC_START;
        return SyncStream(stream);
      }
      stream->m_startPad = frames * frame;
      stream->m_silentSinceSyncStart = false;
      stream->m_syncState = CAESyncInfo::AESyncState::SYNC_INSYNC;
      stream->m_syncError.Flush(1000ms);
      stream->m_resampleIntegral = 0;
    }
  }
  else if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_INSYNC && m_mode == MODE_PCM)
  {
    // the clock moved by a known shift, so samples follow it exactly and no average spans it
    double shift{0.0};
    double period{0.0};
    stream->m_pClock->GetSyncPhaseId(shift, period);
    if (shift != stream->m_syncShift)
    {
      stream->m_startPad += stream->m_syncShift - shift;
      stream->m_syncShift = shift;
      if (!stream->m_lateStartHandedOver)
        stream->m_syncError.SetErrorInterval(stream->GetErrorInterval());
    }
  }
  else if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_ADJUST &&
           !stream->m_syncErrorBooked && m_mode == MODE_PCM)
  {
    // the error not yet booked is measured anew against the moved clock
    double shift{0.0};
    double period{0.0};
    stream->m_pClock->GetSyncPhaseId(shift, period);
    if (shift != stream->m_syncShift)
    {
      stream->m_syncShift = shift;
      stream->m_syncError.SetErrorInterval(100ms);
    }
  }

  double error;
  double threshold = 100;
  // The error measured at the start of a stream is adjusted whatever the clock speed is.
  double startThreshold = threshold;
  if (stream->m_resampleMode)
  {
    threshold *= 2;
    startThreshold = threshold;
    if (stream->m_pClock)
    {
      double clockspeed = stream->m_pClock->GetClockSpeed();
      if (clockspeed >= 1.05 || clockspeed <= 0.95)
        threshold *= 5;
    }
  }

  std::chrono::milliseconds timeout = (stream->m_syncState != CAESyncInfo::AESyncState::SYNC_INSYNC)
                                          ? 100ms
                                          : stream->GetErrorInterval();
  // a start error handed to the player stays reported until the player reads it
  bool newerror = !stampedStart &&
                  (!stream->m_lateStartHandedOver || m_stats.IsSyncErrorTaken(stream)) &&
                  stream->m_syncError.Get(error, timeout);

  if (newerror && stream->m_lateStartHandedOver)
  {
    // Mostly averaged before the player moved its clock to the late audio, so neither acted upon
    // nor reported.
    stream->m_lateStartHandedOver = false;
    stream->m_syncError.Flush(timeout);
    newerror = false;
    CLog::Log(LOGDEBUG, "ActiveAE::SyncStream - average error {:f} dropped", error);
  }

  // Without resampling, the player moves its clock to the audio once the error exceeds its own
  // limit. Audio starting late is therefore left to the player once it was heard, as skipping it
  // would cut audio already playing.
  const bool clockFollowsAudio{!stream->m_resampleMode};
  const auto keepAudible = [&](double syncError)
  {
    const bool lateStart{clockFollowsAudio && syncError < 0 && -syncError <= MAX_LATE_START_MS};
    // a late start is skipped before it is heard, which keeps the clock sync phase
    if (lateStart && phasePeriod > 0)
      return false;
    return fabs(syncError) <= startThreshold || lateStart;
  };

  if (newerror && fabs(error) > threshold && stream->m_syncState == CAESyncInfo::AESyncState::SYNC_INSYNC)
  {
    stream->m_syncState = CAESyncInfo::AESyncState::SYNC_ADJUST;
    stream->m_processingBuffers->SetRR(1.0, m_settings.atempoThreshold);
    stream->m_resampleIntegral = 0;
    // a long average can span a step, so the error to correct is measured anew
    stream->m_syncErrorBooked = false;
    stream->m_startPad = 0.0;
    stream->m_exactStart = false;
    stream->m_syncError.SetErrorInterval(100ms);
    CLog::Log(LOGDEBUG, "ActiveAE::SyncStream - average error {:f} above threshold of {:f}", error,
              threshold);
  }
  else if (newerror && stream->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE)
  {
    if (clockFollowsAudio && error < 0 && -error <= MAX_LATE_START_MS &&
        !stream->m_silentSinceSyncStart)
    {
      // a clock move by whole phase periods keeps the clock sync phase; the rest is padded
      double handover = error;
      if (phasePeriod > 0)
      {
        handover = -std::ceil(-error / phasePeriod) * phasePeriod;
        stream->m_startPad = error - handover;
        stream->m_exactStart = true;
      }
      stream->m_syncState = CAESyncInfo::AESyncState::SYNC_INSYNC;
      stream->m_syncError.Flush(1000ms);
      // Reports the error to the player right away
      stream->m_syncError.Correction(handover);
      stream->m_resampleIntegral = 0;
      stream->m_lateStartHandedOver = true;
      CLog::Log(LOGDEBUG,
                "ActiveAE::SyncStream - average error of {:f}, {:f} left to the player's clock",
                error, handover);
    }
    else
    {
      stream->m_syncState = CAESyncInfo::AESyncState::SYNC_ADJUST;
      stream->m_lastSyncError = error;
      stream->m_syncErrorBooked = true;
      CLog::Log(LOGDEBUG, "ActiveAE::SyncStream - average error of {:f}, start adjusting", error);
    }
  }
  else if (newerror && stream->m_syncState == CAESyncInfo::AESyncState::SYNC_ADJUST &&
           !stream->m_syncErrorBooked)
  {
    stream->m_lastSyncError = error;
    stream->m_syncErrorBooked = true;
    CLog::Log(LOGDEBUG, "ActiveAE::SyncStream - average error of {:f}, start adjusting", error);
  }

  // While the error is averaged, samples that will not be skipped stay audible. Not in passthrough,
  // where the correction that follows interrupts the bitstream the receiver just locked on to.
  double errorSoFar{0.0};
  const bool audible{m_mode != MODE_RAW && stream->m_syncError.GetSoFar(errorSoFar) &&
                     keepAudible(errorSoFar)};
  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE && audible)
    stream->m_silentSinceSyncStart = false;
  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_MUTE && !audible)
  {
    CSampleBuffer *buf = stream->m_processingBuffers->m_outputSamples.front();
    if (stampedStart && buf->timestamp)
    {
      // measured content waits for the decision behind pads, which count as sink reports
      ret = m_silenceBuffers->GetFreeBuffer();
      if (ret)
        FillSyncPad(stream, ret, stream->m_format.m_streamInfo.GetDuration());
    }
    else
      MuteSamples(stream, buf);
  }
  else if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_ADJUST &&
           stream->m_syncErrorBooked)
  {
    // the error measured once when adjusting started, less what was corrected since
    error = stream->m_lastSyncError;
    bool corrected = false;
    if (error > 0)
    {
      ret = m_silenceBuffers->GetFreeBuffer();
      if (ret)
      {
        const double delayed = FillSyncPad(stream, ret, error);
        stream->m_syncError.Correction(-delayed);
        error = std::max(error - delayed, 0.0);
        CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - delay {:.3f}ms error {:.0f}ms",
                  delayed, error);

        if ((ret->pkt->nb_samples == 0) && (ret->pkt->pause_burst_ms == 0))
        {
          ret->Return();
          ret = nullptr;
          corrected = true;
        }
      }
      else
        stream->m_silentSinceSyncStart = false;
    }
    else
    {
      CSampleBuffer *buf = stream->m_processingBuffers->m_outputSamples.front();
      const double skipped = SkipSyncSamples(stream, buf, -error);
      stream->m_syncError.Correction(skipped);
      error += skipped;
      corrected = skipped == 0 && (m_mode == MODE_RAW || buf->pkt->nb_samples > 0);
      CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - skip {:.3f}ms error {:.0f}ms", skipped,
                error);
    }

    stream->m_lastSyncError = error;
    if (corrected)
    {
      stream->m_syncState = CAESyncInfo::AESyncState::SYNC_INSYNC;
      stream->m_syncError.Flush(1000ms);
      stream->m_resampleIntegral = 0;
      stream->m_silentSinceSyncStart = false;
      stream->m_processingBuffers->SetRR(1.0, m_settings.atempoThreshold);
      CLog::Log(LOGDEBUG, "ActiveAE::SyncStream - error corrected, {:f} left", error);
    }

    return ret;
  }

  if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_INSYNC && stream->m_startPad > 0)
  {
    ret = m_silenceBuffers->GetFreeBuffer();
    if (ret)
    {
      const double delayed = FillSyncPad(stream, ret, stream->m_startPad);
      CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - start pad {:.3f}ms of {:.3f}ms",
                delayed, stream->m_startPad);
      if (delayed > 0)
      {
        stream->m_startPad -= delayed;
        // a shift padded over several passes leaves no average of the samples before its end
        if (!stream->m_lateStartHandedOver)
          stream->m_syncError.SetErrorInterval(stream->GetErrorInterval());
      }
      else
      {
        stream->m_startPad = 0.0;
        stream->m_silentSinceSyncStart = false;
        ret->Return();
        ret = nullptr;
      }
    }
    else
      stream->m_silentSinceSyncStart = false;
  }
  else if (stream->m_syncState == CAESyncInfo::AESyncState::SYNC_INSYNC && stream->m_startPad < 0)
  {
    CSampleBuffer* buf = stream->m_processingBuffers->m_outputSamples.front();
    const double skipped = SkipSyncSamples(stream, buf, -stream->m_startPad);
    CLog::Log(LOGDEBUG, LOGAUDIO, "ActiveAE::SyncStream - shift skip {:.3f}ms of {:.3f}ms", skipped,
              -stream->m_startPad);
    if (skipped > 0)
    {
      stream->m_startPad += skipped;
      if (!stream->m_lateStartHandedOver)
        stream->m_syncError.SetErrorInterval(stream->GetErrorInterval());
    }
    else
      stream->m_startPad = 0.0;
  }

  if (!newerror || stream->m_syncState != CAESyncInfo::AESyncState::SYNC_INSYNC)
    return ret;

  if (stream->m_resampleMode)
  {
    if (stream->m_processingBuffers)
    {
      stream->m_processingBuffers->SetRR(stream->CalcResampleRatio(error), m_settings.atempoThreshold);
    }
  }
  else if (stream->m_processingBuffers)
  {
    stream->m_processingBuffers->SetRR(1.0, m_settings.atempoThreshold);
  }

  stream->m_syncError.SetErrorInterval(stream->GetErrorInterval());

  return ret;
}

void CActiveAE::MixSounds(CSoundPacket &dstSample)
{
  if (m_sounds_playing.empty())
    return;

  float volume;
  float *out;
  float *sample_buffer;
  int max_samples = dstSample.nb_samples;

  std::list<SoundState>::iterator it;
  for (it = m_sounds_playing.begin(); it != m_sounds_playing.end(); )
  {
    if (!it->sound->IsConverted())
      ResampleSound(it->sound);
    int available_samples = it->sound->GetSound(false)->nb_samples - it->samples_played;
    int mix_samples = std::min(max_samples, available_samples);
    int start = it->samples_played *
                av_get_bytes_per_sample(it->sound->GetSound(false)->config.fmt) *
                it->sound->GetSound(false)->config.channels /
                it->sound->GetSound(false)->planes;

    for(int j=0; j<dstSample.planes; j++)
    {
      volume = it->sound->GetVolume();
      out = (float*)dstSample.data[j];
      sample_buffer = (float*)(it->sound->GetSound(false)->data[j]+start);
      int nb_floats = mix_samples * dstSample.config.channels / dstSample.planes;
#if defined(HAVE_SSE) && defined(__SSE__)
      CAEUtil::SSEMulAddArray(out, sample_buffer, volume, nb_floats);
#else
      for (int k = 0; k < nb_floats; ++k)
        *out++ += *sample_buffer++ * volume;
#endif
    }

    it->samples_played += mix_samples;

    // no more frames, so remove it from the list
    if (it->samples_played >= it->sound->GetSound(false)->nb_samples)
    {
      it = m_sounds_playing.erase(it);
      continue;
    }
    ++it;
  }
}

void CActiveAE::Deamplify(CSoundPacket &dstSample)
{
  if (m_volumeScaled < 1.0f || m_muted)
  {
    int nb_floats = dstSample.nb_samples * dstSample.config.channels / dstSample.planes;
    float volume = m_muted ? 0.0f : m_volumeScaled;

    for(int j=0; j<dstSample.planes; j++)
    {
      float* buffer = reinterpret_cast<float*>(dstSample.data[j]);
#if defined(HAVE_SSE) && defined(__SSE__)
      CAEUtil::SSEMulArray(buffer, volume, nb_floats);
#else
      float *fbuffer = buffer;
      for (int i = 0; i < nb_floats; i++)
        *fbuffer++ *= volume;
#endif
    }
  }
}

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

void CActiveAE::LoadSettings(AEAudioFormat *format)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  int soc_id = aml_get_cpufamily_id();

  m_settings.device = settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE);
  m_settings.passthroughdevice = settings->GetString(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE);

  // find on NEWSTREAM the matching passthrough device by device type
  if (format != NULL && (soc_id >= AML_S5 || soc_id == AML_T7))
  {
    std::string device(m_settings.passthroughdevice);
    if (SupportsFormat(*format, &device))
    {
      if (device != m_settings.passthroughdevice)
      {
        CLog::LogF(LOGINFO, "Change temporary passthrough output device because of Amlogic SoC '{}', stream type: {:d}",
           aml_get_cpufamily_name(), format->m_streamInfo.m_type);
        m_settings.passthroughdevice = device;
      }
    }
  }

  m_settings.config = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG);
  m_settings.channels = (m_sink.GetDeviceType(m_settings.device) == AE_DEVTYPE_IEC958) ? AE_CH_LAYOUT_2_0 : settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS);
  m_settings.samplerate = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE);

  m_settings.stereoupmix = IsSettingVisible(CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX) ? settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX) : false;
  m_settings.normalizelevels = !settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME);
  m_settings.guisoundmode = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDMODE);

  m_settings.passthrough = m_settings.config == AE_CONFIG_FIXED ? false : settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH);
  if (!m_sink.HasPassthroughDevice())
    m_settings.passthrough = false;
  m_settings.ac3passthrough = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH);
  m_settings.ac3transcode = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE);
  m_settings.eac3passthrough = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH);
  m_settings.truehdpassthrough = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH);
  m_settings.dtspassthrough = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH);
  m_settings.dtshdpassthrough = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH);
  m_settings.usesdtscorefallback =
      settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK);

  m_settings.resampleQuality = static_cast<AEQuality>(settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_PROCESSQUALITY));
  m_settings.atempoThreshold = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD) / 100.0;
  m_settings.streamNoise = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_STREAMNOISE);
  m_settings.silenceTimeoutMinutes = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_STREAMSILENCE);
  m_settings.mixSubLevel = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_MIXSUBLEVEL) / 100.0;
  m_settings.lowLatencyMode = settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_LOWLATENCY);
}

void CActiveAE::ValidateOutputDevices(bool saveChanges)
{
  std::string device = m_sink.ValidateOuputDevice(m_settings.device, false);

  if (!device.empty() && device != m_settings.device)
  {
    CLog::LogF(LOGWARNING, "audio output device setting has been updated from '{}' to '{}'",
               m_settings.device, device);

    const AESinkDevice oldDevice = CAESinkFactory::ParseDevice(m_settings.device);
    const AESinkDevice newDevice = CAESinkFactory::ParseDevice(device);

    m_settings.device = device;

    if (saveChanges && newDevice.IsSameDeviceAs(oldDevice))
    {
      const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
      if (settings)
      {
        settings->SetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE, m_settings.device);
        settings->Save();
        CLog::LogF(LOGDEBUG, "the change of the audio output device setting has been saved");
      }
    }
  }

  device = m_sink.ValidateOuputDevice(m_settings.passthroughdevice, true);

  if (!device.empty() && device != m_settings.passthroughdevice)
  {
    CLog::LogF(LOGWARNING, "passthrough output device setting has been updated from '{}' to '{}'",
               m_settings.passthroughdevice, device);

    const AESinkDevice oldDevice = CAESinkFactory::ParseDevice(m_settings.passthroughdevice);
    const AESinkDevice newDevice = CAESinkFactory::ParseDevice(device);

    m_settings.passthroughdevice = device;

    if (saveChanges && newDevice.IsSameDeviceAs(oldDevice))
    {
      const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
      if (settings)
      {
        settings->SetString(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE,
                            m_settings.passthroughdevice);
        settings->Save();
        CLog::LogF(LOGDEBUG, "the change of the passthrough output device setting has been saved");
      }
    }
  }
}

void CActiveAE::HandleDeviceCountChange(const std::string& driver, bool defaultDeviceChanged)
{
  if (defaultDeviceChanged)
    CLog::LogF(LOGDEBUG, "default device change event");
  else
    CLog::LogF(LOGDEBUG, "device count change event from driver: {}", driver);

  const std::string currentDriver = m_openedDriver;
  const std::string currentDevice = m_openedDevice;

  m_sink.EnumerateSinkList(true, driver);

  const bool passthrough = m_mode == MODE_RAW;
  const std::string configuredDevice =
      passthrough ? m_settings.passthroughdevice : m_settings.device;
  const AESinkDevice configured = CAESinkFactory::ParseDevice(configuredDevice);
  const std::string validatedDevice = m_sink.ValidateOuputDevice(configuredDevice, passthrough);
  const AESinkDevice validated = CAESinkFactory::ParseDevice(validatedDevice);

  const INTERNAL::DeviceChangeDecision decision{
      m_sink.DeviceExist(currentDriver, currentDevice),
      defaultDeviceChanged,
      m_currentDeviceFollowsDefault,
      IsDefaultDevice(configured),
      !validatedDevice.empty() && IsPreferredDeviceAvailable(configured, validated),
      IsSameDevice(currentDriver, currentDevice, validated)};

  if (!INTERNAL::ShouldReconfigure(decision))
    return;

  UnconfigureSink();
  LoadSettings();
  ValidateOutputDevices(false);
  m_extErrorRetries = 0;
  m_extError = false;
  Configure();
  if (!m_extError)
  {
    m_state = AE_TOP_CONFIGURED_PLAY;
    m_extTimeout = 0ms;
  }
  else
  {
    m_state = AE_TOP_ERROR;
    m_extTimeout = 500ms;
  }
}

void CActiveAE::Start()
{
  Create();
  Message *reply;
  if (m_controlPort.SendOutMessageSync(CActiveAEControlProtocol::INIT, &reply, 10s))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    reply->Release();
    if (!success)
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error", __FUNCTION__);
    }
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to init", __FUNCTION__);
  }

  m_inMsgEvent.Reset();
}

void CActiveAE::EnumerateOutputDevices(AEDeviceList &devices, bool passthrough)
{
  m_sink.EnumerateOutputDevices(devices, passthrough);
}

void CActiveAE::OnSettingsChange()
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::RECONFIGURE);
}

bool CActiveAE::SupportsFormat(AEAudioFormat &format, std::string *device)
{
  AEDeviceList devices;
  AEDeviceType pt_device_type =
    m_sink.GetDeviceType(CServiceBroker::GetSettingsComponent()->GetSettings()->GetString(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE));

  m_sink.EnumerateOutputDevices(devices, true);

  for (AEDeviceList::const_iterator sink = devices.begin(); sink != devices.end(); ++sink)
  {
    if (m_sink.GetDeviceType(sink->second) == pt_device_type)
    {
      if (m_sink.SupportsFormat(sink->second, format))
      {
        if (device != NULL)
          *device = static_cast<std::string>(sink->second);
        return true;
      }
    }
  }

  return false;
}

bool CActiveAE::SupportsRaw(AEAudioFormat &format)
{
  // check if passthrough is enabled
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH))
    return false;

  // fixed config disabled passthrough
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) == AE_CONFIG_FIXED)
    return false;

  // check if the format is enabled in settings
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !m_settings.ac3passthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_512 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_1024 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_2048 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && !m_settings.eac3passthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD && !m_settings.truehdpassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD && !m_settings.dtshdpassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA && !m_settings.dtshdpassthrough)
    return false;

  if (!SupportsFormat(format))
    return false;

  return true;
}

bool CActiveAE::UsesDtsCoreFallback()
{
  return m_settings.usesdtscorefallback;
}

bool CActiveAE::SupportsSilenceTimeout()
{
  return true;
}

bool CActiveAE::HasStereoAudioChannelCount()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string device = settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE);
  int numChannels = (m_sink.GetDeviceType(device) == AE_DEVTYPE_IEC958) ? AE_CH_LAYOUT_2_0 : settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS);
  bool passthrough = settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) == AE_CONFIG_FIXED ? false : settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH);
  return numChannels == AE_CH_LAYOUT_2_0 && !passthrough;
}

bool CActiveAE::HasHDAudioChannelCount()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string device = settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE);
  int numChannels = (m_sink.GetDeviceType(device) == AE_DEVTYPE_IEC958) ? AE_CH_LAYOUT_2_0 : settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS);
  return numChannels > AE_CH_LAYOUT_5_1;
}

bool CActiveAE::SupportsQualityLevel(enum AEQuality level)
{
  if (level == AE_QUALITY_LOW || level == AE_QUALITY_MID || level == AE_QUALITY_HIGH)
    return true;

  return false;
}

bool CActiveAE::IsSettingVisible(const std::string &settingId)
{
  if (settingId == CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE)
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (m_sink.GetDeviceType(settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE)) == AE_DEVTYPE_IEC958)
      return true;
    if (settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) == AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_CHANNELS)
  {
    if (m_sink.GetDeviceType(CServiceBroker::GetSettingsComponent()->GetSettings()->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE)) != AE_DEVTYPE_IEC958)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH)
  {
    if (m_sink.HasPassthroughDevice() && CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH)
  {
    AEAudioFormat format;
    format.m_dataFormat = AE_FMT_RAW;
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_DTS_512;
    format.m_sampleRate = 48000;
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (SupportsFormat(format) &&
        settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH)
  {
    AEAudioFormat format;
    format.m_dataFormat = AE_FMT_RAW;
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
    format.m_streamInfo.m_sampleRate = 192000;
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (SupportsFormat(format) &&
        settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH)
  {
    AEAudioFormat format;
    format.m_dataFormat = AE_FMT_RAW;
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_DTSHD;
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (SupportsFormat(format) &&
        settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH)
  {
    AEAudioFormat format;
    format.m_dataFormat = AE_FMT_RAW;
    format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (SupportsFormat(format) &&
        settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED)
      return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX)
  {
    if (m_sink.HasPassthroughDevice() ||
        CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS) > AE_CH_LAYOUT_2_0)
    return true;
  }
  else if (settingId == CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE)
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    if (m_sink.HasPassthroughDevice() &&
        settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH) &&
        settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) != AE_CONFIG_FIXED &&
        (settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS) <= AE_CH_LAYOUT_2_0 || m_sink.GetDeviceType(settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE)) == AE_DEVTYPE_IEC958))
      return true;
  }
  return false;
}

void CActiveAE::Shutdown()
{
  Dispose();
}

bool CActiveAE::Suspend()
{
  return m_controlPort.SendOutMessage(CActiveAEControlProtocol::SUSPEND);
}

bool CActiveAE::Resume()
{
  Message *reply;
  if (m_controlPort.SendOutMessageSync(CActiveAEControlProtocol::INIT, &reply, 5s))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    reply->Release();
    if (!success)
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error", __FUNCTION__);
      return false;
    }
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - failed to init", __FUNCTION__);
    return false;
  }

  m_inMsgEvent.Reset();
  return true;
}

bool CActiveAE::IsSuspended()
{
  return m_stats.IsSuspended();
}

bool CActiveAE::YieldDevice()
{
  return SendYieldDevice(true);
}

bool CActiveAE::ReclaimDevice()
{
  return SendYieldDevice(false);
}

bool CActiveAE::SendYieldDevice(bool yield)
{
  Message* reply = nullptr;
  if (!m_controlPort.SendOutMessageSync(CActiveAEControlProtocol::YIELDDEVICE, &reply, 2s, &yield,
                                        sizeof(bool)))
  {
    CLog::LogF(LOGERROR, "timed out");
    return false;
  }

  const bool success = reply->signal == CActiveAEControlProtocol::ACC;
  reply->Release();
  return success;
}

float CActiveAE::GetVolume()
{
  return m_aeVolume;
}

void CActiveAE::SetVolume(const float volume)
{
  m_aeVolume = std::max( 0.0f, std::min(1.0f, volume));
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::VOLUME, &m_aeVolume, sizeof(float));
}

void CActiveAE::SetMute(const bool enabled)
{
  m_aeMuted = enabled;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::MUTE, &m_aeMuted, sizeof(bool));
}

bool CActiveAE::IsMuted()
{
  return m_aeMuted;
}

void CActiveAE::KeepConfiguration(unsigned int millis)
{
  unsigned int timeMs = millis;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::KEEPCONFIG, &timeMs, sizeof(unsigned int));
}

void CActiveAE::DeviceChange()
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::DEVICECHANGE);
}

void CActiveAE::DefaultDeviceChange()
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::DEFAULTDEVICECHANGE);
}

void CActiveAE::DeviceCountChange(const std::string& driver)
{
  const char* name = driver.c_str();
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::DEVICECOUNTCHANGE, name,
                               driver.length() + 1);
}

bool CActiveAE::GetCurrentSinkFormat(AEAudioFormat &SinkFormat)
{
  SinkFormat = m_stats.GetCurrentSinkFormat();
  return true;
}

void CActiveAE::OnLostDisplay()
{
  Message *reply;
  if (m_controlPort.SendOutMessageSync(CActiveAEControlProtocol::DISPLAYLOST, &reply, 5s))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    reply->Release();
    if (!success)
    {
      CLog::Log(LOGERROR, "ActiveAE::{} - returned error", __FUNCTION__);
    }
  }
  else
  {
    CLog::Log(LOGERROR, "ActiveAE::{} - timed out", __FUNCTION__);
  }
}

void CActiveAE::OnResetDisplay()
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::DISPLAYRESET);
}

void CActiveAE::OnAppFocusChange(bool focus)
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::APPFOCUSED, &focus, sizeof(focus));
}

//-----------------------------------------------------------------------------
// Utils
//-----------------------------------------------------------------------------

uint8_t **CActiveAE::AllocSoundSample(SampleConfig &config, int &samples, int &bytes_per_sample, int &planes, int &linesize)
{
  uint8_t **buffer;
  planes = av_sample_fmt_is_planar(config.fmt) ? config.channels : 1;
  buffer = new uint8_t*[planes];

  // align buffer to 16 in order to be compatible with sse in CAEConvert
  av_samples_alloc(buffer, &linesize, config.channels,
                                 samples, config.fmt, 16);
  bytes_per_sample = av_get_bytes_per_sample(config.fmt);
  return buffer;
}

void CActiveAE::FreeSoundSample(uint8_t **data)
{
  av_freep(data);
  delete [] data;
}

bool CActiveAE::CompareFormat(const AEAudioFormat& lhs, const AEAudioFormat& rhs)
{
  if (lhs.m_channelLayout != rhs.m_channelLayout ||
      lhs.m_dataFormat != rhs.m_dataFormat ||
      lhs.m_sampleRate != rhs.m_sampleRate)
    return false;
  else if (lhs.m_dataFormat == AE_FMT_RAW && rhs.m_dataFormat == AE_FMT_RAW &&
           lhs.m_streamInfo.m_type != rhs.m_streamInfo.m_type)
    return false;
  else
    return true;
}

//-----------------------------------------------------------------------------
// GUI Sounds
//-----------------------------------------------------------------------------

/**
 * load sound from an audio file and store original format
 * register the sound in ActiveAE
 * later when the engine is idle it will convert the sound to sink format
 */

IAE::SoundPtr CActiveAE::MakeSound(const std::string& file)
{
  AVFormatContext *fmt_ctx = nullptr;
  AVCodecContext *dec_ctx = nullptr;
  AVIOContext *io_ctx;
  const AVInputFormat* io_fmt = nullptr;
  const AVCodec* dec = nullptr;
  SampleConfig config{};

  // No custom deleter until sound is registered
  auto sound = std::make_unique<CActiveAESound>(file, this);
  if (!sound->Prepare())
  {
    return nullptr;
  }
  int fileSize = sound->GetFileSize();

  int bufferSize = 4096;
  int blockSize = sound->GetChunkSize();
  if (blockSize > 1)
    bufferSize = blockSize;

  fmt_ctx = avformat_alloc_context();
  unsigned char* buffer = (unsigned char*)av_malloc(bufferSize);
  io_ctx = avio_alloc_context(buffer, bufferSize, 0, sound.get(), CActiveAESound::Read, NULL,
                              CActiveAESound::Seek);

  io_ctx->max_packet_size = bufferSize;

  if (!sound->IsSeekPossible())
  {
    io_ctx->seekable = 0;
    io_ctx->max_packet_size = 0;
  }

  fmt_ctx->pb = io_ctx;

  av_probe_input_buffer(io_ctx, &io_fmt, file.c_str(), nullptr, 0, 0);
  if (!io_fmt)
  {
    avformat_close_input(&fmt_ctx);
    if (io_ctx)
    {
      av_freep(&io_ctx->buffer);
      av_freep(&io_ctx);
    }
    return nullptr;
  }

  // find decoder
  if (avformat_open_input(&fmt_ctx, file.c_str(), nullptr, nullptr) == 0)
  {
    fmt_ctx->flags |= AVFMT_FLAG_NOPARSE;
    if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0)
    {
      AVCodecID codecId = fmt_ctx->streams[0]->codecpar->codec_id;
      dec = avcodec_find_decoder(codecId);
      config.sample_rate = fmt_ctx->streams[0]->codecpar->sample_rate;
      config.channels = fmt_ctx->streams[0]->codecpar->ch_layout.nb_channels;
      config.channel_layout = fmt_ctx->streams[0]->codecpar->ch_layout.u.mask;
    }
  }
  if (dec == nullptr)
  {
    avformat_close_input(&fmt_ctx);
    if (io_ctx)
    {
      av_freep(&io_ctx->buffer);
      av_freep(&io_ctx);
    }
    return nullptr;
  }

  dec_ctx = avcodec_alloc_context3(dec);
  dec_ctx->sample_rate = config.sample_rate;
  AVChannelLayout layout = {};
  if (!config.channel_layout)
    av_channel_layout_default(&layout, config.channels);
  else
    av_channel_layout_from_mask(&layout, config.channel_layout);
  config.channel_layout = layout.u.mask;
  av_channel_layout_copy(&dec_ctx->ch_layout, &layout);
  av_channel_layout_uninit(&layout);

  AVPacket* avpkt = av_packet_alloc();
  if (!avpkt)
    CLog::Log(LOGERROR, "CActiveAE::{} - av_packet_alloc failed: {}", __FUNCTION__,
              strerror(errno));

  AVFrame* decoded_frame = av_frame_alloc();
  if (!decoded_frame)
    CLog::Log(LOGERROR, "CActiveAE::{} - av_frame_alloc failed: {}", __FUNCTION__, strerror(errno));

  bool error = false;

  if (avpkt && decoded_frame && avcodec_open2(dec_ctx, dec, nullptr) >= 0)
  {
    bool init = false;

    // decode until eof
    int ret;
    while (av_read_frame(fmt_ctx, avpkt) >= 0 && !error)
    {
      ret = avcodec_send_packet(dec_ctx, avpkt);
      if (ret < 0)
      {
        error = true;
        break;
      }

      while ((ret = avcodec_receive_frame(dec_ctx, decoded_frame)) == 0)
      {
        if (!init)
        {
          int samples = fileSize / av_get_bytes_per_sample(dec_ctx->sample_fmt) / config.channels;
          config.fmt = dec_ctx->sample_fmt;
          config.bits_per_sample = dec_ctx->bits_per_coded_sample;
          sound->InitSound(true, config, samples);
          init = true;
        }
        sound->StoreSound(true, decoded_frame->extended_data,
                          decoded_frame->nb_samples, decoded_frame->linesize[0]);
      }
      av_packet_unref(avpkt);

      if (ret < 0 && ret != AVERROR(EAGAIN))
      {
        error = true;
        break;
      }
    }
    ret = avcodec_send_packet(dec_ctx, nullptr);
    while ((ret = avcodec_receive_frame(dec_ctx, decoded_frame)) != AVERROR_EOF)
    {
      if (ret == 0)
      {
        sound->StoreSound(true, decoded_frame->extended_data,
                          decoded_frame->nb_samples, decoded_frame->linesize[0]);
      }
      else
      {
        error = true;
        break;
      }
    }
  }

  av_packet_free(&avpkt);
  av_frame_free(&decoded_frame);
  av_channel_layout_uninit(&dec_ctx->ch_layout);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt_ctx);
  if (io_ctx)
  {
    av_freep(&io_ctx->buffer);
    av_freep(&io_ctx);
  }

  if (error)
  {
    return nullptr;
  }

  sound->Finish();

  // register sound
  m_dataPort.SendOutMessage(CActiveAEDataProtocol::NEWSOUND, &sound, sizeof(CActiveAESound*));

  // transfer ownership to std::unique_ptr that unregisters the sound
  return {sound.release(), IAESoundDeleter(*this)};
}

void CActiveAE::FreeSound(IAESound *sound)
{
  m_dataPort.SendOutMessage(CActiveAEDataProtocol::FREESOUND, &sound, sizeof(CActiveAESound*));
}

void CActiveAE::PlaySound(CActiveAESound *sound)
{
  m_dataPort.SendOutMessage(CActiveAEDataProtocol::PLAYSOUND, &sound, sizeof(CActiveAESound*));
}

void CActiveAE::StopSound(CActiveAESound *sound)
{
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STOPSOUND, &sound, sizeof(CActiveAESound*));
}

/**
 * resample sounds to destination format for mixing
 * destination format is either format of stream or
 * default sink format when no stream is playing
 */
void CActiveAE::ResampleSounds()
{
  if ((m_settings.guisoundmode == AE_SOUND_OFF ||
      (m_settings.guisoundmode == AE_SOUND_IDLE && !m_streams.empty())) &&
      !m_aeGUISoundForce)
    return;

  std::vector<CActiveAESound*>::iterator it;
  for (it = m_sounds.begin(); it != m_sounds.end(); ++it)
  {
    if (!(*it)->IsConverted())
    {
      ResampleSound(*it);
      // only do one sound, then yield to main loop
      break;
    }
  }
}

bool CActiveAE::ResampleSound(CActiveAESound *sound)
{
  SampleConfig orig_config, dst_config;
  uint8_t **dst_buffer;
  int dst_samples;

  if (m_mode == MODE_RAW || m_internalFormat.m_dataFormat == AE_FMT_INVALID)
    return false;

  if (!sound->GetSound(true))
    return false;

  orig_config = sound->GetSound(true)->config;

  dst_config.channel_layout = CAEUtil::GetAVChannelLayout(m_internalFormat.m_channelLayout);
  dst_config.channels = m_internalFormat.m_channelLayout.Count();
  dst_config.sample_rate = m_internalFormat.m_sampleRate;
  dst_config.fmt = CAEUtil::GetAVSampleFormat(m_internalFormat.m_dataFormat);
  dst_config.bits_per_sample = CAEUtil::DataFormatToUsedBits(m_internalFormat.m_dataFormat);
  dst_config.dither_bits = CAEUtil::DataFormatToDitherBits(m_internalFormat.m_dataFormat);

  AEChannel testChannel = sound->GetChannel();
  CAEChannelInfo outChannels;
  if (sound->GetSound(true)->config.channels == 1 && testChannel != AE_CH_NULL)
  {
    for (unsigned int out=0; out < m_internalFormat.m_channelLayout.Count(); out++)
    {
      if (m_internalFormat.m_channelLayout[out] == AE_CH_FC && testChannel != AE_CH_FC) /// To become center clear on position test ??????
        outChannels += AE_CH_FL;
      else if (m_internalFormat.m_channelLayout[out] == testChannel)
        outChannels += AE_CH_FC;
      else
        outChannels += m_internalFormat.m_channelLayout[out];
    }
  }

  std::unique_ptr<IAEResample> resampler =
      CAEResampleFactory::Create(AERESAMPLEFACTORY_QUICK_RESAMPLE);

  resampler->Init(dst_config, orig_config, false, true, M_SQRT1_2,
                  outChannels.Count() > 0 ? &outChannels : nullptr, m_settings.resampleQuality,
                  false, 0.0f);

  dst_samples = resampler->CalcDstSampleCount(sound->GetSound(true)->nb_samples,
                                              m_internalFormat.m_sampleRate,
                                              orig_config.sample_rate);

  dst_buffer = sound->InitSound(false, dst_config, dst_samples);
  if (!dst_buffer)
    return false;

  int samples = resampler->Resample(dst_buffer, dst_samples,
                                    sound->GetSound(true)->data,
                                    sound->GetSound(true)->nb_samples,
                                    1.0);

  sound->GetSound(false)->nb_samples = samples;

  sound->SetConverted(true);
  return true;
}

//-----------------------------------------------------------------------------
// Streams
//-----------------------------------------------------------------------------

IAE::StreamPtr CActiveAE::MakeStream(AEAudioFormat& audioFormat,
                                     unsigned int options,
                                     IAEClockCallback* clock)
{
  if (audioFormat.m_dataFormat <= AE_FMT_INVALID || audioFormat.m_dataFormat >= AE_FMT_MAX)
  {
    return nullptr;
  }

  if (IsSuspended())
    return NULL;

  //! @todo pass number of samples in audio packet

  AEAudioFormat format = audioFormat;
  format.m_frames = format.m_sampleRate / 10;

  if (format.m_dataFormat != AE_FMT_RAW)
  {
    format.m_frameSize = format.m_channelLayout.Count() *
                         (CAEUtil::DataFormatToBits(format.m_dataFormat) >> 3);
  }
  else
    format.m_frameSize = 1;

  MsgStreamNew msg;
  msg.format = format;
  msg.options = options;
  msg.clock = clock;

  Message *reply;
  if (m_dataPort.SendOutMessageSync(CActiveAEDataProtocol::NEWSTREAM, &reply, 10s, &msg,
                                    sizeof(MsgStreamNew)))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    if (success)
    {
      IAE::StreamPtr stream(*reinterpret_cast<CActiveAEStream**>(reply->data),
                            IAEStreamDeleter(*this));
      reply->Release();
      return stream;
    }
    reply->Release();
  }

  CLog::Log(LOGERROR, "ActiveAE::{} - could not create stream", __FUNCTION__);
  return NULL;
}

bool CActiveAE::FreeStream(IAEStream *stream, bool finish)
{
  MsgStreamFree msg;
  msg.stream = static_cast<CActiveAEStream*>(stream);
  msg.finish = finish;
  msg.stream->m_freed = true;

  Message *reply;
  if (m_dataPort.SendOutMessageSync(CActiveAEDataProtocol::FREESTREAM, &reply, 1s, &msg,
                                    sizeof(MsgStreamFree)))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    reply->Release();
    if (success)
    {
      return true;
    }
  }
  CLog::Log(LOGERROR, "CActiveAE::FreeStream - failed");
  return false;
}

void CActiveAE::FlushStream(CActiveAEStream *stream)
{
  Message *reply;
  if (m_controlPort.SendOutMessageSync(CActiveAEControlProtocol::FLUSHSTREAM, &reply, 1s, &stream,
                                       sizeof(CActiveAEStream*)))
  {
    bool success = reply->signal == CActiveAEControlProtocol::ACC;
    reply->Release();
    if (!success)
    {
      CLog::Log(LOGERROR, "CActiveAE::FlushStream - failed");
    }
  }
}

void CActiveAE::PauseStream(CActiveAEStream *stream, bool pause)
{
  //! @todo pause sink, needs api change
  if (pause)
  {
    m_stats.PauseStream(stream);
    m_controlPort.SendOutMessage(CActiveAEControlProtocol::PAUSESTREAM,
                                   &stream, sizeof(CActiveAEStream*));
  }
  else
    m_controlPort.SendOutMessage(CActiveAEControlProtocol::RESUMESTREAM,
                                   &stream, sizeof(CActiveAEStream*));
}

void CActiveAE::SetStreamAmplification(CActiveAEStream *stream, float amplify)
{
  MsgStreamParameter msg;
  msg.stream = stream;
  msg.parameter.float_par = amplify;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMAMP,
                                     &msg, sizeof(MsgStreamParameter));
}

void CActiveAE::SetStreamReplaygain(CActiveAEStream *stream, float rgain)
{
  MsgStreamParameter msg;
  msg.stream = stream;
  msg.parameter.float_par = rgain;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMRGAIN,
                                     &msg, sizeof(MsgStreamParameter));
}

void CActiveAE::SetStreamVolume(CActiveAEStream *stream, float volume)
{
  MsgStreamParameter msg;
  msg.stream = stream;
  msg.parameter.float_par = volume;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMVOLUME,
                                     &msg, sizeof(MsgStreamParameter));
}

void CActiveAE::SetStreamResampleRatio(CActiveAEStream *stream, double ratio)
{
  MsgStreamParameter msg;
  msg.stream = stream;
  msg.parameter.double_par = ratio;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMRESAMPLERATIO,
                                     &msg, sizeof(MsgStreamParameter));
}

void CActiveAE::SetStreamResampleMode(CActiveAEStream *stream, int mode)
{
  MsgStreamParameter msg;
  msg.stream = stream;
  msg.parameter.int_par = mode;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMRESAMPLEMODE,
                               &msg, sizeof(MsgStreamParameter));
}

void CActiveAE::SetStreamFFmpegInfo(CActiveAEStream *stream, int profile, enum AVMatrixEncoding matrix_encoding, enum AVAudioServiceType audio_service_type)
{
  MsgStreamFFmpegInfo msg;
  msg.stream = stream;
  msg.profile = profile;
  msg.matrix_encoding = matrix_encoding;
  msg.audio_service_type = audio_service_type;

  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMFFMPEGINFO, &msg, sizeof(MsgStreamFFmpegInfo));
}

void CActiveAE::SetStreamFade(CActiveAEStream *stream, float from, float target, unsigned int millis)
{
  MsgStreamFade msg;
  msg.stream = stream;
  msg.from = from;
  msg.target = target;
  msg.millis = millis;
  m_controlPort.SendOutMessage(CActiveAEControlProtocol::STREAMFADE,
                                     &msg, sizeof(MsgStreamFade));
}

void CActiveAE::RegisterAudioCallback(IAudioCallback* pCallback)
{
  std::unique_lock lock(m_vizLock);
  m_audioCallback.push_back(pCallback);
  m_vizInitialized = false;
}

void CActiveAE::UnregisterAudioCallback(IAudioCallback* pCallback)
{
  std::unique_lock lock(m_vizLock);
  auto it = std::ranges::find(m_audioCallback, pCallback);
  if (it != m_audioCallback.end())
    m_audioCallback.erase(it);
}
