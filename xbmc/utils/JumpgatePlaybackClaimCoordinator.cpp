/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackClaimCoordinator.h"

#include "JumpgateHistoryEventClient.h"
#include "utils/StringUtils.h"

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::array<std::chrono::milliseconds, 5> RETRY_DELAYS = {
    std::chrono::milliseconds{0}, std::chrono::milliseconds{100}, std::chrono::milliseconds{250},
    std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}};

void SecureClear(std::string& value)
{
  volatile char* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = '\0';
  value.clear();
}

bool IsRetryableHttpStatus(int status)
{
  switch (status)
  {
    case 408:
    case 425:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
      return true;
    default:
      return false;
  }
}

bool IsRetryable(const JumpgateHistoryEventResult& result)
{
  return result.status == JumpgateHistoryEventStatus::TransportFailure ||
         result.status == JumpgateHistoryEventStatus::Unavailable ||
         result.status == JumpgateHistoryEventStatus::InvalidResponse ||
         IsRetryableHttpStatus(result.httpStatus);
}

bool IsRetryable(const PlaybackReleaseResult& result)
{
  return result.status == PlaybackReleaseStatus::TransportFailure ||
         result.status == PlaybackReleaseStatus::InvalidResponse ||
         IsRetryableHttpStatus(result.httpStatus);
}

void WaitBeforeRetry(std::chrono::milliseconds delay)
{
  if (delay.count() > 0)
    std::this_thread::sleep_for(delay);
}

PlaybackClaimResult SafeClaim(CJumpgatePlaybackClaimClient& client,
                              const PlaybackClaimRequest& request) noexcept
{
  try
  {
    return client.Claim(request);
  }
  catch (...)
  {
    PlaybackClaimResult result;
    result.status = PlaybackClaimStatus::TransportFailure;
    return result;
  }
}

JumpgateHistoryEventResult SafeHistorySend(CJumpgateHistoryEventClient& client,
                                           JumpgateHistoryEvent event,
                                           const JumpgateHistoryEventRequest& request,
                                           const JumpgatePlaybackHttpRequest& prepared) noexcept
{
  try
  {
    return client.SendPrepared(event, request, prepared);
  }
  catch (...)
  {
    JumpgateHistoryEventResult result;
    result.status = JumpgateHistoryEventStatus::TransportFailure;
    return result;
  }
}

PlaybackReleaseResult SafeRelease(CJumpgatePlaybackClaimClient& client,
                                  const PlaybackReleaseRequest& request) noexcept
{
  try
  {
    return client.Release(request);
  }
  catch (...)
  {
    PlaybackReleaseResult result;
    result.status = PlaybackReleaseStatus::TransportFailure;
    return result;
  }
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

  if (!state->completion->completion.result.IsClaimed() &&
      !QueueCleanupLocked(*state, state->completion->cleanup))
  {
    return false;
  }
  ClearCleanupRequest(state->completion->cleanup);
  state->completion->completion.result.ClearSensitive();
  state->completion.reset();
  state->completionOffered = false;
  state->condition.notify_one();
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
        QueueCleanupLocked(*state, state->completion->cleanup, true);
        ClearCleanupRequest(state->completion->cleanup);
        state->completion->completion.result.ClearSensitive();
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
  SecureClear(request.attemptId);
  request.fingerprints.clear();
  SecureClear(request.intentUrlHash);
  request.bridgeOrigin.clear();
  if (request.client)
  {
    request.client->platform.clear();
    request.client->version.clear();
  }
  request.client.reset();
  request.launchedAt = 0;
}

void CJumpgatePlaybackClaimCoordinator::ClearReleaseRequest(PlaybackReleaseRequest& request)
{
  request.ClearSensitive();
}

void CJumpgatePlaybackClaimCoordinator::ClearCleanupRequest(ClaimCleanupRequest& request)
{
  request.bridgeOrigin.clear();
  SecureClear(request.deviceToken);
  SecureClear(request.historyGrant);
  request.historyGrantKind.clear();
  SecureClear(request.sessionId);
  request.sessionRevision = 0;
}

