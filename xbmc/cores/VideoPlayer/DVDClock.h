/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VsyncPhase.h"
#include "threads/CriticalSection.h"

#include <memory>
#include <stdint.h>

class CVideoReferenceClock;

class CDVDClock
{
public:

  CDVDClock();
  ~CDVDClock();

  double GetClock(bool interpolated = true);
  double GetClock(double& absolute, bool interpolated = true);
  //! The clock plus the vsync adjust, read together
  double GetClockWithVsyncAdjust();
  //! The clock and the epoch of its mapping, read together
  double GetClock(int& epoch);

  //! An `exact` error is moved by in full, as the audio engine already quantised it. A `start`
  //! handover is never skipped as a minor update.
  double ErrorAdjust(double error, const char* log, bool exact = false, bool start = false);
  void Discontinuity(double clock, double absolute);
  void Discontinuity(double clock = 0LL)
  {
    Discontinuity(clock, GetAbsoluteClock());
  }

  void Reset() { m_bReset = true; }
  void SetSpeed(int iSpeed);
  void SetSpeedAdjust(double adjust);
  double GetSpeedAdjust() const;

  double GetClockSpeed()
      const; /**< get the current speed of the clock relative normal system time */

  /* tells clock at what framerate video is, to  *
   * allow it to adjust speed for a better match */
  int UpdateFramerate(double fps, double* interval = NULL);

  void SetMaxSpeedAdjust(double speed);

  double GetAbsoluteClock(bool interpolated = true);
  double GetFrequency() { return (double)m_systemFrequency ; }

  bool GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const;
  void SetVsyncAdjust(double adjustment);
  CVsyncPhase& GetVsyncPhase() { return m_vsyncPhase; }

  void Pause(bool pause);
  bool IsPaused() const;
  void Advance(double time);

protected:
  double SystemToAbsolute(int64_t system) const;
  int64_t AbsoluteToSystem(double absolute) const;
  double SystemToPlaying(int64_t system);

  mutable CCriticalSection m_critSection;
  int64_t m_systemUsed;
  int64_t m_startClock;
  int64_t m_pauseClock;
  double m_iDisc;
  bool m_bReset;
  bool m_paused;
  int m_speedAfterPause;
  std::unique_ptr<CVideoReferenceClock> m_videoRefClock;

  int64_t m_systemFrequency;
  int64_t m_systemOffset;
  CCriticalSection m_systemsection;

  int64_t m_systemAdjust;
  int64_t m_lastSystemTime;
  double m_speedAdjust;
  double m_vSyncAdjust;
  double m_frameTime;

  double m_maxspeedadjust;
  CCriticalSection m_speedsection;

  friend class CVsyncPhase;
  int m_clockEpoch{0};
  CVsyncPhase m_vsyncPhase{*this};
};
