/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>

namespace KODI::JUMPGATE
{

struct JumpgateScrobbleAuthority
{
  uint64_t playbackGeneration{0};
  uint64_t contentGeneration{0};
  uint64_t authGeneration{0};

  bool operator==(const JumpgateScrobbleAuthority& other) const;
};

struct JumpgateScrobbleStartAttempt
{
  uint64_t id{0};
  JumpgateScrobbleAuthority authority;
};

enum class JumpgateScrobbleStartCompletion
{
  Invalid,
  Failed,
  Commit,
  Compensate,
};

class CJumpgateScrobbleStartCoordinator final
{
public:
  std::optional<JumpgateScrobbleStartAttempt> Reserve(const JumpgateScrobbleAuthority& authority);
  void Invalidate();
  JumpgateScrobbleStartCompletion Complete(const JumpgateScrobbleStartAttempt& attempt,
                                           const JumpgateScrobbleAuthority& currentAuthority,
                                           bool requestSucceeded,
                                           bool stillEligible);
  bool FinishCompensation(const JumpgateScrobbleStartAttempt& attempt);

  uint64_t BeginCleanup();
  bool FinishCleanup(uint64_t cleanupId);

private:
  struct Reservation
  {
    JumpgateScrobbleStartAttempt attempt;
    bool stale{false};
    bool compensating{false};
  };

  std::mutex m_mutex;
  std::optional<Reservation> m_reservation;
  std::set<uint64_t> m_pendingCleanups;
  uint64_t m_nextId{1};
};

} // namespace KODI::JUMPGATE
