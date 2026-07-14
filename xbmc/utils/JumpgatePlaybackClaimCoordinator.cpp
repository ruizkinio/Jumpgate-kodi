/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackClaimCoordinator.h"

#include <algorithm>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
void SecureClear(std::string& value)
{
  volatile char* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = '\0';
  value.clear();
}
} // namespace

CJumpgatePlaybackClaimCoordinator::CJumpgatePlaybackClaimCoordinator(
    std::shared_ptr<IJumpgatePlaybackClaimTransport> transport,
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

CJumpgatePlaybackClaimCoordinator::~CJumpgatePlaybackClaimCoordinator()
{
  Stop(false, std::chrono::milliseconds{0});
}

bool CJumpgatePlaybackClaimCoordinator::QueueClaim(uint64_t generation,
                                                   PlaybackClaimRequest request)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
  {
    ClearClaimRequest(request);
    return false;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || generation == 0 || generation <= state->latestGeneration ||
      !ReleaseStoredCompletionLocked(*state))
  {
    ClearClaimRequest(request);
    return false;
  }

  if (state->claim)
    ClearClaimRequest(state->claim->request);
  state->latestGeneration = generation;
  state->claim = ClaimJob{generation, std::move(request)};
  state->condition.notify_one();
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::QueueRelease(PlaybackReleaseRequest request)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
  {
    ClearReleaseRequest(request);
    return false;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || !QueueReleaseLocked(*state, request))
  {
    ClearReleaseRequest(request);
    return false;
  }
  state->condition.notify_one();
  return true;
}

std::optional<PlaybackClaimCompletion> CJumpgatePlaybackClaimCoordinator::TakeCompletion()
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return std::nullopt;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->completion || state->completionOffered || state->stopping)
    return std::nullopt;
  state->completionOffered = true;
  return state->completion->completion;
}

bool CJumpgatePlaybackClaimCoordinator::AcceptCompletion(uint64_t generation)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || !state->completion || !state->completionOffered ||
      state->completion->completion.generation != generation)
  {
    return false;
  }
  ClearReleaseRequest(state->completion->release);
  state->completion.reset();
  state->completionOffered = false;
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::RejectCompletion(uint64_t generation)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || !state->completion || !state->completionOffered ||
      state->completion->completion.generation != generation ||
      !ReleaseStoredCompletionLocked(*state))
  {
    return false;
  }
  state->condition.notify_one();
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::Stop(bool drainReleases, std::chrono::milliseconds timeout)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->stopping)
    {
      state->stopping = true;
      if (state->claim)
      {
        ClearClaimRequest(state->claim->request);
        state->claim.reset();
      }
      if (state->completion)
      {
        if (state->completion->completion.result.IsClaimed())
        {
          PlaybackReleaseRequest release = std::move(state->completion->release);
          if (state->pendingReleaseIds.emplace(release.sessionId).second)
            state->priorityRelease = std::move(release);
          else
            ClearReleaseRequest(release);
        }
        else
          ClearReleaseRequest(state->completion->release);
        state->completion.reset();
        state->completionOffered = false;
      }
    }
  }
  state->condition.notify_all();

  bool finishedBeforeDeadline = false;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const std::chrono::milliseconds waitTime = drainReleases
                                                   ? std::max(timeout, std::chrono::milliseconds{0})
                                                   : std::chrono::milliseconds{0};
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

void CJumpgatePlaybackClaimCoordinator::ClearClaimRequest(PlaybackClaimRequest& request)
{
  SecureClear(request.deviceToken);
  request.fingerprints.clear();
  SecureClear(request.intentUrlHash);
  request.bridgeOrigin.clear();
  request.client.reset();
  request.launchedAt = 0;
}

void CJumpgatePlaybackClaimCoordinator::ClearReleaseRequest(PlaybackReleaseRequest& request)
{
  SecureClear(request.deviceToken);
  SecureClear(request.sessionId);
  request.bridgeOrigin.clear();
}

bool CJumpgatePlaybackClaimCoordinator::QueueReleaseLocked(WorkerState& state,
                                                           PlaybackReleaseRequest& request)
{
  if (state.pendingReleaseIds.find(request.sessionId) != state.pendingReleaseIds.end())
  {
    ClearReleaseRequest(request);
    return true;
  }
  if (state.pendingReleaseIds.size() >= MAX_RETAINED_RELEASES)
    return false;
  state.pendingReleaseIds.emplace(request.sessionId);
  if (state.releases.size() < MAX_PENDING_RELEASES)
    state.releases.emplace_back(std::move(request));
  else
    state.deferredReleases.emplace_back(std::move(request));
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::ReleaseStoredCompletionLocked(WorkerState& state)
{
  if (!state.completion)
    return true;
  if (state.completion->completion.result.IsClaimed() &&
      !QueueReleaseLocked(state, state.completion->release))
  {
    return false;
  }
  if (!state.completion->completion.result.IsClaimed())
    ClearReleaseRequest(state.completion->release);
  state.completion.reset();
  state.completionOffered = false;
  return true;
}

void CJumpgatePlaybackClaimCoordinator::Run(const std::shared_ptr<WorkerState>& state)
{
  CJumpgatePlaybackClaimClient client{*state->transport};
  while (true)
  {
    std::optional<ClaimJob> claim;
    std::optional<PlaybackReleaseRequest> release;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(lock,
                            [&state]
                            {
                              return state->stopping || state->claim.has_value() ||
                                     state->priorityRelease.has_value() ||
                                     !state->releases.empty() || !state->deferredReleases.empty();
                            });
      if (state->priorityRelease)
      {
        release = std::move(state->priorityRelease);
        state->priorityRelease.reset();
      }
      else if (!state->releases.empty())
      {
        release = std::move(state->releases.front());
        state->releases.pop_front();
      }
      else if (!state->deferredReleases.empty())
      {
        release = std::move(state->deferredReleases.front());
        state->deferredReleases.pop_front();
      }
      else if (!state->stopping && state->claim)
      {
        claim = std::move(state->claim);
        state->claim.reset();
      }
      else if (state->stopping)
      {
        break;
      }
    }

    if (release)
    {
      client.Release(*release);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pendingReleaseIds.erase(release->sessionId);
      }
      ClearReleaseRequest(*release);
      continue;
    }
    if (!claim)
      continue;

    PlaybackClaimResult result = client.Claim(claim->request);
    PlaybackReleaseRequest cleanup{claim->request.bridgeOrigin, claim->request.deviceToken,
                                   result.IsClaimed() ? result.claim.sessionId : ""};

    bool releaseImmediately = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      const bool stale = state->stopping || claim->generation != state->latestGeneration;
      ClearClaimRequest(claim->request);
      if (stale)
      {
        releaseImmediately = result.IsClaimed();
        if (!releaseImmediately)
          ClearReleaseRequest(cleanup);
      }
      else if (ReleaseStoredCompletionLocked(*state))
      {
        PlaybackClaimCompletion completion{claim->generation, std::move(result)};
        state->completion = StoredCompletion{std::move(completion), std::move(cleanup)};
        state->completionOffered = false;
      }
      else
      {
        releaseImmediately = result.IsClaimed();
        if (!releaseImmediately)
          ClearReleaseRequest(cleanup);
      }
    }
    if (releaseImmediately)
    {
      client.Release(cleanup);
      ClearReleaseRequest(cleanup);
    }
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->transport.reset();
    state->finished = true;
  }
  state->finishedCondition.notify_all();
}

} // namespace KODI::JUMPGATE
