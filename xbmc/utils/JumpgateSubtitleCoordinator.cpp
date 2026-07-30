/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateSubtitleCoordinator.h"

#include <algorithm>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_DISCARDED_COMPLETIONS = 4;

std::int64_t SystemNowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsRetryable(JumpgateSubtitleResultStatus status)
{
  return status == JumpgateSubtitleResultStatus::RetryableBusy ||
         status == JumpgateSubtitleResultStatus::SoftFailure;
}

std::chrono::milliseconds RetryDelay(const JumpgateSubtitleCoordinatorOptions& options,
                                     JumpgateSubtitleResultStatus status,
                                     std::uint32_t retryAfterSeconds)
{
  if (status == JumpgateSubtitleResultStatus::RetryableBusy)
    return options.retryAfterSecond * retryAfterSeconds;
  return options.softFailureDelay;
}
} // namespace

CJumpgateSubtitleCoordinator::CJumpgateSubtitleCoordinator(
    std::shared_ptr<IJumpgateSubtitleTransport> transport,
    JumpgateSubtitleCoordinatorOptions options,
    std::shared_ptr<CJumpgateThreadRegistry> registry)
  : m_registry(registry ? std::move(registry) : CJumpgateThreadRegistry::Global())
{
  if (!transport || options.maximumAttempts < 1 || options.maximumAttempts > 4 ||
      options.retryAfterSecond < std::chrono::milliseconds{1} ||
      options.retryAfterSecond > std::chrono::seconds{1} ||
      options.softFailureDelay < std::chrono::milliseconds{0} ||
      options.softFailureDelay > std::chrono::seconds{60})
  {
    return;
  }
  m_registryReservation = m_registry->Reserve();
  if (!m_registryReservation)
    return;
  if (!options.nowMilliseconds)
    options.nowMilliseconds = SystemNowMilliseconds;
  m_state = std::make_shared<WorkerState>(std::move(transport), std::move(options));
  m_worker = std::thread([state = m_state] { Run(state); });
}

CJumpgateSubtitleCoordinator::~CJumpgateSubtitleCoordinator()
{
  Stop();
}

bool CJumpgateSubtitleCoordinator::Queue(JumpgateSubtitleRequest request)
{
  if (request.binding.generation == 0 ||
      !CJumpgateSubtitleClient::IsCanonicalRouteIdentifier(request.binding.profileId) ||
      !CJumpgateSubtitleClient::IsCanonicalRouteIdentifier(request.binding.deviceId) ||
      !CJumpgateSubtitleClient::IsCanonicalOrigin(request.binding.bridgeOrigin) ||
      !CJumpgateSubtitleClient::IsCanonicalRouteIdentifier(request.binding.sessionId) ||
      !request.authority.IsValid() || request.languagePreferences.size() > 16 ||
      !std::all_of(request.languagePreferences.begin(), request.languagePreferences.end(),
                   CJumpgateSubtitleClient::IsCanonicalLanguage))
  {
    return false;
  }

  auto cancellation = std::make_shared<CJumpgateSubtitleCancellationSource>();
  std::lock_guard<std::mutex> ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  bool requestSafeCancellation = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping ||
        (state->latestBinding && request.binding.generation <= state->latestBinding->generation))
    {
      return false;
    }
    if (state->completion)
    {
      if (state->discardedCompletions.size() >= MAX_DISCARDED_COMPLETIONS)
        return false;
      state->discardedCompletions.emplace_back(std::move(*state->completion));
      state->completion.reset();
    }
    if (state->activeCancellation)
    {
      state->activeCancellation->Cancel();
      requestSafeCancellation = true;
    }
    if (state->pending)
    {
      state->pending->cancellation->Cancel();
      state->pending.reset();
    }
    state->latestBinding = request.binding;
    state->pending = Job{std::move(request), std::move(cancellation)};
  }
  state->condition.notify_all();
  if (requestSafeCancellation && state->options.requestSafeTransportCancellation)
    state->options.requestSafeTransportCancellation();
  return true;
}

