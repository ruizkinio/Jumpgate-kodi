/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgateHistoryEventState.h"
#include "JumpgateThreadRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace KODI::JUMPGATE
{

class CJumpgateHistoryEventDispatcher final
{
public:
  using RetryWait = std::function<void(std::chrono::milliseconds)>;

  explicit CJumpgateHistoryEventDispatcher(
      std::shared_ptr<IJumpgatePlaybackClaimTransport> transport,
      RetryWait retryWait = {},
      std::shared_ptr<CJumpgateThreadRegistry> registry = CJumpgateThreadRegistry::Global());
  ~CJumpgateHistoryEventDispatcher();

  CJumpgateHistoryEventDispatcher(const CJumpgateHistoryEventDispatcher&) = delete;
  CJumpgateHistoryEventDispatcher& operator=(const CJumpgateHistoryEventDispatcher&) = delete;

  bool AdvanceGeneration(std::uint64_t generation);
  void CancelGeneration(std::uint64_t generation);
  bool BindClaim(JumpgateHistoryEventBinding binding,
                 JumpgateHistorySnapshot snapshot,
                 std::int64_t nowMs);
  void PlaybackStarted(bool resumed, JumpgateHistorySnapshot snapshot, std::int64_t nowMs);
  void PlaybackPaused(JumpgateHistorySnapshot snapshot);
  void SetBackgrounded(bool backgrounded, JumpgateHistorySnapshot snapshot);
  void ProcessSlow(JumpgateHistorySnapshot snapshot, std::int64_t nowMs, std::int64_t intervalMs);

  JumpgateHistoryTerminalResult FinishTerminal(std::uint64_t generation,
                                               bool completed,
                                               JumpgateHistorySnapshot snapshot);
  JumpgateHistoryTerminalResult GetTerminalResult(std::uint64_t generation) const;

  bool WaitForIdle(std::chrono::milliseconds timeout);
  bool Stop(bool drain, std::chrono::milliseconds timeout = std::chrono::milliseconds{8000});

private:
  struct WorkerState final
  {
    WorkerState(std::shared_ptr<IJumpgatePlaybackClaimTransport> value, RetryWait wait)
      : transport(std::move(value)),
        retryWait(std::move(wait))
    {
    }

    std::shared_ptr<IJumpgatePlaybackClaimTransport> transport;
    RetryWait retryWait;
    CJumpgateHistoryEventState lifecycle;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable stateChanged;
    bool accepting{true};
    bool stopping{false};
    bool busy{false};
    bool finished{false};
  };

  static bool IsRetryable(const JumpgateHistoryEventResult& result);
  static bool IsAmbiguous(const JumpgateHistoryEventResult& result);
  static bool IsRetryable(const PlaybackReleaseResult& result);
  static void Run(const std::shared_ptr<WorkerState>& state);
  static void NotifyWork(const std::shared_ptr<WorkerState>& state);

  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  CJumpgateThreadRegistry::Reservation m_registryReservation;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
