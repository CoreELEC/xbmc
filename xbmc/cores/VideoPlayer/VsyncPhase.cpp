/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VsyncPhase.h"

#include "DVDClock.h"

#include <mutex>

void CVsyncPhase::SetPhasePeriod(double period)
{
  std::unique_lock lock(m_clock.m_critSection);

  m_phasePeriod = period;
}

int CVsyncPhase::FollowVsyncPhase(double& shift, double& period)
{
  std::unique_lock lock(m_clock.m_critSection);

  shift = 0.0;
  period = m_clock.m_speedAdjust != 0 ? 0.0 : m_phasePeriod;
  return 0;
}
