/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDClock.h"

#include "VideoReferenceClock.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"

#include <inttypes.h>
#include <math.h>
#include <memory>
#include <mutex>

namespace
{
// how far the running clock starts behind a start target: past the decision (60 to 130 ms after
// the start) and under the second after which subtitles reset on a clock that went back
constexpr double START_LEAD_MS = 500.0;
} // namespace

CDVDClock::CDVDClock()
{
  std::unique_lock lock(m_systemsection);

  m_pauseClock = 0;
  m_bReset = true;
  m_paused = false;
  m_speedAfterPause = DVD_PLAYSPEED_PAUSE;
  m_iDisc = 0;
  m_maxspeedadjust = 5.0;
  m_systemAdjust = 0;
  m_speedAdjust = 0;
  m_startClock = 0;
  m_vSyncAdjust = 0;
  m_frameTime = DVD_TIME_BASE / 60.0;

  m_videoRefClock = std::make_unique<CVideoReferenceClock>();
  m_lastSystemTime = m_videoRefClock->GetTime();
  m_systemOffset = m_videoRefClock->GetTime();
  m_systemFrequency = CurrentHostFrequency();
  m_systemUsed = m_systemFrequency;
}

CDVDClock::~CDVDClock() = default;

// Returns the current absolute clock in units of DVD_TIME_BASE (usually microseconds).
double CDVDClock::GetAbsoluteClock(bool interpolated /*= true*/)
{
  std::unique_lock lock(m_systemsection);

  int64_t current;
  current = m_videoRefClock->GetTime(interpolated);

  return SystemToAbsolute(current);
}

double CDVDClock::GetClock(bool interpolated /*= true*/)
{
  std::unique_lock lock(m_critSection);

  int64_t current = m_videoRefClock->GetTime(interpolated);
  m_systemAdjust += m_speedAdjust * (current - m_lastSystemTime);
  m_lastSystemTime = current;

  const double clock = SystemToPlaying(current);
  return clock + GetStartTargetLead(current);
}

double CDVDClock::GetClock(double& absolute, bool interpolated /*= true*/)
{
  std::unique_lock lock(m_critSection);

  const double clock = GetRunningClock(absolute, interpolated);
  return clock + GetStartTargetLead(m_lastSystemTime);
}

double CDVDClock::GetRunningClock(double& absolute, bool interpolated /*= true*/)
{
  std::unique_lock critLock(m_critSection);
  int64_t current = m_videoRefClock->GetTime(interpolated);

  std::unique_lock lock(m_systemsection);
  absolute = SystemToAbsolute(current);

  m_systemAdjust += m_speedAdjust * (current - m_lastSystemTime);
  m_lastSystemTime = current;

  return SystemToPlaying(current);
}

double CDVDClock::GetClockWithVsyncAdjust()
{
  std::unique_lock lock(m_critSection);

  double absolute{0.0};
  const double clock = GetRunningClock(absolute);
  return clock + m_vSyncAdjust;
}

double CDVDClock::GetRunningClock(int& epoch, bool& target)
{
  std::unique_lock lock(m_critSection);

  double absolute{0.0};
  const double clock = GetRunningClock(absolute);
  epoch = m_clockEpoch;
  target = m_startPending;
  return clock;
}

void CDVDClock::SetVsyncAdjust(double adjustment)
{
  std::unique_lock lock(m_critSection);
  m_vSyncAdjust = adjustment;
  m_vsyncPhase.Reset();
}

void CDVDClock::Pause(bool pause)
{
  std::unique_lock lock(m_critSection);

  if (pause && !m_paused)
  {
    if (!m_pauseClock)
      m_speedAfterPause = m_systemFrequency * DVD_PLAYSPEED_NORMAL / m_systemUsed;
    else
      m_speedAfterPause = DVD_PLAYSPEED_PAUSE;

    SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_paused = true;
  }
  else if (!pause && m_paused)
  {
    m_paused = false;
    SetSpeed(m_speedAfterPause);
  }
}

bool CDVDClock::IsPaused() const
{
  std::unique_lock lock(m_critSection);
  return m_pauseClock != 0;
}

void CDVDClock::Advance(double time)
{
  std::unique_lock lock(m_critSection);

  if (m_pauseClock)
  {
    m_pauseClock += time / DVD_TIME_BASE * m_systemFrequency;
  }
}

