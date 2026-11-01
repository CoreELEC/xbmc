/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

class CDVDClock;

// The clock sync phase s (vsync adjust -s) of the vsync presenter. It is none after
// CDVDClock::SetVsyncAdjust(), pending a reading, or published at the clock epoch it was read at.
// The epoch changes with the clock mapping. Every method runs under the clock's lock.
class CVsyncPhase
{
public:
  explicit CVsyncPhase(CDVDClock& clock) : m_clock(clock) {}
  CVsyncPhase(const CVsyncPhase&) = delete;
  CVsyncPhase& operator=(const CVsyncPhase&) = delete;

  //! The display vsync period the clock sync holds the clock to, 0 when off or without the
  //! vsync presenter
  void SetPhasePeriod(double period);
  void SetVsyncPhasePending();
  //! Ends a pending or published phase and clears the phase period; a vsync adjust set
  //! without a phase stays
  void EndVsyncPhase();
  //! Takes the reading of the phase made with the clock of `epoch`, DVD_NOPTS_VALUE for none,
  //! and returns the phase that holds: a running mean of the readings since the last restart,
  //! the newest weighted at least as one of the 31 in the render manager's clock sync average.
  //! A phase not read at the current epoch ends when no reading can come; an ended one starts
  //! again from a reading only with `restart`. At a whole phase period off, the clock and the
  //! phase move by it together.
  double ApplyVsyncPhaseReading(double reading, int epoch, bool restart);
  //! 0 without a phase, -1 while pending or published at an older epoch, else an id that
  //! changes at a new epoch and after pending or none
  int GetVsyncPhaseId();
  //! As GetVsyncPhaseId(), with `shift` 0 and the phase period, 0 without a phase or while the
  //! speed is adjusted
  int FollowVsyncPhase(double& shift, double& period);

private:
  friend class CDVDClock;

  enum class State
  {
    NONE,
    PENDING,
    PUBLISHED,
  };

  void Reset() { m_state = State::NONE; }

  // the readings in one average of the render manager's clock sync
  static constexpr int PHASE_READINGS = 31;

  CDVDClock& m_clock;
  double m_phasePeriod{0.0};
  State m_state{State::NONE};
  int m_epoch{0};
  int m_generation{0};
  int m_readings{0};
};
