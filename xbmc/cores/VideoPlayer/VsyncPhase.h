/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

class CDVDClock;

// The clock sync phase of the vsync presenter. Every method runs under the clock's lock.
class CVsyncPhase
{
public:
  explicit CVsyncPhase(CDVDClock& clock) : m_clock(clock) {}
  CVsyncPhase(const CVsyncPhase&) = delete;
  CVsyncPhase& operator=(const CVsyncPhase&) = delete;

  //! The display vsync period the clock sync holds the clock to, 0 when off or without the
  //! vsync presenter
  void SetPhasePeriod(double period);
  //! 0, with `shift` 0 and the phase period, 0 while the speed is adjusted
  int FollowVsyncPhase(double& shift, double& period);

private:
  CDVDClock& m_clock;
  double m_phasePeriod{0.0};
};
