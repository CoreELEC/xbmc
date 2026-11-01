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
    if (m_state == State::PENDING || m_epoch != m_clock.m_clockEpoch)
    {
      m_clock.m_vSyncAdjust = 0;
      m_state = State::NONE;
    }
    return -m_clock.m_vSyncAdjust;
  }
  if (epoch != m_clock.m_clockEpoch || m_clock.m_bReset || m_clock.m_pauseClock)
    return -m_clock.m_vSyncAdjust;

  // the lattice branch nearest to the phase held
  double vsyncPhase = -m_clock.m_vSyncAdjust;
  const double step = reading - vsyncPhase;
  vsyncPhase += step - m_phasePeriod * std::floor(step / m_phasePeriod + 0.5);

  // a wrap moves the clock by a period so the clock minus the phase stays put
  if (std::abs(vsyncPhase) >= m_phasePeriod)
  {
    const double wrap = vsyncPhase > 0 ? m_phasePeriod : -m_phasePeriod;
    m_clock.m_iDisc -= wrap;
    vsyncPhase -= wrap;
    CLog::Log(LOGDEBUG,
              "CVsyncPhase::ApplyVsyncPhaseReading - phase wrap, disc:{:.3f} wrap:{:.3f}",
              m_clock.m_iDisc / DVD_TIME_BASE, wrap / DVD_TIME_BASE);
  }

  m_clock.m_vSyncAdjust = -vsyncPhase;

  // readers measure against one id, so a phase they have not seen gets a new one
  if (m_state != State::PUBLISHED || m_epoch != m_clock.m_clockEpoch)
    m_generation++;
  m_state = State::PUBLISHED;
  m_epoch = m_clock.m_clockEpoch;
  return -m_clock.m_vSyncAdjust;
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
  period = m_clock.m_speedAdjust != 0 ? 0.0 : m_phasePeriod;
  return GetVsyncPhaseId();
}
