/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateScrobbleStartCoordinator.h"

namespace KODI::JUMPGATE
{

bool JumpgateScrobbleAuthority::operator==(const JumpgateScrobbleAuthority& other) const
{
  return playbackGeneration == other.playbackGeneration &&
         contentGeneration == other.contentGeneration && authGeneration == other.authGeneration;
}

std::optional<JumpgateScrobbleStartAttempt> CJumpgateScrobbleStartCoordinator::Reserve(
    const JumpgateScrobbleAuthority& authority)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_reservation || !m_pendingCleanups.empty() || authority.playbackGeneration == 0)
    return std::nullopt;

  JumpgateScrobbleStartAttempt attempt{m_nextId++, authority};
  m_reservation = Reservation{attempt, false, false};
  return attempt;
}

void CJumpgateScrobbleStartCoordinator::Invalidate()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_reservation)
    m_reservation->stale = true;
}

JumpgateScrobbleStartCompletion CJumpgateScrobbleStartCoordinator::Complete(
    const JumpgateScrobbleStartAttempt& attempt,
    const JumpgateScrobbleAuthority& currentAuthority,
    bool requestSucceeded,
    bool stillEligible)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_reservation || m_reservation->attempt.id != attempt.id ||
      !(m_reservation->attempt.authority == attempt.authority) || m_reservation->compensating)
  {
    return JumpgateScrobbleStartCompletion::Invalid;
  }

  const bool stale =
      m_reservation->stale || !(attempt.authority == currentAuthority) || !stillEligible;
  if (!requestSucceeded)
  {
    m_reservation.reset();
    return JumpgateScrobbleStartCompletion::Failed;
  }
  if (!stale)
  {
    m_reservation.reset();
    return JumpgateScrobbleStartCompletion::Commit;
  }

  m_reservation->compensating = true;
  return JumpgateScrobbleStartCompletion::Compensate;
}

bool CJumpgateScrobbleStartCoordinator::FinishCompensation(
    const JumpgateScrobbleStartAttempt& attempt)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_reservation || m_reservation->attempt.id != attempt.id || !m_reservation->compensating)
  {
    return false;
  }
  m_reservation.reset();
  return true;
}

uint64_t CJumpgateScrobbleStartCoordinator::BeginCleanup()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_reservation)
    m_reservation->stale = true;
  const uint64_t cleanupId = m_nextId++;
  m_pendingCleanups.emplace(cleanupId);
  return cleanupId;
}

bool CJumpgateScrobbleStartCoordinator::FinishCleanup(uint64_t cleanupId)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_pendingCleanups.erase(cleanupId) == 1;
}

} // namespace KODI::JUMPGATE
