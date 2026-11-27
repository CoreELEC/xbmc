/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VsyncPhase.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/CriticalSection.h"

#include <memory>
#include <stdint.h>

class CVideoReferenceClock;

class CDVDClock
{
public:

  CDVDClock();
  ~CDVDClock();

  //! The clock, held at a start target while it is pending or the running clock is behind it
  double GetClock(bool interpolated = true);
  double GetClock(double& absolute, bool interpolated = true);
  //! The clock as it runs, behind a start target, for what paces and releases pictures
  double GetRunningClock(double& absolute, bool interpolated = true);
  //! The running clock plus the vsync adjust, read together
  double GetClockWithVsyncAdjust();
  //! The running clock and the epoch of its mapping, read together, with `target` whether a start
  //! target is pending
  double GetRunningClock(int& epoch, bool& target);

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

  //! Whether the audio keeps its stream's silent frame on the wire through a pause, which a
  //! clock start from pause then holds a start target for
  void SetAudioStamped(bool stamped);
  //! The clocks at which the first and the last queued pictures are due, DVD_NOPTS_VALUE if none
  void SetQueued(double first, double last);
  //! Whether a picture release read at `epoch` may go out: none while a start target is pending,
  //! and a read from before a start target was armed or moved the clock is stale
  bool CanRelease(int epoch, bool target) const;
  //! Arms a start target at the readers' value on a running clock at normal speed unless one is
  //! pending; whether one is pending
  bool ArmStartTarget();
  //! Ends a pending start target with the clock at it, and the audio is no longer stamped
  void EndStartTarget();
  //! Whether a start target is pending, with how far ahead of the running clock the first and the
  //! last queued pictures are due and the `epoch` they were read at
  bool GetStartTarget(double& first, double& last, int& epoch);
  //! Ends a pending start target with the running clock moved by `move`; false without one or at
  //! another epoch. The readers stay at the target until the running clock reaches it.
  bool SetStartTarget(double move, int epoch);

protected:
  double SystemToAbsolute(int64_t system) const;
  int64_t AbsoluteToSystem(double absolute) const;
  double SystemToPlaying(int64_t system);
  // how far the clock at `system` is behind a start target, below 0 once it passed a pending one,
  // 0 once there is none
  double GetStartTargetLead(int64_t system);
  // holds the readers at their value as a pending start target, with the running clock the lead
  // behind it
  void ArmStartTarget(int64_t system);

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
  // a start of stamped audio holds the clock readers at this value while it is pending and then
  // until the running clock reaches it; pending until the audio sets the clock or the start ends
  double m_startTarget{DVD_NOPTS_VALUE};
  bool m_startPending{false};
  int m_startArmEpoch{0};
  double m_firstQueued{DVD_NOPTS_VALUE};
  double m_lastQueued{DVD_NOPTS_VALUE};
  bool m_audioStamped{false};
};
