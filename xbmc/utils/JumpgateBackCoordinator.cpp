/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateBackCoordinator.h"

using namespace KODI::JUMPGATE;

namespace
{
struct ExecutingBackEffect
{
  const CJumpgateBackDispatcher* dispatcher;
  CJumpgateBackDispatcher::LifecycleToken lifecycleToken;
  CJumpgateBackDispatcher::PublicationToken publicationToken;
  const ExecutingBackEffect* previous;
};

thread_local const ExecutingBackEffect* g_executingBackEffect{nullptr};

class CExecutingBackEffectScope
{
public:
  CExecutingBackEffectScope(const CJumpgateBackDispatcher* dispatcher,
                            CJumpgateBackDispatcher::LifecycleToken lifecycleToken,
                            CJumpgateBackDispatcher::PublicationToken publicationToken)
    : m_effect{dispatcher, lifecycleToken, publicationToken, g_executingBackEffect}
  {
    g_executingBackEffect = &m_effect;
  }

  ~CExecutingBackEffectScope() { g_executingBackEffect = m_effect.previous; }

private:
  ExecutingBackEffect m_effect;
};
} // namespace

void CJumpgateBackCoordinator::OnCreated(bool externalMode)
{
  std::unique_lock lock(m_mutex);
  m_externalMode = externalMode;
  m_nativeReady = false;
  m_suppressCancelledApi36Invoke = false;
  ResetStateLocked();
  ClearLegacySequenceLocked();
}

