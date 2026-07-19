/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgatePlaybackClaimClient.h"
#include "JumpgateThreadRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace KODI::JUMPGATE
{

struct PlaybackClaimCompletion
{
  uint64_t generation{0};
  PlaybackClaimResult result;
};

class CJumpgatePlaybackClaimCoordinator final
{
public:
  explicit CJumpgatePlaybackClaimCoordinator(
      std::shared_ptr<IJumpgatePlaybackClaimTransport> transport,
      std::shared_ptr<CJumpgateThreadRegistry> registry = CJumpgateThreadRegistry::Global());
  ~CJumpgatePlaybackClaimCoordinator();

  CJumpgatePlaybackClaimCoordinator(const CJumpgatePlaybackClaimCoordinator&) = delete;
  CJumpgatePlaybackClaimCoordinator& operator=(const CJumpgatePlaybackClaimCoordinator&) = delete;

  bool QueueClaim(uint64_t generation, PlaybackClaimRequest request);
  bool QueueRelease(PlaybackReleaseRequest request);
  std::optional<PlaybackClaimCompletion> TakeCompletion();
  bool AcceptCompletion(uint64_t generation);
  bool RejectCompletion(uint64_t generation);

  bool Stop(bool drainReleases,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});

private:
  struct ClaimJob
  {
    uint64_t generation{0};
    PlaybackClaimRequest request;
  };

  struct ClaimCleanupRequest
  {
    std::string bridgeOrigin;
    std::string deviceToken;
    std::string historyGrant;
    std::string historyGrantKind;
    std::string sessionId;
    std::uint64_t sessionRevision{0};
  };

  struct StoredCompletion
  {
    PlaybackClaimCompletion completion;
    ClaimCleanupRequest cleanup;
  };

  struct WorkerState
  {
    explicit WorkerState(std::shared_ptr<IJumpgatePlaybackClaimTransport> claimTransport)
      : transport(std::move(claimTransport))
    {
    }

    std::shared_ptr<IJumpgatePlaybackClaimTransport> transport;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable finishedCondition;
    std::optional<ClaimJob> claim;
    std::deque<PlaybackReleaseRequest> releases;
    std::deque<ClaimCleanupRequest> cleanups;
    std::set<std::string> pendingSessionIds;
    std::optional<ClaimCleanupRequest> priorityCleanup;
    std::optional<StoredCompletion> completion;
    bool completionOffered{false};
    uint64_t latestGeneration{0};
    bool stopping{false};
    bool finished{false};
  };

  static void ClearClaimRequest(PlaybackClaimRequest& request);
  static void ClearReleaseRequest(PlaybackReleaseRequest& request);
  static void ClearCleanupRequest(ClaimCleanupRequest& request);
  static bool QueueReleaseLocked(WorkerState& state, PlaybackReleaseRequest& request);
  static bool QueueCleanupLocked(WorkerState& state,
                                 ClaimCleanupRequest& request,
                                 bool priority = false);
  static bool ReleaseStoredCompletionLocked(WorkerState& state);
  static void TerminateAndRelease(IJumpgatePlaybackClaimTransport& transport,
                                  ClaimCleanupRequest& request);
  static void Run(const std::shared_ptr<WorkerState>& state);
  static void RunLoop(const std::shared_ptr<WorkerState>& state);
  static void Finalize(const std::shared_ptr<WorkerState>& state);

  static constexpr std::size_t MAX_PENDING_SESSIONS = 256;

  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  CJumpgateThreadRegistry::Reservation m_registryReservation;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
