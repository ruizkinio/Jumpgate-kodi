/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateHistoryEventDispatcher.h"

#include <algorithm>
#include <array>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::array<std::chrono::milliseconds, 5> RETRY_DELAYS = {
    std::chrono::milliseconds{0}, std::chrono::milliseconds{100}, std::chrono::milliseconds{250},
    std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}};

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

void ClearRelease(PlaybackReleaseRequest& request)
{
  request.bridgeOrigin.clear();
  std::fill(request.deviceToken.begin(), request.deviceToken.end(), '\0');
  request.deviceToken.clear();
  std::fill(request.sessionId.begin(), request.sessionId.end(), '\0');
  request.sessionId.clear();
  std::fill(request.terminalReceiptId.begin(), request.terminalReceiptId.end(), '\0');
  request.terminalReceiptId.clear();
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

CJumpgateHistoryEventDispatcher::CJumpgateHistoryEventDispatcher(
    std::shared_ptr<IJumpgatePlaybackClaimTransport> transport,
    RetryWait retryWait,
    std::shared_ptr<CJumpgateThreadRegistry> registry)
  : m_registry(registry ? std::move(registry) : CJumpgateThreadRegistry::Global())
{
  if (!transport)
    return;
  if (!retryWait)
  {
    retryWait = [](std::chrono::milliseconds delay)
    {
      if (delay.count() > 0)
        std::this_thread::sleep_for(delay);
    };
  }

  m_registryReservation = m_registry->Reserve();
  if (!m_registryReservation)
    return;
  m_state = std::make_shared<WorkerState>(std::move(transport), std::move(retryWait));
  m_worker = std::thread([state = m_state] { Run(state); });
}

CJumpgateHistoryEventDispatcher::~CJumpgateHistoryEventDispatcher()
{
  Stop(false, std::chrono::milliseconds{0});
}

bool CJumpgateHistoryEventDispatcher::AdvanceGeneration(std::uint64_t generation)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->accepting && state->lifecycle.AdvanceGeneration(generation);
}

void CJumpgateHistoryEventDispatcher::CancelGeneration(std::uint64_t generation)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return;
    state->lifecycle.CancelGeneration(generation);
  }
  NotifyWork(state);
}

bool CJumpgateHistoryEventDispatcher::BindClaim(JumpgateHistoryEventBinding binding,
                                                JumpgateHistorySnapshot snapshot,
                                                std::int64_t nowMs)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
  {
    binding.ClearSensitive();
    return false;
  }

  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->accepting)
      bound = state->lifecycle.BindClaim(std::move(binding), std::move(snapshot), nowMs);
    else
      binding.ClearSensitive();
  }
  NotifyWork(state);
  return bound;
}

void CJumpgateHistoryEventDispatcher::PlaybackStarted(bool resumed,
                                                      JumpgateHistorySnapshot snapshot,
                                                      std::int64_t nowMs)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return;
    state->lifecycle.PlaybackStarted(resumed, std::move(snapshot), nowMs);
  }
  NotifyWork(state);
}

void CJumpgateHistoryEventDispatcher::PlaybackPaused(JumpgateHistorySnapshot snapshot)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return;
    state->lifecycle.PlaybackPaused(std::move(snapshot));
  }
  NotifyWork(state);
}

void CJumpgateHistoryEventDispatcher::SetBackgrounded(bool backgrounded,
                                                      JumpgateHistorySnapshot snapshot)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return;
    state->lifecycle.SetBackgrounded(backgrounded, std::move(snapshot));
  }
  NotifyWork(state);
}

void CJumpgateHistoryEventDispatcher::ProcessSlow(JumpgateHistorySnapshot snapshot,
                                                  std::int64_t nowMs,
                                                  std::int64_t intervalMs)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return;
    state->lifecycle.ProcessSlow(std::move(snapshot), nowMs, intervalMs);
  }
  NotifyWork(state);
}

JumpgateHistoryTerminalResult CJumpgateHistoryEventDispatcher::FinishTerminal(
    std::uint64_t generation, bool completed, JumpgateHistorySnapshot snapshot)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return {JumpgateHistoryTerminalStatus::NotRequired, generation};

  JumpgateHistoryTerminalResult result;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting)
      return {JumpgateHistoryTerminalStatus::Rejected, generation};
    result = state->lifecycle.BeginTerminal(generation, completed, std::move(snapshot));
  }
  NotifyWork(state);
  return result;
}

JumpgateHistoryTerminalResult CJumpgateHistoryEventDispatcher::GetTerminalResult(
    std::uint64_t generation) const
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return {JumpgateHistoryTerminalStatus::NotRequired, generation};
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->lifecycle.GetTerminalResult(generation);
}