void CJumpgateBackCoordinator::SetExternalMode(bool externalMode)
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED || m_externalMode == externalMode)
    return;

  m_externalMode = externalMode;
  if (!externalMode)
  {
    // Keep the raw route latch until its matching UP. Kodi must see both halves
    // of a standalone sequence, while an externally consumed DOWN must not leak
    // only its UP after external-mode teardown.
    const bool preserveStandaloneApi36Button = m_state == State::PRESSED &&
                                               m_source == SequenceSource::API36_BUTTON &&
                                               m_api36ButtonRoute == Api36ButtonRoute::STANDALONE;
    if (!preserveStandaloneApi36Button)
    {
      ResetStateLocked();
      m_suppressCancelledApi36Invoke = false;
    }
  }
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnLegacyRawDown(
    uint64_t sequenceId, std::chrono::milliseconds heldDuration, bool repeat)
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED)
    return Action::CONSUME;

  if (m_hasLegacySequence && m_legacySequenceId == sequenceId)
  {
    if (m_legacyRoute == LegacyRoute::PASS_THROUGH)
      return Action::PASS_THROUGH;

    if (m_state == State::COMMIT_PENDING)
      return Action::CONSUME;

    if (m_state == State::PRESSED && m_source == SequenceSource::LEGACY_RAW &&
        heldDuration >= LONG_PRESS_THRESHOLD)
    {
      m_state = State::LONG_CONSUMED;
      return Action::OPEN_EXTERNAL_SETTINGS;
    }

    return Action::CONSUME;
  }

  if (m_hasLegacySequence)
  {
    ClearLegacySequenceLocked();
    if (m_source == SequenceSource::LEGACY_RAW && m_state != State::COMMIT_PENDING)
      ResetStateLocked();
  }

  if (repeat)
    return m_externalMode ? Action::CONSUME : Action::PASS_THROUGH;

  m_hasLegacySequence = true;
  m_legacySequenceId = sequenceId;
  m_legacyRoute = m_externalMode ? LegacyRoute::CONSUME : LegacyRoute::PASS_THROUGH;
  if (m_legacyRoute == LegacyRoute::PASS_THROUGH)
    return Action::PASS_THROUGH;

  if (m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  ResetStateLocked();

  m_state = State::PRESSED;
  m_source = SequenceSource::LEGACY_RAW;
  if (heldDuration >= LONG_PRESS_THRESHOLD)
  {
    m_state = State::LONG_CONSUMED;
    return Action::OPEN_EXTERNAL_SETTINGS;
  }

  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnLegacyRawUp(
    uint64_t sequenceId, std::chrono::milliseconds heldDuration, bool cancelled)
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED)
    return Action::CONSUME;

  if (!m_hasLegacySequence)
    return m_externalMode ? Action::CONSUME : Action::PASS_THROUGH;

  const bool matchingSequence = m_legacySequenceId == sequenceId;
  if (!matchingSequence)
    return Action::CONSUME;

  const LegacyRoute route = m_legacyRoute;
  ClearLegacySequenceLocked();

  if (route == LegacyRoute::PASS_THROUGH)
  {
    if (m_source == SequenceSource::LEGACY_RAW)
      ResetStateLocked();
    return Action::PASS_THROUGH;
  }

  if (m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  const bool activeSequence = (m_state == State::PRESSED || m_state == State::LONG_CONSUMED) &&
                              m_source == SequenceSource::LEGACY_RAW;
  if (!activeSequence || cancelled)
  {
    if (m_source == SequenceSource::LEGACY_RAW)
      ResetStateLocked();
    return Action::CONSUME;
  }

  if (m_state == State::LONG_CONSUMED)
  {
    ResetStateLocked();
    return Action::CONSUME;
  }

  if (heldDuration >= LONG_PRESS_THRESHOLD)
  {
    ResetStateLocked();
    return Action::OPEN_EXTERNAL_SETTINGS;
  }

  return CommitShortLocked(true);
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnApi36BackStarted(Api36Source source)
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED || m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  if (m_state == State::PRESSED && m_source == SequenceSource::API36_BUTTON &&
      m_hasApi36RawSequence)
  {
    return Action::CONSUME;
  }

  ResetStateLocked();
  m_suppressCancelledApi36Invoke = false;
  m_state = State::PRESSED;
  m_source =
      source == Api36Source::BUTTON ? SequenceSource::API36_BUTTON : SequenceSource::API36_GESTURE;
  m_api36SequenceExternal = m_externalMode;
  if (source == Api36Source::BUTTON)
  {
    m_api36ButtonRoute = m_externalMode ? Api36ButtonRoute::EXTERNAL : Api36ButtonRoute::STANDALONE;
  }
  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnApi36BackLongPress()
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED)
    return Action::CONSUME;

  return CommitApi36ButtonLongPressLocked();
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnApi36BackCancelled()
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED)
    return Action::CONSUME;

  if (m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  ResetStateLocked();
  m_suppressCancelledApi36Invoke = true;
  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnApi36BackInvoked()
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED || m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  if (m_suppressCancelledApi36Invoke)
  {
    m_suppressCancelledApi36Invoke = false;
    return Action::CONSUME;
  }

  if (m_state == State::LONG_CONSUMED)
  {
    ResetStateLocked();
    return Action::CONSUME;
  }

  const bool activeApi36Sequence =
      m_state == State::PRESSED &&
      (m_source == SequenceSource::API36_GESTURE || m_source == SequenceSource::API36_BUTTON);
  if (!activeApi36Sequence)
    return Action::CONSUME;

  return CommitShortLocked(m_api36SequenceExternal);
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnUnexpectedApi36RawBack()
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::PRESSED && m_source == SequenceSource::API36_GESTURE)
    PromoteApi36SequenceToButtonLocked();
  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnApi36RawBack(
    uint64_t sequenceId,
    std::chrono::milliseconds heldDuration,
    bool down,
    bool repeat,
    bool cancelled)
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED || m_state == State::COMMIT_PENDING)
    return Action::CONSUME;

  if (cancelled)
  {
    ResetStateLocked();
    m_suppressCancelledApi36Invoke = true;
    return Action::CONSUME;
  }

  if (down)
  {
    if (m_state == State::IDLE)
    {
      m_state = State::PRESSED;
      m_source = SequenceSource::API36_BUTTON;
      m_api36SequenceExternal = m_externalMode;
      m_api36ButtonRoute =
          m_externalMode ? Api36ButtonRoute::EXTERNAL : Api36ButtonRoute::STANDALONE;
    }
    else if (m_state == State::PRESSED && m_source == SequenceSource::API36_GESTURE)
    {
      PromoteApi36SequenceToButtonLocked();
    }

    if (m_source == SequenceSource::API36_BUTTON)
    {
      if (!m_hasApi36RawSequence)
      {
        m_hasApi36RawSequence = true;
        m_api36RawSequenceId = sequenceId;
      }
      if (m_api36RawSequenceId != sequenceId)
        return Action::CONSUME;
      if (repeat || heldDuration >= LONG_PRESS_THRESHOLD)
        return CommitApi36ButtonLongPressLocked();
    }
    return Action::CONSUME;
  }

  if (!m_hasApi36RawSequence || m_api36RawSequenceId != sequenceId) // gitleaks:allow
    return Action::CONSUME;
  if (heldDuration >= LONG_PRESS_THRESHOLD)
    return CommitApi36ButtonLongPressLocked();
  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnNativeReady()
{
  std::unique_lock lock(m_mutex);
  if (m_state == State::DESTROYED)
    return Action::NONE;

  m_nativeReady = true;
  if (m_state != State::COMMIT_PENDING || m_pendingCommitInFlight)
    return Action::NONE;

  m_pendingCommitInFlight = true;
  return m_pendingCommitExternal ? Action::DISPATCH_EXTERNAL_BACK
                                 : Action::DISPATCH_KODI_BACK_SHORT;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnWindowLost()
{
  std::unique_lock lock(m_mutex);
  m_nativeReady = false;
  return Action::NONE;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::OnDestroyed()
{
  std::unique_lock lock(m_mutex);
  m_nativeReady = false;
  ResetStateLocked();
  ClearLegacySequenceLocked();
  m_suppressCancelledApi36Invoke = false;
  m_state = State::DESTROYED;
  return Action::NONE;
}

void CJumpgateBackCoordinator::OnActionDelivered(Action action)
{
  std::unique_lock lock(m_mutex);
  if ((action == Action::DISPATCH_EXTERNAL_BACK || action == Action::DISPATCH_KODI_BACK_SHORT) &&
      m_state == State::COMMIT_PENDING && m_pendingCommitInFlight)
  {
    ResetStateLocked();
  }
}

void CJumpgateBackCoordinator::OnActionFailed(Action action)
{
  std::unique_lock lock(m_mutex);
  if ((action == Action::DISPATCH_EXTERNAL_BACK || action == Action::DISPATCH_KODI_BACK_SHORT) &&
      m_state == State::COMMIT_PENDING)
  {
    m_pendingCommitInFlight = false;
  }
}

void CJumpgateBackCoordinator::OnActionRejected(Action action)
{
  std::unique_lock lock(m_mutex);
  if ((action == Action::DISPATCH_EXTERNAL_BACK || action == Action::DISPATCH_KODI_BACK_SHORT) &&
      m_state == State::COMMIT_PENDING && m_pendingCommitInFlight)
  {
    ResetStateLocked();
  }
}

CJumpgateBackCoordinator::State CJumpgateBackCoordinator::GetState() const
{
  std::unique_lock lock(m_mutex);
  return m_state;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::CommitShortLocked(bool externalSequence)
{
  ClearLegacySequenceLocked();
  ResetStateLocked();
  m_state = State::COMMIT_PENDING;
  m_pendingCommitExternal = externalSequence;
  if (m_nativeReady)
  {
    m_pendingCommitInFlight = true;
    return externalSequence ? Action::DISPATCH_EXTERNAL_BACK : Action::DISPATCH_KODI_BACK_SHORT;
  }

  return Action::CONSUME;
}

CJumpgateBackCoordinator::Action CJumpgateBackCoordinator::CommitApi36ButtonLongPressLocked()
{
  if (m_state != State::PRESSED || m_source != SequenceSource::API36_BUTTON)
    return Action::CONSUME;

  m_state = State::LONG_CONSUMED;
  return m_api36ButtonRoute == Api36ButtonRoute::EXTERNAL ? Action::OPEN_EXTERNAL_SETTINGS
                                                          : Action::DISPATCH_KODI_BACK_LONG;
}

void CJumpgateBackCoordinator::PromoteApi36SequenceToButtonLocked()
{
  m_source = SequenceSource::API36_BUTTON;
  m_api36ButtonRoute =
      m_api36SequenceExternal ? Api36ButtonRoute::EXTERNAL : Api36ButtonRoute::STANDALONE;
}

void CJumpgateBackCoordinator::ResetStateLocked()
{
  m_state = State::IDLE;
  m_source = SequenceSource::NONE;
  m_api36ButtonRoute = Api36ButtonRoute::NONE;
  m_api36SequenceExternal = false;
  m_hasApi36RawSequence = false;
  m_api36RawSequenceId = 0;
  m_pendingCommitExternal = false;
  m_pendingCommitInFlight = false;
}

void CJumpgateBackCoordinator::ClearLegacySequenceLocked()
{
  m_hasLegacySequence = false;
  m_legacySequenceId = 0;
  m_legacyRoute = LegacyRoute::NONE;
}

CJumpgateBackDispatcher::LifecycleOperation::LifecycleOperation(CJumpgateBackDispatcher& dispatcher,
                                                                LifecycleToken lifecycleToken)
  : m_dispatcher(&dispatcher),
    m_lifecycleToken(lifecycleToken)
{
}

CJumpgateBackDispatcher::LifecycleOperation::~LifecycleOperation()
{
  Reset();
}

CJumpgateBackDispatcher::LifecycleOperation::LifecycleOperation(LifecycleOperation&& other) noexcept
  : m_dispatcher(std::exchange(other.m_dispatcher, nullptr)),
    m_lifecycleToken(std::exchange(other.m_lifecycleToken, INVALID_LIFECYCLE_TOKEN))
{
}

CJumpgateBackDispatcher::LifecycleOperation& CJumpgateBackDispatcher::LifecycleOperation::operator=(
    LifecycleOperation&& other) noexcept
{
  if (this != &other)
  {
    Reset();
    m_dispatcher = std::exchange(other.m_dispatcher, nullptr);
    m_lifecycleToken = std::exchange(other.m_lifecycleToken, INVALID_LIFECYCLE_TOKEN);
  }
  return *this;
}

void CJumpgateBackDispatcher::LifecycleOperation::Reset() noexcept
{
  CJumpgateBackDispatcher* dispatcher = std::exchange(m_dispatcher, nullptr);
  const LifecycleToken lifecycleToken = std::exchange(m_lifecycleToken, INVALID_LIFECYCLE_TOKEN);
  if (dispatcher != nullptr && lifecycleToken != INVALID_LIFECYCLE_TOKEN)
    dispatcher->ReleaseLifecycleOperation(lifecycleToken);
}

bool CJumpgateBackDispatcher::CommandContext::BeginExecution() const
{
  return m_dispatcher != nullptr &&
         m_dispatcher->BeginCommandExecution(m_lifecycleToken, m_publicationToken,
                                             m_readinessGeneration);
}

void CJumpgateBackDispatcher::CommandContext::EndExecution() const noexcept
{
  if (m_dispatcher != nullptr)
    m_dispatcher->EndCommandExecution(m_lifecycleToken, m_publicationToken);
}

bool CJumpgateBackDispatcher::CommandContext::Cancel() const
{
  if (m_fence == nullptr)
    return false;

  FenceState expected = FenceState::PENDING;
  if (!m_fence->state.compare_exchange_strong(expected, FenceState::CANCELLED,
                                              std::memory_order_acq_rel, std::memory_order_acquire))
  {
    return false;
  }

  Finish(false, false);
  return true;
}

bool CJumpgateBackDispatcher::CommandContext::Reject() const
{
  if (m_fence == nullptr)
    return false;

  FenceState expected = FenceState::PENDING;
  if (!m_fence->state.compare_exchange_strong(expected, FenceState::FAILED,
                                              std::memory_order_acq_rel, std::memory_order_acquire))
  {
    return false;
  }

  Finish(false, false, true);
  return true;
}

bool CJumpgateBackDispatcher::CommandContext::Fail(bool retirePublication) const
{
  if (m_fence == nullptr)
    return false;

  FenceState expected = FenceState::PENDING;
  if (!m_fence->state.compare_exchange_strong(expected, FenceState::FAILED,
                                              std::memory_order_acq_rel, std::memory_order_acquire))
  {
    return false;
  }

  Finish(false, retirePublication);
  return true;
}

void CJumpgateBackDispatcher::CommandContext::Finish(bool delivered,
                                                     bool retirePublication,
                                                     bool terminalRejection) const noexcept
{
  if (m_dispatcher != nullptr)
    m_dispatcher->FinalizeCommand(*this, delivered, retirePublication, terminalRejection);
}

CJumpgateBackDispatcher::LifecycleToken CJumpgateBackDispatcher::OnLifecycleStarted(
    bool initialExternalMode)
{
  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  const LifecycleToken requestedToken = ++m_lastLifecycleToken;

  if (m_pendingLifecycleToken != INVALID_LIFECYCLE_TOKEN)
  {
    if (m_activePublication != nullptr &&
        m_activePublication->lifecycleToken == m_pendingLifecycleToken)
    {
      retiredSink = RetireCurrentSinkLocked();
    }
    m_pendingLifecycleToken = INVALID_LIFECYCLE_TOKEN;
    m_pendingLifecycleExternalMode = false;
  }

  if (m_currentLifecycleToken != INVALID_LIFECYCLE_TOKEN)
  {
    m_activationBarrierToken = m_currentLifecycleToken;
    retiredSink = RetireCurrentSinkLocked();
    ++m_readinessGeneration;
    m_windowReady = false;
    m_currentLifecycleToken = INVALID_LIFECYCLE_TOKEN;
    m_coordinator.OnDestroyed();
  }

  m_pendingLifecycleToken = requestedToken;
  m_pendingLifecycleExternalMode = initialExternalMode;
  ActivatePendingLifecycleLocked();
  lock.unlock();
  retiredSink.reset();
  return requestedToken;
}

CJumpgateBackDispatcher::PublicationToken CJumpgateBackDispatcher::PublishSink(
    LifecycleToken token, std::shared_ptr<ISink> sink)
{
  if (!sink)
    return INVALID_PUBLICATION_TOKEN;

  EffectLease lease;
  PublicationToken publicationToken{INVALID_PUBLICATION_TOKEN};
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentOrPendingLifecycleLocked(token) ||
        (IsCurrentLifecycleLocked(token) &&
         m_coordinator.GetState() == CJumpgateBackCoordinator::State::DESTROYED) ||
        m_activePublication != nullptr || HasRetiringPublicationEffectsLocked(token))
    {
      return INVALID_PUBLICATION_TOKEN;
    }

    publicationToken = ++m_lastPublicationToken;
    auto publication = std::make_shared<SinkPublication>();
    publication->lifecycleToken = token;
    publication->publicationToken = publicationToken;
    publication->sink = std::move(sink);
    m_publications.emplace(publicationToken, publication);
    m_activePublication = std::move(publication);
    if (IsCurrentLifecycleLocked(token) && m_windowReady)
      PrepareActionLocked(token, m_coordinator.OnNativeReady(), lease);
  }

  ExecuteEffect(lease);
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentOrPendingLifecycleLocked(token) || m_activePublication == nullptr ||
        m_activePublication->publicationToken != publicationToken || m_activePublication->retired)
    {
      return INVALID_PUBLICATION_TOKEN;
    }
  }
  return publicationToken;
}

