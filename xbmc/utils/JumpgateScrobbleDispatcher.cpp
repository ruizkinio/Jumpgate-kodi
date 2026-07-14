/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateScrobbleDispatcher.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace KODI::JUMPGATE
{

CJumpgateScrobbleDispatcher::CJumpgateScrobbleDispatcher(
    std::shared_ptr<IJumpgateScrobbleStopTransport> transport,
    std::shared_ptr<CJumpgateThreadRegistry> registry)
  : m_registry(registry ? std::move(registry) : CJumpgateThreadRegistry::Global())
{
  if (!transport)
    return;
  m_registryReservation = m_registry->Reserve();
  if (!m_registryReservation)
    return;
  m_state = std::make_shared<WorkerState>(std::move(transport));
  m_worker = std::thread([state = m_state] { Run(state); });
}

CJumpgateScrobbleDispatcher::~CJumpgateScrobbleDispatcher()
{
  Stop(false, std::chrono::milliseconds{0});
}

bool CJumpgateScrobbleDispatcher::QueueStop(std::string cleanupId,
                                            std::string jsonBody,
                                            std::string accessToken,
                                            Completion completion)
{
  Job job{std::move(cleanupId), std::move(jsonBody), std::move(accessToken)};
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state || job.cleanupId.empty() || job.jsonBody.empty() || job.accessToken.empty())
  {
    ClearJob(job);
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping)
  {
    ClearJob(job);
    return false;
  }
  if (state->pendingIds.find(job.cleanupId) != state->pendingIds.end())
  {
    if (completion)
      state->completions[job.cleanupId].emplace_back(std::move(completion));
    ClearJob(job);
    return true;
  }
  if (state->pendingIds.size() >= MAX_RETAINED_STOPS)
  {
    ClearJob(job);
    return false;
  }
  state->pendingIds.emplace(job.cleanupId);
  if (completion)
    state->completions[job.cleanupId].emplace_back(std::move(completion));
  if (state->jobs.size() < MAX_PENDING_STOPS)
    state->jobs.emplace_back(std::move(job));
  else
    state->deferredJobs.emplace_back(std::move(job));
  state->condition.notify_one();
  return true;
}

bool CJumpgateScrobbleDispatcher::Stop(bool drain, std::chrono::milliseconds timeout)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->stopping = true;
  }
  state->condition.notify_all();

  bool finishedBeforeDeadline = false;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const std::chrono::milliseconds waitTime =
        drain ? std::max(timeout, std::chrono::milliseconds{0}) : std::chrono::milliseconds{0};
    finishedBeforeDeadline =
        state->finishedCondition.wait_for(lock, waitTime, [&state] { return state->finished; });
  }
  if (finishedBeforeDeadline)
  {
    if (m_worker.joinable())
      m_worker.join();
    m_registryReservation.Reset();
  }
  else if (m_worker.joinable())
  {
    m_registry->Adopt(m_worker, std::move(m_registryReservation),
                      [state](std::chrono::milliseconds waitTime)
                      {
                        std::unique_lock<std::mutex> lock(state->mutex);
                        return state->finishedCondition.wait_for(lock, waitTime, [&state]
                                                                 { return state->finished; });
                      });
  }
  m_state.reset();
  return finishedBeforeDeadline;
}

void CJumpgateScrobbleDispatcher::ClearJob(Job& job)
{
  std::fill(job.accessToken.begin(), job.accessToken.end(), '\0');
  job.accessToken.clear();
  job.jsonBody.clear();
  job.cleanupId.clear();
}

void CJumpgateScrobbleDispatcher::Run(const std::shared_ptr<WorkerState>& state)
{
  while (true)
  {
    Job job;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(
          lock, [&state]
          { return state->stopping || !state->jobs.empty() || !state->deferredJobs.empty(); });
      if (!state->jobs.empty())
      {
        job = std::move(state->jobs.front());
        state->jobs.pop_front();
      }
      else if (!state->deferredJobs.empty())
      {
        job = std::move(state->deferredJobs.front());
        state->deferredJobs.pop_front();
      }
      else
      {
        if (state->stopping)
          break;
        continue;
      }
    }

    const bool sent = state->transport->SendStop(job.jsonBody, job.accessToken);
    std::vector<Completion> completions;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->pendingIds.erase(job.cleanupId);
      auto found = state->completions.find(job.cleanupId);
      if (found != state->completions.end())
      {
        completions = std::move(found->second);
        state->completions.erase(found);
      }
    }
    ClearJob(job);
    for (const Completion& completion : completions)
      completion(sent);
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->transport.reset();
    state->finished = true;
  }
  state->finishedCondition.notify_all();
}

} // namespace KODI::JUMPGATE
