/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VsyncPhase.h"

#include "DVDClock.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

void CVsyncPhase::SetPhasePeriod(double period)
{
  std::unique_lock lock(m_clock.m_critSection);

  m_phasePeriod = period;
}

void CVsyncPhase::SetVsyncPhasePending()
{
  std::unique_lock lock(m_clock.m_critSection);

  m_clock.m_vSyncAdjust = 0;
  m_state = State::PENDING;
}

void CVsyncPhase::EndVsyncPhase()
{
  std::unique_lock lock(m_clock.m_critSection);

  m_phasePeriod = 0;
  if (m_state == State::NONE)
    return;

  m_clock.m_vSyncAdjust = 0;
  m_state = State::NONE;
}

double CVsyncPhase::ApplyVsyncPhaseReading(double reading, int epoch, bool restart)
{
  std::unique_lock lock(m_clock.m_critSection);

  if (m_state == State::NONE)
  {
    if (!restart)
      return -m_clock.m_vSyncAdjust;
    m_clock.m_vSyncAdjust = 0;
    m_state = State::PENDING;
  }

  // the audio start holds for a phase not read at this epoch only while a reading can come
  if (reading == DVD_NOPTS_VALUE || m_clock.m_speedAdjust != 0 ||
      m_clock.m_systemUsed != m_clock.m_systemFrequency || m_phasePeriod <= 0)
  {
    // the clock runs off the display while the speed is adjusted, so the mean starts again
    if (m_clock.m_speedAdjust != 0)
      m_readings = 0;
    if (m_state == State::PENDING || m_epoch != m_clock.m_clockEpoch)
    {
      m_clock.m_vSyncAdjust = 0;
      m_state = State::NONE;
    }
    return -m_clock.m_vSyncAdjust;
  }
  if (epoch != m_clock.m_clockEpoch || m_clock.m_bReset || m_clock.m_pauseClock)
    return -m_clock.m_vSyncAdjust;

  double vsyncPhase = -m_clock.m_vSyncAdjust;
  const double step = reading - vsyncPhase;
  if (m_state != State::PUBLISHED || m_epoch != m_clock.m_clockEpoch)
    m_readings = 0;
  m_readings = std::min(m_readings + 1, PHASE_READINGS);
  // the lattice branch nearest to the phase held, averaged so off-lattice pictures barely move it
  vsyncPhase += (step - m_phasePeriod * std::floor(step / m_phasePeriod + 0.5)) / m_readings;

  SetVsyncPhase(vsyncPhase);

  // readers measure against one id, so a phase they have not seen gets a new one
  if (m_state != State::PUBLISHED || m_epoch != m_clock.m_clockEpoch)
    m_generation++;
  m_state = State::PUBLISHED;
  m_epoch = m_clock.m_clockEpoch;
  return -m_clock.m_vSyncAdjust;
}

bool CVsyncPhase::ShiftVsyncPhase(double delay, double& phase)
{
  std::unique_lock lock(m_clock.m_critSection);

  if (m_state != State::PUBLISHED || m_phasePeriod <= 0 || m_clock.m_speedAdjust != 0 ||
      m_clock.m_systemUsed != m_clock.m_systemFrequency)
    return false;

  // the video moves by whole periods, so what the audio has not followed stays within half of one
  const double owed = m_shift - m_shiftFollowed;
  const double shift =
      owed + delay - m_phasePeriod * std::floor((owed + delay) / m_phasePeriod + 0.5) - owed;
  if (shift != 0)
  {
    m_shift += shift;
    m_generation++;
    SetVsyncPhase(-m_clock.m_vSyncAdjust - shift);
  }
  CLog::Log(LOGDEBUG, "CVsyncPhase::ShiftVsyncPhase - delay:{:.3f} video:{:.3f} shift:{:.3f}",
            delay / DVD_TIME_BASE, (delay - shift) / DVD_TIME_BASE, shift / DVD_TIME_BASE);
  phase = -m_clock.m_vSyncAdjust;
  return true;
}

void CVsyncPhase::SetVsyncPhase(double vsyncPhase)
{
  // a wrap moves the clock by a period so the clock minus the phase stays put
  if (std::abs(vsyncPhase) >= m_phasePeriod)
  {
    const double wrap = vsyncPhase > 0 ? m_phasePeriod : -m_phasePeriod;
    m_clock.m_iDisc -= wrap;
    vsyncPhase -= wrap;
    CLog::Log(LOGDEBUG, "CVsyncPhase::SetVsyncPhase - phase wrap, disc:{:.3f} wrap:{:.3f}",
              m_clock.m_iDisc / DVD_TIME_BASE, wrap / DVD_TIME_BASE);
  }

  m_clock.m_vSyncAdjust = -vsyncPhase;
}

int CVsyncPhase::GetVsyncPhaseId()
{
  std::unique_lock lock(m_clock.m_critSection);

  if (m_state == State::NONE)
    return 0;
  if (m_state == State::PENDING || m_epoch != m_clock.m_clockEpoch || m_clock.m_bReset)
    return -1;
  return m_generation;
}

int CVsyncPhase::FollowVsyncPhase(double& shift, double& period)
{
  std::unique_lock lock(m_clock.m_critSection);

  shift = 0.0;
  period = m_state == State::NONE || m_clock.m_speedAdjust != 0 ? 0.0 : m_phasePeriod;
  return GetVsyncPhaseId();
}