bool CJumpgateBackDispatcher::UnpublishSink(LifecycleToken token, PublicationToken publicationToken)
{
  if (publicationToken == INVALID_PUBLICATION_TOKEN)
    return false;

  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  const auto publicationIt = m_publications.find(publicationToken);
  if (publicationIt == m_publications.end())
    return false;

  const std::shared_ptr<SinkPublication> publication = publicationIt->second;
  if (publication->lifecycleToken != token)
    return false;

  if (publication->retired)
    return true;

  if (m_activePublication != publication)
    return false;

  publication->retired = true;
  if (m_activePublication == publication)
  {
    m_activePublication.reset();
    retiredSink = CancelCommandFenceLocked(publicationToken);
    ++m_readinessGeneration;
    m_windowReady = false;
    if (IsCurrentLifecycleLocked(token))
      m_coordinator.OnWindowLost();
  }
  if (!retiredSink)
    retiredSink = CollectRetiredPublicationLocked(publication);
  lock.unlock();
  retiredSink.reset();
  return true;
}

bool CJumpgateBackDispatcher::OnLifecycleDestroyed(LifecycleToken token)
{
  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  if (token != INVALID_LIFECYCLE_TOKEN && token == m_pendingLifecycleToken)
  {
    if (m_activePublication != nullptr && m_activePublication->lifecycleToken == token)
      retiredSink = RetireCurrentSinkLocked();
    m_pendingLifecycleToken = INVALID_LIFECYCLE_TOKEN;
    m_pendingLifecycleExternalMode = false;
    ActivatePendingLifecycleLocked();
    lock.unlock();
    retiredSink.reset();
    return true;
  }

  if (token != INVALID_LIFECYCLE_TOKEN && token == m_activationBarrierToken)
    return true;

  if (!IsCurrentLifecycleLocked(token))
    return false;

  m_activationBarrierToken = token;
  retiredSink = RetireCurrentSinkLocked();
  ++m_readinessGeneration;
  m_windowReady = false;
  m_currentLifecycleToken = INVALID_LIFECYCLE_TOKEN;
  m_pendingLifecycleToken = INVALID_LIFECYCLE_TOKEN;
  m_coordinator.OnDestroyed();

  ActivatePendingLifecycleLocked();
  lock.unlock();
  retiredSink.reset();
  return true;
}