void CDVDClock::SetSpeed(int iSpeed)
{
  // this will sometimes be a little bit of due to rounding errors, ie clock might jump a bit when changing speed
  std::unique_lock lock(m_critSection);

  if (m_paused)
  {
    m_speedAfterPause = iSpeed;
    return;
  }

  if (iSpeed != DVD_PLAYSPEED_NORMAL)
  {
    m_iDisc += GetStartTargetLead(m_videoRefClock->GetTime());
    m_startTarget = DVD_NOPTS_VALUE;
    m_startPending = false;
  }

  if (iSpeed == DVD_PLAYSPEED_PAUSE)
  {
    if (!m_pauseClock)
    {
      m_pauseClock = m_videoRefClock->GetTime();
      m_clockEpoch++;
    }
    return;
  }

  int64_t current;
  int64_t newfreq = m_systemFrequency * DVD_PLAYSPEED_NORMAL / iSpeed;
  // stamped audio keeps its rhythm through a pause and starts only in whole frames, so the
  // picture waits behind the start value until the audio decided how it meets it
  const bool startTarget{m_pauseClock && iSpeed == DVD_PLAYSPEED_NORMAL && m_audioStamped &&
                         !m_bReset};

  if (m_pauseClock || newfreq != m_systemUsed)
    m_clockEpoch++;

  current = m_videoRefClock->GetTime();
  if (m_pauseClock)
  {
    m_startClock += current - m_pauseClock;
    m_pauseClock = 0;
  }

  m_startClock = current - (int64_t)((double)(current - m_startClock) * newfreq / m_systemUsed);
  m_systemUsed = newfreq;
  if (startTarget)
    ArmStartTarget(current);
}

void CDVDClock::SetSpeedAdjust(double adjust)
{
  CLog::Log(LOGDEBUG, "CDVDClock::SetSpeedAdjust - adjusted:{:f}", adjust);

  std::unique_lock lock(m_critSection);
  m_speedAdjust = adjust;
}

double CDVDClock::GetSpeedAdjust() const
{
  std::unique_lock lock(m_critSection);
  return m_speedAdjust;
}

double CDVDClock::ErrorAdjust(double error,
                              const char* log,
                              bool exact /* = false */,
                              bool start /* = false */)
{
  std::unique_lock lock(m_critSection);

  double clock, absolute, adjustment;
  clock = GetClock(absolute);

  // skip minor updates while speed adjust is active
  // -> adjusting buffer levels
  if (!exact && !start && m_speedAdjust != 0 && error < DVD_MSEC_TO_TIME(100))
  {
    return 0;
  }

  adjustment = error;

  // a start under speed adjust has no clock sync phase to keep
  if (!exact && m_vSyncAdjust != 0 && (!start || m_speedAdjust == 0))
  {
    // Audio ahead is more noticeable then audio behind video.
    // Correct if aufio is more than 20ms ahead or more then
    // 27ms behind. In a worst case scenario we switch from
    // 20ms ahead to 21ms behind (for fps of 23.976)
    if (error > 0.02 * DVD_TIME_BASE)
      adjustment = m_frameTime;
    else if (error < -0.027 * DVD_TIME_BASE)
      adjustment = -m_frameTime;
    else
      adjustment = 0;
  }

  if (adjustment == 0)
    return 0;

  Discontinuity(clock+adjustment, absolute);

  CLog::Log(LOGDEBUG, "CDVDClock::ErrorAdjust - {} - error:{:f}, adjusted:{:f}", log, error,
            adjustment);
  return adjustment;
}

void CDVDClock::Discontinuity(double clock, double absolute)
{
  std::unique_lock lock(m_critSection);
  // a start target moves to `clock`, and the clock stays behind it by the lead left
  const double lead = GetStartTargetLead(AbsoluteToSystem(absolute));
  if (m_startTarget != DVD_NOPTS_VALUE)
    m_startTarget = clock;
  m_startClock = AbsoluteToSystem(absolute);
  if(m_pauseClock)
    m_pauseClock = m_startClock;
  m_iDisc = clock - lead;
  m_bReset = false;
  m_systemAdjust = 0;
  m_speedAdjust = 0;
  m_clockEpoch++;
}

void CDVDClock::SetMaxSpeedAdjust(double speed)
{
  std::unique_lock lock(m_speedsection);

  m_maxspeedadjust = speed;
}

//returns the refreshrate if the videoreferenceclock is running, -1 otherwise
int CDVDClock::UpdateFramerate(double fps, double* interval /*= NULL*/)
{
  //sent with fps of 0 means we are not playing video
  if(fps == 0.0)
    return -1;

  m_frameTime = 1/fps * DVD_TIME_BASE;

  //check if the videoreferenceclock is running, will return -1 if not
  double rate = m_videoRefClock->GetRefreshRate(interval);

  if (rate <= 0)
    return -1;

  std::unique_lock lock(m_speedsection);

  double weight = (rate * 2) / fps;

  //set the speed of the videoreferenceclock based on fps, refreshrate and maximum speed adjust set by user
  if (m_maxspeedadjust > 0.05)
  {
    if (weight / MathUtils::round_int(weight) < 1.0 + m_maxspeedadjust / 100.0 &&
      weight / MathUtils::round_int(weight) > 1.0 - m_maxspeedadjust / 100.0)
      weight = MathUtils::round_int(weight);
  }
  double speed = (rate * 2.0 ) / (fps * weight);
  lock.unlock();

  m_videoRefClock->SetSpeed(speed);

  return rate;
}