bool CJumpgateSubtitleCoordinator::Cancel(const JumpgateSubtitleBinding& binding)
{
  std::lock_guard<std::mutex> ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;
  bool requestSafeCancellation = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping || !state->latestBinding || !SameBinding(*state->latestBinding, binding))
      return false;
    if (state->completion && SameBinding(state->completion->binding, binding))
    {
      if (state->discardedCompletions.size() < MAX_DISCARDED_COMPLETIONS)
      {
        state->discardedCompletions.emplace_back(std::move(*state->completion));
        state->completion.reset();
      }
    }
    if (state->activeCancellation)
    {
      state->activeCancellation->Cancel();
      requestSafeCancellation = true;
    }
    if (state->pending && SameBinding(state->pending->request.binding, binding))
    {
      state->pending->cancellation->Cancel();
      state->pending.reset();
    }
  }
  state->condition.notify_all();
  if (requestSafeCancellation && state->options.requestSafeTransportCancellation)
    state->options.requestSafeTransportCancellation();
  return true;
}

std::optional<JumpgateSubtitleCompletion> CJumpgateSubtitleCoordinator::TakeCompletion(
    const JumpgateSubtitleBinding& binding)
{
  std::lock_guard<std::mutex> ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return std::nullopt;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || !state->latestBinding || !SameBinding(*state->latestBinding, binding) ||
      !state->completion || !SameBinding(state->completion->binding, binding))
  {
    return std::nullopt;
  }
  std::optional<JumpgateSubtitleCompletion> completion{std::move(*state->completion)};
  state->completion.reset();
  return completion;
}

bool CJumpgateSubtitleCoordinator::ReturnCompletion(JumpgateSubtitleCompletion&& completion)
{
  std::lock_guard<std::mutex> ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return false;

  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stopping || state->completion || !state->latestBinding ||
      !SameBinding(*state->latestBinding, completion.binding))
  {
    return false;
  }
  state->completion = std::move(completion);
  return true;
}

bool CJumpgateSubtitleCoordinator::Stop(std::chrono::milliseconds timeout)
{
  std::lock_guard<std::mutex> ownerLock(m_ownerMutex);
  const std::shared_ptr<WorkerState> state = m_state;
  if (!state)
    return true;
  bool requestSafeCancellation = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->stopping)
    {
      state->stopping = true;
      if (state->completion && state->discardedCompletions.size() < MAX_DISCARDED_COMPLETIONS)
      {
        state->discardedCompletions.emplace_back(std::move(*state->completion));
        state->completion.reset();
      }
      if (state->activeCancellation)
      {
        state->activeCancellation->Cancel();
        requestSafeCancellation = true;
      }
      if (state->pending)
      {
        state->pending->cancellation->Cancel();
        state->pending.reset();
      }
    }
  }
  state->condition.notify_all();
  if (requestSafeCancellation && state->options.requestSafeTransportCancellation)
    state->options.requestSafeTransportCancellation();

  bool finishedBeforeDeadline = false;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    finishedBeforeDeadline =
        state->finishedCondition.wait_for(lock, std::max(timeout, std::chrono::milliseconds{0}),
                                          [&state] { return state->finished; });
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

bool CJumpgateSubtitleCoordinator::SameBinding(const JumpgateSubtitleBinding& left,
                                               const JumpgateSubtitleBinding& right)
{
  return left.generation == right.generation && left.profileId == right.profileId &&
         left.deviceId == right.deviceId && left.bridgeOrigin == right.bridgeOrigin &&
         left.sessionId == right.sessionId;
}

