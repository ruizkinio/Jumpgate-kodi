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

  struct StoredCompletion
  {
    PlaybackClaimCompletion completion;
    PlaybackReleaseRequest release;
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
    std::deque<PlaybackReleaseRequest> deferredReleases;
    std::set<std::string> pendingReleaseIds;
    std::optional<PlaybackReleaseRequest> priorityRelease;
    std::optional<StoredCompletion> completion;
    bool completionOffered{false};
    uint64_t latestGeneration{0};
    bool stopping{false};
    bool finished{false};
  };

  static void ClearClaimRequest(PlaybackClaimRequest& request);
  static void ClearReleaseRequest(PlaybackReleaseRequest& request);
  static bool QueueReleaseLocked(WorkerState& state, PlaybackReleaseRequest& request);
  static bool ReleaseStoredCompletionLocked(WorkerState& state);
  static void Run(const std::shared_ptr<WorkerState>& state);

  static constexpr std::size_t MAX_PENDING_RELEASES = 8;
  static constexpr std::size_t MAX_RETAINED_RELEASES = 256;

  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
