/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgateThreadRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace KODI::JUMPGATE
{

class IJumpgateScrobbleStopTransport
{
public:
  virtual ~IJumpgateScrobbleStopTransport() = default;
  virtual bool SendStop(const std::string& jsonBody,
                        const std::string& accessToken,
                        const std::string& clientId) = 0;
};

class CJumpgateScrobbleDispatcher final
{
public:
  using Completion = std::function<void(bool)>;

  explicit CJumpgateScrobbleDispatcher(
      std::shared_ptr<IJumpgateScrobbleStopTransport> transport,
      std::shared_ptr<CJumpgateThreadRegistry> registry = CJumpgateThreadRegistry::Global());
  ~CJumpgateScrobbleDispatcher();

  CJumpgateScrobbleDispatcher(const CJumpgateScrobbleDispatcher&) = delete;
  CJumpgateScrobbleDispatcher& operator=(const CJumpgateScrobbleDispatcher&) = delete;

  bool QueueStop(std::string cleanupId,
                 std::string jsonBody,
                 std::string accessToken,
                 std::string clientId,
                 Completion completion = {});
  bool Stop(bool drain, std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});

private:
  struct Job
  {
    std::string cleanupId;
    std::string jsonBody;
    std::string accessToken;
    std::string clientId;
  };

  struct WorkerState
  {
    explicit WorkerState(std::shared_ptr<IJumpgateScrobbleStopTransport> stopTransport)
      : transport(std::move(stopTransport))
    {
    }

    std::shared_ptr<IJumpgateScrobbleStopTransport> transport;
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable finishedCondition;
    std::deque<Job> jobs;
    std::deque<Job> deferredJobs;
    std::set<std::string> pendingIds;
    std::map<std::string, std::vector<Completion>> completions;
    bool stopping{false};
    bool finished{false};
  };

  static void ClearJob(Job& job);
  static void Run(const std::shared_ptr<WorkerState>& state);

  static constexpr std::size_t MAX_PENDING_STOPS = 8;
  static constexpr std::size_t MAX_RETAINED_STOPS = 64;
  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  CJumpgateThreadRegistry::Reservation m_registryReservation;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