void CJumpgateSubtitleCoordinator::ClearCompletion(const std::shared_ptr<WorkerState>& state,
                                                   JumpgateSubtitleCompletion& completion)
{
  for (JumpgateSubtitleStagedPart& part : completion.artifact.parts)
  {
    volatile std::uint8_t* data = part.bytes.empty() ? nullptr : part.bytes.data();
    for (std::size_t index = 0; index < part.bytes.size(); ++index)
      data[index] = 0;
    part.bytes.clear();
    part.sha256.clear();
  }
  completion.artifact.parts.clear();
  completion.artifact.selected = {};
  completion.artifact.artifactId.clear();
  completion.artifact.expiresAt = 0;
  if (state->options.completionClearObserver)
  {
    try
    {
      state->options.completionClearObserver(completion);
    }
    catch (...)
    {
    }
  }
}

JumpgateSubtitleCompletion CJumpgateSubtitleCoordinator::Execute(
    const std::shared_ptr<WorkerState>& state, Job& job, CJumpgateSubtitleClient& client)
{
  JumpgateSubtitleCompletion completion;
  completion.binding = job.request.binding;
  const CJumpgateSubtitleCancellationToken cancellation = job.cancellation->Token();

  JumpgateSubtitleDiscoverResult discovered;
  for (std::uint32_t attempt = 0; attempt < state->options.maximumAttempts; ++attempt)
  {
    discovered = client.Discover(job.request.binding.bridgeOrigin, job.request.authority,
                                 job.request.binding.sessionId, cancellation);
    if (!IsRetryable(discovered.status) || attempt + 1 == state->options.maximumAttempts)
      break;
    if (!WaitForRetry(state, cancellation,
                      RetryDelay(state->options, discovered.status, discovered.retryAfterSeconds)))
    {
      discovered.status = JumpgateSubtitleResultStatus::Cancelled;
      break;
    }
  }
  completion.status = discovered.status;
  completion.httpStatus = discovered.httpStatus;
  if (discovered.status != JumpgateSubtitleResultStatus::Success)
    return completion;

  const std::optional<JumpgateSubtitleCandidate> selected =
      SelectJumpgateSubtitleCandidate(discovered.candidates, job.request.languagePreferences);
  if (!selected)
  {
    completion.status = JumpgateSubtitleResultStatus::NoMatch;
    return completion;
  }
  completion.artifact.selected = *selected;

  JumpgateSubtitleResolveResult resolved;
  for (std::uint32_t attempt = 0; attempt < state->options.maximumAttempts; ++attempt)
  {
    resolved = client.Resolve(job.request.binding.bridgeOrigin, job.request.authority,
                              job.request.binding.sessionId, selected->selector, cancellation);
    if (!IsRetryable(resolved.status) || attempt + 1 == state->options.maximumAttempts)
      break;
    if (!WaitForRetry(state, cancellation,
                      RetryDelay(state->options, resolved.status, resolved.retryAfterSeconds)))
    {
      resolved.status = JumpgateSubtitleResultStatus::Cancelled;
      break;
    }
  }
  completion.status = resolved.status;
  completion.httpStatus = resolved.httpStatus;
  if (resolved.status != JumpgateSubtitleResultStatus::Success)
  {
    ClearCompletion(state, completion);
    return completion;
  }
  if (resolved.artifact.expiresAt <= 0 ||
      resolved.artifact.expiresAt <= state->options.nowMilliseconds())
  {
    completion.status = JumpgateSubtitleResultStatus::Stale;
    ClearCompletion(state, completion);
    return completion;
  }

  completion.artifact.artifactId = resolved.artifact.artifactId;
  completion.artifact.expiresAt = resolved.artifact.expiresAt;
  completion.artifact.parts.reserve(resolved.artifact.parts.size());
  for (const JumpgateSubtitlePartDescriptor& descriptor : resolved.artifact.parts)
  {
    if (resolved.artifact.expiresAt <= state->options.nowMilliseconds())
    {
      completion.status = JumpgateSubtitleResultStatus::Stale;
      ClearCompletion(state, completion);
      return completion;
    }

    JumpgateSubtitlePartResult downloaded;
    for (std::uint32_t attempt = 0; attempt < state->options.maximumAttempts; ++attempt)
    {
      downloaded = client.Download(job.request.binding.bridgeOrigin, job.request.authority,
                                   descriptor, cancellation);
      if (!IsRetryable(downloaded.status) || attempt + 1 == state->options.maximumAttempts)
        break;
      if (!WaitForRetry(
              state, cancellation,
              RetryDelay(state->options, downloaded.status, downloaded.retryAfterSeconds)))
      {
        downloaded.status = JumpgateSubtitleResultStatus::Cancelled;
        break;
      }
    }
    completion.status = downloaded.status;
    completion.httpStatus = downloaded.httpStatus;
    if (downloaded.status != JumpgateSubtitleResultStatus::Success)
    {
      ClearCompletion(state, completion);
      return completion;
    }
    completion.artifact.parts.emplace_back(std::move(downloaded.part));
  }

  if (resolved.artifact.expiresAt <= state->options.nowMilliseconds())
  {
    completion.status = JumpgateSubtitleResultStatus::Stale;
    ClearCompletion(state, completion);
    return completion;
  }
  completion.status = JumpgateSubtitleResultStatus::Success;
  completion.httpStatus = 200;
  return completion;
}