bool CJumpgatePlaybackClaimCoordinator::QueueReleaseLocked(WorkerState& state,
                                                           PlaybackReleaseRequest& request)
{
  if (request.sessionId.empty() || request.terminalReceiptId.empty())
    return false;
  if (state.pendingSessionIds.find(request.sessionId) != state.pendingSessionIds.end())
  {
    ClearReleaseRequest(request);
    return true;
  }
  if (state.pendingSessionIds.size() >= MAX_PENDING_SESSIONS)
    return false;
  state.pendingSessionIds.emplace(request.sessionId);
  state.releases.emplace_back(std::move(request));
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::QueueCleanupLocked(WorkerState& state,
                                                           ClaimCleanupRequest& request,
                                                           bool priority)
{
  if (request.sessionId.empty() || request.historyGrant.empty() || request.sessionRevision == 0)
  {
    ClearCleanupRequest(request);
    return true;
  }
  if (state.pendingSessionIds.find(request.sessionId) != state.pendingSessionIds.end())
  {
    ClearCleanupRequest(request);
    return true;
  }
  if (!priority && state.pendingSessionIds.size() >= MAX_PENDING_SESSIONS)
    return false;

  state.pendingSessionIds.emplace(request.sessionId);
  if (priority && !state.priorityCleanup)
    state.priorityCleanup = std::move(request);
  else if (priority)
    state.cleanups.emplace_front(std::move(request));
  else
    state.cleanups.emplace_back(std::move(request));
  return true;
}

bool CJumpgatePlaybackClaimCoordinator::ReleaseStoredCompletionLocked(WorkerState& state)
{
  if (!state.completion)
    return true;
  if (!QueueCleanupLocked(state, state.completion->cleanup))
    return false;
  ClearCleanupRequest(state.completion->cleanup);
  state.completion->completion.result.ClearSensitive();
  state.completion.reset();
  state.completionOffered = false;
  return true;
}

void CJumpgatePlaybackClaimCoordinator::TerminateAndRelease(
    IJumpgatePlaybackClaimTransport& transport, ClaimCleanupRequest& cleanup)
{
  JumpgateHistoryEventRequest eventRequest;
  eventRequest.bridgeOrigin = cleanup.bridgeOrigin;
  eventRequest.deviceToken = cleanup.deviceToken;
  eventRequest.historyGrant = cleanup.historyGrant;
  eventRequest.historyGrantKind = cleanup.historyGrantKind;
  eventRequest.idempotencyKey = StringUtils::CreateUUID();
  eventRequest.sessionId = cleanup.sessionId;
  eventRequest.sessionRevision = cleanup.sessionRevision;

  CJumpgateHistoryEventClient historyClient{transport};
  JumpgatePlaybackHttpRequest prepared;
  JumpgateHistoryEventResult historyResult;
  if (historyClient.Prepare(JumpgateHistoryEvent::Stop, eventRequest, prepared))
  {
    for (const std::chrono::milliseconds delay : RETRY_DELAYS)
    {
      WaitBeforeRetry(delay);
      historyResult =
          SafeHistorySend(historyClient, JumpgateHistoryEvent::Stop, eventRequest, prepared);
      if (historyResult.IsAccepted() || !IsRetryable(historyResult))
        break;
    }
  }

  if (historyResult.IsAccepted())
  {
    PlaybackReleaseRequest release{cleanup.bridgeOrigin, cleanup.deviceToken, cleanup.sessionId,
                                   eventRequest.idempotencyKey};
    CJumpgatePlaybackClaimClient claimClient{transport};
    for (const std::chrono::milliseconds delay : RETRY_DELAYS)
    {
      WaitBeforeRetry(delay);
      const PlaybackReleaseResult result = SafeRelease(claimClient, release);
      if (result.status == PlaybackReleaseStatus::Released ||
          result.status == PlaybackReleaseStatus::NotFound || !IsRetryable(result))
      {
        break;
      }
    }
    release.ClearSensitive();
  }

  SecureClear(eventRequest.deviceToken);
  SecureClear(eventRequest.historyGrant);
  SecureClear(eventRequest.idempotencyKey);
  SecureClear(eventRequest.sessionId);
}

void CJumpgatePlaybackClaimCoordinator::Run(const std::shared_ptr<WorkerState>& state)
{
  try
  {
    RunLoop(state);
  }
  catch (...)
  {
  }
  Finalize(state);
}

void CJumpgatePlaybackClaimCoordinator::RunLoop(const std::shared_ptr<WorkerState>& state)
{
  CJumpgatePlaybackClaimClient client{*state->transport};
  while (true)
  {
    std::optional<ClaimJob> claim;
    std::optional<PlaybackReleaseRequest> release;
    std::optional<ClaimCleanupRequest> cleanup;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(lock,
                            [&state]
                            {
                              return state->stopping || state->claim.has_value() ||
                                     state->priorityCleanup.has_value() ||
                                     !state->releases.empty() || !state->cleanups.empty();
                            });
      if (!state->stopping && state->claim)
      {
        claim = std::move(state->claim);
        state->claim.reset();
      }
      else if (state->priorityCleanup)
      {
        cleanup = std::move(state->priorityCleanup);
        state->priorityCleanup.reset();
      }
      else if (!state->releases.empty())
      {
        release = std::move(state->releases.front());
        state->releases.pop_front();
      }
      else if (!state->cleanups.empty())
      {
        cleanup = std::move(state->cleanups.front());
        state->cleanups.pop_front();
      }
      else if (state->stopping)
      {
        break;
      }
    }

    if (release)
    {
      for (const std::chrono::milliseconds delay : RETRY_DELAYS)
      {
        WaitBeforeRetry(delay);
        const PlaybackReleaseResult result = SafeRelease(client, *release);
        if (result.status == PlaybackReleaseStatus::Released ||
            result.status == PlaybackReleaseStatus::NotFound || !IsRetryable(result))
        {
          break;
        }
      }
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pendingSessionIds.erase(release->sessionId);
      }
      ClearReleaseRequest(*release);
      continue;
    }

    if (cleanup)
    {
      TerminateAndRelease(*state->transport, *cleanup);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pendingSessionIds.erase(cleanup->sessionId);
      }
      ClearCleanupRequest(*cleanup);
      continue;
    }

    if (!claim)
      continue;

    PlaybackClaimResult result = SafeClaim(client, claim->request);
    ClaimCleanupRequest claimedCleanup;
    claimedCleanup.bridgeOrigin = claim->request.bridgeOrigin;
    claimedCleanup.deviceToken = claim->request.deviceToken;
    claimedCleanup.historyGrant = result.claim.historyGrant;
    claimedCleanup.historyGrantKind = result.claim.historyGrantKind;
    claimedCleanup.sessionId = result.claim.sessionId;
    claimedCleanup.sessionRevision = result.claim.sessionRevision;

    bool cleanupImmediately = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      const bool stale = state->stopping || claim->generation != state->latestGeneration;
      ClearClaimRequest(claim->request);
      if (stale)
      {
        cleanupImmediately = !QueueCleanupLocked(*state, claimedCleanup);
      }
      else if (ReleaseStoredCompletionLocked(*state))
      {
        PlaybackClaimCompletion completion{claim->generation, std::move(result)};
        state->completion = StoredCompletion{std::move(completion), std::move(claimedCleanup)};
        state->completionOffered = false;
      }
      else
      {
        cleanupImmediately = !QueueCleanupLocked(*state, claimedCleanup);
      }
    }
    if (cleanupImmediately)
      TerminateAndRelease(*state->transport, claimedCleanup);
    ClearCleanupRequest(claimedCleanup);
    result.ClearSensitive();
  }
}

void CJumpgatePlaybackClaimCoordinator::Finalize(const std::shared_ptr<WorkerState>& state)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->claim)
  {
    ClearClaimRequest(state->claim->request);
    state->claim.reset();
  }
  for (PlaybackReleaseRequest& release : state->releases)
    ClearReleaseRequest(release);
  state->releases.clear();
  for (ClaimCleanupRequest& cleanup : state->cleanups)
    ClearCleanupRequest(cleanup);
  state->cleanups.clear();
  if (state->priorityCleanup)
  {
    ClearCleanupRequest(*state->priorityCleanup);
    state->priorityCleanup.reset();
  }
  if (state->completion)
  {
    ClearCleanupRequest(state->completion->cleanup);
    state->completion->completion.result.ClearSensitive();
    state->completion.reset();
  }
  state->completionOffered = false;
  state->pendingSessionIds.clear();
  state->transport.reset();
  state->finished = true;
  state->finishedCondition.notify_all();
}

} // namespace KODI::JUMPGATE
