/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgateSubtitleClient.h"
#include "JumpgateThreadRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace KODI::JUMPGATE
{

struct JumpgateSubtitleBinding
{
  std::uint64_t generation{0};
  std::string profileId;
  std::string deviceId;
  std::string bridgeOrigin;
  std::string sessionId;
};

struct JumpgateSubtitleRequest
{
  JumpgateSubtitleBinding binding;
  CJumpgateSubtitleBearerAuthority authority;
  std::vector<std::string> languagePreferences;
};

struct JumpgateSubtitleStagedArtifact
{
  JumpgateSubtitleCandidate selected;
  std::string artifactId;
  std::int64_t expiresAt{0};
  std::vector<JumpgateSubtitleStagedPart> parts;
};

struct JumpgateSubtitleCompletion
{
  JumpgateSubtitleBinding binding;
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  int httpStatus{0};
  JumpgateSubtitleStagedArtifact artifact;
};

struct JumpgateSubtitleCoordinatorOptions
{
  std::uint32_t maximumAttempts{3};
  std::chrono::milliseconds retryAfterSecond{std::chrono::seconds{1}};
  std::chrono::milliseconds softFailureDelay{std::chrono::milliseconds{250}};
  std::function<std::int64_t()> nowMilliseconds;
  std::function<bool()> requestSafeTransportCancellation;
  std::function<void(const JumpgateSubtitleCompletion&)> completionClearObserver;
  std::function<void(const JumpgateSubtitleBinding&)> completionPublishedObserver;
};

class CJumpgateSubtitleCoordinator final
{
public:
  explicit CJumpgateSubtitleCoordinator(
      std::shared_ptr<IJumpgateSubtitleTransport> transport,
      JumpgateSubtitleCoordinatorOptions options = {},
      std::shared_ptr<CJumpgateThreadRegistry> registry = CJumpgateThreadRegistry::Global());
  ~CJumpgateSubtitleCoordinator();

  CJumpgateSubtitleCoordinator(const CJumpgateSubtitleCoordinator&) = delete;
  CJumpgateSubtitleCoordinator& operator=(const CJumpgateSubtitleCoordinator&) = delete;

  bool Queue(JumpgateSubtitleRequest request);
  bool Cancel(const JumpgateSubtitleBinding& binding);
  std::optional<JumpgateSubtitleCompletion> TakeCompletion(const JumpgateSubtitleBinding& binding);
  bool ReturnCompletion(JumpgateSubtitleCompletion&& completion);

  bool Stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});

private:
  struct Job
  {
    JumpgateSubtitleRequest request;
    std::shared_ptr<CJumpgateSubtitleCancellationSource> cancellation;
  };

  struct WorkerState
  {
    WorkerState(std::shared_ptr<IJumpgateSubtitleTransport> subtitleTransport,
                JumpgateSubtitleCoordinatorOptions coordinatorOptions)
      : transport(std::move(subtitleTransport)),
        options(std::move(coordinatorOptions))
    {
    }

    std::shared_ptr<IJumpgateSubtitleTransport> transport;
    JumpgateSubtitleCoordinatorOptions options;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable finishedCondition;
    std::optional<Job> pending;
    std::shared_ptr<CJumpgateSubtitleCancellationSource> activeCancellation;
    std::optional<JumpgateSubtitleBinding> latestBinding;
    std::optional<JumpgateSubtitleCompletion> completion;
    std::deque<JumpgateSubtitleCompletion> discardedCompletions;
    bool stopping{false};
    bool finished{false};
  };

  static bool SameBinding(const JumpgateSubtitleBinding& left,
                          const JumpgateSubtitleBinding& right);
  static void ClearCompletion(const std::shared_ptr<WorkerState>& state,
                              JumpgateSubtitleCompletion& completion);
  static JumpgateSubtitleCompletion Execute(const std::shared_ptr<WorkerState>& state,
                                            Job& job,
                                            CJumpgateSubtitleClient& client);
  static bool WaitForRetry(const std::shared_ptr<WorkerState>& state,
                           const CJumpgateSubtitleCancellationToken& cancellation,
                           std::chrono::milliseconds delay);
  static void Run(const std::shared_ptr<WorkerState>& state);

  mutable std::mutex m_ownerMutex;
  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  CJumpgateThreadRegistry::Reservation m_registryReservation;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