bool CJumpgateHistoryEventDispatcher::WaitForIdle(std::chrono::milliseconds timeout)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  std::unique_lock<std::mutex> lock(state->mutex);
  return state->stateChanged.wait_for(
      lock, std::max(timeout, std::chrono::milliseconds{0}),
      [&state] { return (!state->busy && !state->lifecycle.HasPendingWork()) || state->finished; });
}

bool CJumpgateHistoryEventDispatcher::Stop(bool drain, std::chrono::milliseconds timeout)
{
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->accepting = false;
    state->stopping = true;
  }
  state->condition.notify_all();

  bool finished = false;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const std::chrono::milliseconds waitTime =
        drain ? std::max(timeout, std::chrono::milliseconds{0}) : std::chrono::milliseconds{0};
    finished = state->stateChanged.wait_for(lock, waitTime, [&state] { return state->finished; });
  }

  if (finished)
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
                        return state->stateChanged.wait_for(lock, waitTime,
                                                            [&state] { return state->finished; });
                      });
  }
  m_state.reset();
  return finished;
}

bool CJumpgateHistoryEventDispatcher::IsRetryable(const JumpgateHistoryEventResult& result)
{
  return result.status == JumpgateHistoryEventStatus::TransportFailure ||
         result.status == JumpgateHistoryEventStatus::Unavailable ||
         result.status == JumpgateHistoryEventStatus::InvalidResponse ||
         IsRetryableHttpStatus(result.httpStatus);
}

bool CJumpgateHistoryEventDispatcher::IsAmbiguous(const JumpgateHistoryEventResult& result)
{
  return result.status == JumpgateHistoryEventStatus::TransportFailure ||
         result.status == JumpgateHistoryEventStatus::Unavailable ||
         result.status == JumpgateHistoryEventStatus::InvalidResponse ||
         IsRetryableHttpStatus(result.httpStatus);
}

bool CJumpgateHistoryEventDispatcher::IsRetryable(const PlaybackReleaseResult& result)
{
  return result.status == PlaybackReleaseStatus::TransportFailure ||
         result.status == PlaybackReleaseStatus::InvalidResponse ||
         IsRetryableHttpStatus(result.httpStatus);
}

void CJumpgateHistoryEventDispatcher::Run(const std::shared_ptr<WorkerState>& state)
{
  while (true)
  {
    std::optional<CJumpgateHistoryEventState::Work> work;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(lock, [&state]
                            { return state->stopping || state->lifecycle.HasPendingWork(); });
      work = state->lifecycle.TakeNext();
      if (!work)
      {
        state->stateChanged.notify_all();
        if (state->stopping)
          break;
        continue;
      }
      state->busy = true;
    }

    JumpgateHistoryEventResult result;
    std::size_t attempts = 0;
    try
    {
      CJumpgateHistoryEventClient historyClient{*state->transport};
      JumpgatePlaybackHttpRequest prepared;
      if (!historyClient.Prepare(work->event, work->request, prepared))
      {
        result.status = JumpgateHistoryEventStatus::InvalidRequest;
      }
      else
      {
        for (const std::chrono::milliseconds delay : RETRY_DELAYS)
        {
          state->retryWait(delay);
          result = SafeHistorySend(historyClient, work->event, work->request, prepared);
          ++attempts;
          if (result.IsAccepted() || !IsRetryable(result))
            break;
        }
      }

      if (work->terminal && result.IsAccepted())
      {
        PlaybackReleaseRequest release{work->request.bridgeOrigin, work->request.deviceToken,
                                       work->request.sessionId, work->request.idempotencyKey};
        CJumpgatePlaybackClaimClient claimClient{*state->transport};
        PlaybackReleaseResult releaseResult;
        for (const std::chrono::milliseconds delay : RETRY_DELAYS)
        {
          state->retryWait(delay);
          releaseResult = SafeRelease(claimClient, release);
          if (releaseResult.status == PlaybackReleaseStatus::Released ||
              releaseResult.status == PlaybackReleaseStatus::NotFound ||
              !IsRetryable(releaseResult))
          {
            break;
          }
        }
        ClearRelease(release);
      }
    }
    catch (...)
    {
      if (!result.IsAccepted())
      {
        result = {};
        result.status = JumpgateHistoryEventStatus::TransportFailure;
      }
    }

    const bool ambiguous = IsAmbiguous(result);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->lifecycle.Complete(*work, result, attempts, ambiguous);
      work->ClearSensitive();
      state->busy = false;
    }
    state->stateChanged.notify_all();
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->lifecycle.Clear();
    state->transport.reset();
    state->retryWait = {};
    state->busy = false;
    state->finished = true;
  }
  state->stateChanged.notify_all();
}

void CJumpgateHistoryEventDispatcher::NotifyWork(const std::shared_ptr<WorkerState>& state)
{
  state->condition.notify_one();
}

} // namespace KODI::JUMPGATE