bool CJumpgateSubtitleCoordinator::WaitForRetry(
    const std::shared_ptr<WorkerState>& state,
    const CJumpgateSubtitleCancellationToken& cancellation,
    std::chrono::milliseconds delay)
{
  std::unique_lock<std::mutex> lock(state->mutex);
  const bool interrupted =
      state->condition.wait_for(lock, delay, [&state, &cancellation]
                                { return state->stopping || cancellation.IsCancelled(); });
  return !interrupted && !cancellation.IsCancelled();
}

void CJumpgateSubtitleCoordinator::Run(const std::shared_ptr<WorkerState>& state)
{
  CJumpgateSubtitleClient client{*state->transport};
  while (true)
  {
    std::optional<Job> job;
    std::optional<JumpgateSubtitleCompletion> discardedCompletion;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(lock,
                            [&state]
                            {
                              return state->stopping || state->pending.has_value() ||
                                     !state->discardedCompletions.empty();
                            });
      if (!state->discardedCompletions.empty())
      {
        discardedCompletion = std::move(state->discardedCompletions.front());
        state->discardedCompletions.pop_front();
      }
      else if (state->stopping && state->completion)
      {
        discardedCompletion = std::move(state->completion);
        state->completion.reset();
      }
      else if (state->stopping && !state->pending)
        break;
      else
      {
        job = std::move(state->pending);
        state->pending.reset();
        state->activeCancellation = job->cancellation;
      }
    }

    if (discardedCompletion)
    {
      ClearCompletion(state, *discardedCompletion);
      continue;
    }

    JumpgateSubtitleCompletion completion = Execute(state, *job, client);
    std::optional<JumpgateSubtitleCompletion> supersededCompletion;
    bool current = false;
    bool published = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      current = !state->stopping && state->latestBinding &&
                SameBinding(*state->latestBinding, job->request.binding) &&
                !job->cancellation->Token().IsCancelled();
      if (state->activeCancellation == job->cancellation)
        state->activeCancellation.reset();
      if (current)
      {
        if (state->completion)
          supersededCompletion = std::move(state->completion);
        state->completion = std::move(completion);
        published = true;
      }
    }
    if (supersededCompletion)
      ClearCompletion(state, *supersededCompletion);
    if (!current)
      ClearCompletion(state, completion);
    if (published && state->options.completionPublishedObserver)
    {
      try
      {
        state->options.completionPublishedObserver(job->request.binding);
      }
      catch (...)
      {
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->activeCancellation.reset();
    state->transport.reset();
    state->finished = true;
  }
  state->finishedCondition.notify_all();
}

} // namespace KODI::JUMPGATE