bool CJumpgateBackDispatcher::SetExternalMode(LifecycleToken token, bool externalMode)
{
  std::unique_lock lock(m_mutex);
  if (!IsCurrentLifecycleLocked(token))
    return false;

  m_coordinator.SetExternalMode(externalMode);
  return true;
}

bool CJumpgateBackDispatcher::SetWindowReady(LifecycleToken token, bool ready)
{
  EffectLease lease;
  std::shared_ptr<ISink> retiredSink;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return false;

    retiredSink = CancelCommandFenceLocked();
    ++m_readinessGeneration;
    m_windowReady = ready;
    if (!ready)
      m_coordinator.OnWindowLost();
    else if (m_activePublication != nullptr)
      PrepareActionLocked(token, m_coordinator.OnNativeReady(), lease);
  }

  retiredSink.reset();
  ExecuteEffect(lease);
  return true;
}

bool CJumpgateBackDispatcher::OnLegacyRawDown(LifecycleToken token,
                                              uint64_t sequenceId,
                                              std::chrono::milliseconds heldDuration,
                                              bool repeat)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(
        token, m_coordinator.OnLegacyRawDown(sequenceId, heldDuration, repeat), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnLegacyRawUp(LifecycleToken token,
                                            uint64_t sequenceId,
                                            std::chrono::milliseconds heldDuration,
                                            bool cancelled)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(
        token, m_coordinator.OnLegacyRawUp(sequenceId, heldDuration, cancelled), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnApi36BackStarted(LifecycleToken token, int sourceValue)
{
  using Source = CJumpgateBackCoordinator::Api36Source;

  Source source = Source::GESTURE_LEFT;
  if (sourceValue == 1)
    source = Source::GESTURE_RIGHT;
  else if (sourceValue == 2)
    source = Source::BUTTON;

  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(token, m_coordinator.OnApi36BackStarted(source), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnApi36BackLongPress(LifecycleToken token)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(token, m_coordinator.OnApi36BackLongPress(), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnApi36BackCancelled(LifecycleToken token)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(token, m_coordinator.OnApi36BackCancelled(), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnApi36BackInvoked(LifecycleToken token)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(token, m_coordinator.OnApi36BackInvoked(), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnApi36RawBack(LifecycleToken token,
                                             uint64_t sequenceId,
                                             std::chrono::milliseconds heldDuration,
                                             bool down,
                                             bool repeat,
                                             bool cancelled)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(
        token, m_coordinator.OnApi36RawBack(sequenceId, heldDuration, down, repeat, cancelled),
        lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

bool CJumpgateBackDispatcher::OnUnexpectedApi36RawBack(LifecycleToken token)
{
  EffectLease lease;
  bool consumed;
  {
    std::unique_lock lock(m_mutex);
    if (!IsCurrentLifecycleLocked(token))
      return true;

    consumed = PrepareActionLocked(token, m_coordinator.OnUnexpectedApi36RawBack(), lease);
  }

  ExecuteEffect(lease);
  return consumed;
}

CJumpgateBackDispatcher::LifecycleOperation CJumpgateBackDispatcher::TryAcquireLifecycleOperation(
    LifecycleToken token)
{
  std::unique_lock lock(m_mutex);
  if (!IsCurrentLifecycleLocked(token))
    return {};

  ++m_inFlightEffects[token];
  return LifecycleOperation{*this, token};
}

bool CJumpgateBackDispatcher::IsCurrentLifecycle(LifecycleToken token) const
{
  std::unique_lock lock(m_mutex);
  return IsCurrentLifecycleLocked(token);
}

bool CJumpgateBackDispatcher::HasPublishedSink(LifecycleToken token) const
{
  std::unique_lock lock(m_mutex);
  return IsCurrentLifecycleLocked(token) && m_activePublication != nullptr &&
         !m_activePublication->retired;
}

bool CJumpgateBackDispatcher::IsWindowReady(LifecycleToken token) const
{
  std::unique_lock lock(m_mutex);
  return IsCurrentLifecycleLocked(token) && m_windowReady;
}

CJumpgateBackCoordinator::State CJumpgateBackDispatcher::GetState(LifecycleToken token) const
{
  std::unique_lock lock(m_mutex);
  if (!IsCurrentLifecycleLocked(token))
    return CJumpgateBackCoordinator::State::DESTROYED;

  return m_coordinator.GetState();
}

bool CJumpgateBackDispatcher::IsCurrentLifecycleLocked(LifecycleToken token) const
{
  return token != INVALID_LIFECYCLE_TOKEN && token == m_currentLifecycleToken;
}

bool CJumpgateBackDispatcher::IsCurrentOrPendingLifecycleLocked(LifecycleToken token) const
{
  return token != INVALID_LIFECYCLE_TOKEN &&
         (token == m_currentLifecycleToken || token == m_pendingLifecycleToken);
}

bool CJumpgateBackDispatcher::PrepareActionLocked(LifecycleToken token,
                                                  CJumpgateBackCoordinator::Action action,
                                                  EffectLease& lease)
{
  using Action = CJumpgateBackCoordinator::Action;

  switch (action)
  {
    case Action::NONE:
    case Action::PASS_THROUGH:
      return false;
    case Action::CONSUME:
      return true;
    case Action::DISPATCH_EXTERNAL_BACK:
    case Action::DISPATCH_KODI_BACK_SHORT:
    case Action::DISPATCH_KODI_BACK_LONG:
    case Action::OPEN_EXTERNAL_SETTINGS:
    {
      if (IsCurrentLifecycleLocked(token) && m_activePublication != nullptr &&
          !m_activePublication->retired && m_windowReady)
      {
        RefreshCommandFenceLocked();
        ++m_activePublication->inFlightEffects;
        ++m_inFlightEffects[token];
        CommandContext context;
        context.m_dispatcher = this;
        context.m_lifecycleToken = token;
        context.m_publicationToken = m_activePublication->publicationToken;
        context.m_readinessGeneration = m_readinessGeneration;
        context.m_action = action;
        context.m_fence = m_commandFence;
        context.m_fence->lifecycleToken = token;
        context.m_fence->publicationToken = m_activePublication->publicationToken;
        context.m_fence->readinessGeneration = m_readinessGeneration;
        context.m_fence->action = action;
        lease = {m_activePublication, action, std::move(context)};
      }
      else
      {
        m_coordinator.OnActionFailed(action);
      }
      return true;
    }
  }

  return false;
}

void CJumpgateBackDispatcher::ExecuteEffect(const EffectLease& lease)
{
  if (lease.publication == nullptr || lease.publication->sink == nullptr)
    return;

  const std::shared_ptr<ISink> sink = lease.publication->sink;
  bool delivered{false};
  try
  {
    CExecutingBackEffectScope effectScope(this, lease.publication->lifecycleToken,
                                          lease.publication->publicationToken);
    if (lease.action == CJumpgateBackCoordinator::Action::DISPATCH_EXTERNAL_BACK)
      delivered = sink->DispatchExternalBack(lease.context);
    else if (lease.action == CJumpgateBackCoordinator::Action::DISPATCH_KODI_BACK_SHORT)
      delivered = sink->DispatchKodiBack(lease.context, false);
    else if (lease.action == CJumpgateBackCoordinator::Action::DISPATCH_KODI_BACK_LONG)
      delivered = sink->DispatchKodiBack(lease.context, true);
    else if (lease.action == CJumpgateBackCoordinator::Action::OPEN_EXTERNAL_SETTINGS)
      delivered = sink->OpenExternalSettings(lease.context);
  }
  catch (...)
  {
    lease.context.Fail(true);
    MarkDispatchReturned(lease.context);
    throw;
  }

  // Sink acceptance only transfers the owned context to its destination queue.
  // Completion is reported by CommandContext::Execute at actual destination execution.
  if (!delivered && lease.context.IsPending())
    lease.context.Cancel();
  MarkDispatchReturned(lease.context);
}

void CJumpgateBackDispatcher::FinalizeCommand(const CommandContext& context,
                                              bool delivered,
                                              bool retirePublication,
                                              bool terminalRejection) noexcept
{
  if (context.m_fence == nullptr || context.m_fence->finalized.exchange(true))
    return;

  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  retiredSink = FinalizeCommandLocked(context, delivered, retirePublication, terminalRejection);
  lock.unlock();
  retiredSink.reset();
}

std::shared_ptr<CJumpgateBackDispatcher::ISink> CJumpgateBackDispatcher::FinalizeCommandLocked(
    const CommandContext& context, bool delivered, bool retirePublication, bool terminalRejection)
{
  const auto publicationIt = m_publications.find(context.m_publicationToken);
  if (publicationIt == m_publications.end() ||
      publicationIt->second->lifecycleToken != context.m_lifecycleToken)
  {
    if (m_commandFence == context.m_fence)
      m_commandFence.reset();
    ActivatePendingLifecycleLocked();
    return {};
  }

  const std::shared_ptr<SinkPublication> publication = publicationIt->second;
  const bool ownsCurrentLifecycle = IsCurrentLifecycleLocked(context.m_lifecycleToken);
  if (ownsCurrentLifecycle)
  {
    if (delivered)
      m_coordinator.OnActionDelivered(context.m_action);
    else if (terminalRejection)
      m_coordinator.OnActionRejected(context.m_action);
    else
      m_coordinator.OnActionFailed(context.m_action);
  }

  if (m_commandFence == context.m_fence)
    m_commandFence.reset();

  if (retirePublication && !publication->retired)
  {
    publication->retired = true;
    if (m_activePublication == publication)
      m_activePublication.reset();
    if (ownsCurrentLifecycle && m_readinessGeneration == context.m_readinessGeneration)
    {
      ++m_readinessGeneration;
      m_windowReady = false;
      m_coordinator.OnWindowLost();
    }
  }

  if (!context.m_fence->dispatchReturned.load(std::memory_order_acquire) ||
      context.m_fence->effectReleased.exchange(true))
  {
    return {};
  }
  return ReleaseEffectLocked(publication);
}

void CJumpgateBackDispatcher::MarkDispatchReturned(const CommandContext& context) noexcept
{
  if (context.m_fence == nullptr || context.m_fence->dispatchReturned.exchange(true) ||
      !context.m_fence->finalized.load(std::memory_order_acquire) ||
      context.m_fence->effectReleased.exchange(true))
  {
    return;
  }

  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  const auto publication = m_publications.find(context.m_publicationToken);
  if (publication != m_publications.end() &&
      publication->second->lifecycleToken == context.m_lifecycleToken)
  {
    retiredSink = ReleaseEffectLocked(publication->second);
  }
  lock.unlock();
  retiredSink.reset();
}

void CJumpgateBackDispatcher::ReleaseEffect(const std::shared_ptr<SinkPublication>& publication)
{
  std::shared_ptr<ISink> retiredSink;
  std::unique_lock lock(m_mutex);
  retiredSink = ReleaseEffectLocked(publication);
  lock.unlock();
  retiredSink.reset();
}

std::shared_ptr<CJumpgateBackDispatcher::ISink> CJumpgateBackDispatcher::ReleaseEffectLocked(
    const std::shared_ptr<SinkPublication>& publication)
{
  if (publication->inFlightEffects == 0)
    return {};

  --publication->inFlightEffects;
  const auto effects = m_inFlightEffects.find(publication->lifecycleToken);
  if (effects == m_inFlightEffects.end())
    return {};

  if (--effects->second == 0)
    m_inFlightEffects.erase(effects);

  std::shared_ptr<ISink> retiredSink;
  if (publication->retired && publication->inFlightEffects == 0)
    retiredSink = CollectRetiredPublicationLocked(publication);

  ActivatePendingLifecycleLocked();
  return retiredSink;
}

std::shared_ptr<CJumpgateBackDispatcher::ISink> CJumpgateBackDispatcher::RetireCurrentSinkLocked()
{
  if (m_activePublication == nullptr)
    return {};

  const std::shared_ptr<SinkPublication> publication = m_activePublication;
  publication->retired = true;
  m_activePublication.reset();
  std::shared_ptr<ISink> retiredSink = CancelCommandFenceLocked(publication->publicationToken);
  if (!retiredSink)
    retiredSink = CollectRetiredPublicationLocked(publication);
  return retiredSink;
}

std::shared_ptr<CJumpgateBackDispatcher::ISink> CJumpgateBackDispatcher::
    CollectRetiredPublicationLocked(const std::shared_ptr<SinkPublication>& publication)
{
  if (!publication->retired || publication->inFlightEffects != 0)
    return {};

  const auto publicationIt = m_publications.find(publication->publicationToken);
  if (publicationIt != m_publications.end() && publicationIt->second == publication)
    m_publications.erase(publicationIt);
  return std::move(publication->sink);
}

void CJumpgateBackDispatcher::ActivatePendingLifecycleLocked()
{
  if (m_currentLifecycleToken != INVALID_LIFECYCLE_TOKEN)
    return;

  if (m_activationBarrierToken != INVALID_LIFECYCLE_TOKEN &&
      HasInFlightEffectsLocked(m_activationBarrierToken))
  {
    return;
  }

  m_activationBarrierToken = INVALID_LIFECYCLE_TOKEN;
  if (m_pendingLifecycleToken == INVALID_LIFECYCLE_TOKEN)
    return;

  m_currentLifecycleToken = m_pendingLifecycleToken;
  m_pendingLifecycleToken = INVALID_LIFECYCLE_TOKEN;
  CancelCommandFenceLocked();
  ++m_readinessGeneration;
  m_windowReady = false;
  m_coordinator.OnCreated(m_pendingLifecycleExternalMode);
  m_pendingLifecycleExternalMode = false;
}

bool CJumpgateBackDispatcher::HasRetiringPublicationEffectsLocked(LifecycleToken token) const
{
  for (const auto& [publicationToken, publication] : m_publications)
  {
    static_cast<void>(publicationToken);
    if (publication->lifecycleToken == token && publication->retired &&
        publication->inFlightEffects != 0)
    {
      return true;
    }
  }
  return false;
}

bool CJumpgateBackDispatcher::HasInFlightEffectsLocked(LifecycleToken token) const
{
  const auto effects = m_inFlightEffects.find(token);
  return effects != m_inFlightEffects.end() && effects->second != 0;
}

bool CJumpgateBackDispatcher::IsExecutingLifecycleOnCurrentThread(LifecycleToken token) const
{
  if (token == INVALID_LIFECYCLE_TOKEN)
    return false;

  for (const ExecutingBackEffect* effect = g_executingBackEffect; effect != nullptr;
       effect = effect->previous)
  {
    if (effect->dispatcher == this && effect->lifecycleToken == token)
      return true;
  }

  const std::thread::id currentThread = std::this_thread::get_id();
  for (const auto& [publicationToken, publication] : m_publications)
  {
    static_cast<void>(publicationToken);
    if (publication->lifecycleToken != token)
      continue;

    const auto execution = publication->executingCommandThreads.find(currentThread);
    if (execution != publication->executingCommandThreads.end() && execution->second != 0)
      return true;
  }
  return false;
}

bool CJumpgateBackDispatcher::IsExecutingPublicationOnCurrentThread(PublicationToken token) const
{
  if (token == INVALID_PUBLICATION_TOKEN)
    return false;

  for (const ExecutingBackEffect* effect = g_executingBackEffect; effect != nullptr;
       effect = effect->previous)
  {
    if (effect->dispatcher == this && effect->publicationToken == token)
      return true;
  }

  const auto publication = m_publications.find(token);
  if (publication != m_publications.end())
  {
    const auto execution =
        publication->second->executingCommandThreads.find(std::this_thread::get_id());
    if (execution != publication->second->executingCommandThreads.end() && execution->second != 0)
      return true;
  }
  return false;
}

bool CJumpgateBackDispatcher::BeginCommandExecution(LifecycleToken token,
                                                    PublicationToken publicationToken,
                                                    uint64_t readinessGeneration)
{
  std::unique_lock lock(m_mutex);
  const auto publication = m_publications.find(publicationToken);
  if (publication == m_publications.end() || publication->second->lifecycleToken != token ||
      publication->second->inFlightEffects == 0 || publication->second->retired ||
      !IsCurrentLifecycleLocked(token) || m_activePublication != publication->second ||
      !m_windowReady || readinessGeneration != m_readinessGeneration)
  {
    return false;
  }

  ++publication->second->executingCommandThreads[std::this_thread::get_id()];
  return true;
}

void CJumpgateBackDispatcher::EndCommandExecution(LifecycleToken token,
                                                  PublicationToken publicationToken) noexcept
{
  std::unique_lock lock(m_mutex);
  const auto publication = m_publications.find(publicationToken);
  if (publication == m_publications.end() || publication->second->lifecycleToken != token)
    return;

  const auto execution =
      publication->second->executingCommandThreads.find(std::this_thread::get_id());
  if (execution == publication->second->executingCommandThreads.end())
    return;

  if (--execution->second == 0)
    publication->second->executingCommandThreads.erase(execution);
}

void CJumpgateBackDispatcher::ReleaseLifecycleOperation(LifecycleToken token) noexcept
{
  std::unique_lock lock(m_mutex);
  const auto effects = m_inFlightEffects.find(token);
  if (effects == m_inFlightEffects.end() || effects->second == 0)
    return;

  if (--effects->second == 0)
    m_inFlightEffects.erase(effects);
  ActivatePendingLifecycleLocked();
}

std::shared_ptr<CJumpgateBackDispatcher::ISink> CJumpgateBackDispatcher::CancelCommandFenceLocked(
    PublicationToken publicationToken)
{
  if (m_commandFence == nullptr)
    return {};

  const std::shared_ptr<CommandContext::Fence> fence = m_commandFence;
  if (publicationToken != INVALID_PUBLICATION_TOKEN && fence->publicationToken != publicationToken)
    return {};

  CommandContext::FenceState expected = CommandContext::FenceState::PENDING;
  const bool cancelled =
      fence->state.compare_exchange_strong(expected, CommandContext::FenceState::CANCELLED,
                                           std::memory_order_acq_rel, std::memory_order_acquire);
  m_commandFence.reset();
  if (!cancelled || fence->finalized.exchange(true))
    return {};

  CommandContext context;
  context.m_dispatcher = this;
  context.m_lifecycleToken = fence->lifecycleToken;
  context.m_publicationToken = fence->publicationToken;
  context.m_readinessGeneration = fence->readinessGeneration;
  context.m_action = fence->action;
  context.m_fence = fence;
  return FinalizeCommandLocked(context, false, false, false);
}

void CJumpgateBackDispatcher::RefreshCommandFenceLocked()
{
  CancelCommandFenceLocked();
  if (m_windowReady && m_activePublication != nullptr && !m_activePublication->retired)
    m_commandFence = std::make_shared<CommandContext::Fence>();
}