bool CDVDClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const
{
  return m_videoRefClock->GetClockInfo(MissedVblanks, ClockSpeed, RefreshRate);
}

double CDVDClock::SystemToAbsolute(int64_t system) const
{
  return DVD_TIME_BASE * (double)(system - m_systemOffset) / m_systemFrequency;
}

int64_t CDVDClock::AbsoluteToSystem(double absolute) const
{
  return (int64_t)(absolute / DVD_TIME_BASE * m_systemFrequency) + m_systemOffset;
}

double CDVDClock::SystemToPlaying(int64_t system)
{
  int64_t current;

  if (m_bReset)
  {
    m_startClock = system;
    m_systemUsed = m_systemFrequency;
    if(m_pauseClock)
      m_pauseClock = m_startClock;
    m_iDisc = 0;
    m_systemAdjust = 0;
    m_speedAdjust = 0;
    m_vSyncAdjust = 0;
    m_bReset = false;
    m_clockEpoch++;
    m_startTarget = DVD_NOPTS_VALUE;
    m_startPending = false;
  }

  if (m_pauseClock)
    current = m_pauseClock;
  else
    current = system;

  return DVD_TIME_BASE * (double)(current - m_startClock + m_systemAdjust) / m_systemUsed + m_iDisc;
}

double CDVDClock::GetClockSpeed() const
{
  std::unique_lock lock(m_critSection);

  double speed = (double)m_systemFrequency / m_systemUsed;
  return m_videoRefClock->GetSpeed() * speed + m_speedAdjust;
}

double CDVDClock::GetStartTargetLead(int64_t system)
{
  if (m_startTarget != DVD_NOPTS_VALUE && !m_bReset)
  {
    const double lead = m_startTarget - SystemToPlaying(system);
    if (lead > 0 || m_startPending)
      return lead;
  }
  m_startTarget = DVD_NOPTS_VALUE;
  m_startPending = false;
  return 0.0;
}

void CDVDClock::SetAudioStamped(bool stamped)
{
  std::unique_lock lock(m_critSection);
  m_audioStamped = stamped;
}

void CDVDClock::SetQueued(double first, double last)
{
  std::unique_lock lock(m_critSection);
  m_firstQueued = first;
  m_lastQueued = last;
}

bool CDVDClock::CanRelease(int epoch, bool target) const
{
  std::unique_lock lock(m_critSection);

  return !m_startPending && epoch >= m_startArmEpoch && (epoch == m_clockEpoch || !target);
}

bool CDVDClock::ArmStartTarget()
{
  std::unique_lock lock(m_critSection);

  if (!m_startPending && !m_pauseClock && !m_bReset && m_systemUsed == m_systemFrequency)
    ArmStartTarget(m_videoRefClock->GetTime());
  return m_startPending;
}

void CDVDClock::EndStartTarget()
{
  std::unique_lock lock(m_critSection);

  m_audioStamped = false;
  if (!m_startPending)
    return;

  m_iDisc += GetStartTargetLead(m_videoRefClock->GetTime());
  m_startTarget = DVD_NOPTS_VALUE;
  m_startPending = false;
  m_clockEpoch++;
}

bool CDVDClock::GetStartTarget(double& first, double& last, int& epoch)
{
  std::unique_lock lock(m_critSection);

  const int64_t current = m_videoRefClock->GetTime();
  const double lead = GetStartTargetLead(current);
  const double clock = SystemToPlaying(current);
  first = m_firstQueued != DVD_NOPTS_VALUE ? m_firstQueued - clock : lead;
  last = m_lastQueued != DVD_NOPTS_VALUE ? m_lastQueued - clock : lead;
  epoch = m_clockEpoch;
  return m_startPending;
}

bool CDVDClock::SetStartTarget(double move, int epoch)
{
  std::unique_lock lock(m_critSection);

  const int64_t current = m_videoRefClock->GetTime();
  const double running = SystemToPlaying(current);
  if (!m_startPending || epoch != m_clockEpoch)
    return false;

  // moved from the running clock as it is now, so a move by whole phase periods keeps the phase
  const double target = m_startTarget;
  const double clock = running + move;
  m_startTarget = DVD_NOPTS_VALUE;
  m_startPending = false;
  Discontinuity(clock, SystemToAbsolute(current));
  if (clock < target)
    m_startTarget = target;
  return true;
}

void CDVDClock::ArmStartTarget(int64_t system)
{
  const double clock = SystemToPlaying(system);
  const double target = clock + GetStartTargetLead(system);
  m_startTarget = target;
  m_startPending = true;
  m_iDisc += target - DVD_MSEC_TO_TIME(START_LEAD_MS) - clock;
  m_clockEpoch++;
  m_startArmEpoch = m_clockEpoch;
}
