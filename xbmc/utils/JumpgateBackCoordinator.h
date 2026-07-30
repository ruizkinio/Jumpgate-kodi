/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace KODI::JUMPGATE
{

template<typename Target>
class CJumpgateLifecycleTargetRegistry final
{
public:
  using LifecycleToken = uint64_t;
  using PublicationToken = uint64_t;

  PublicationToken Publish(LifecycleToken lifecycleToken, std::shared_ptr<Target> target)
  {
    if (lifecycleToken == 0 || !target)
      return 0;

    std::lock_guard lock(m_mutex);
    if (lifecycleToken < m_highestLifecycleToken ||
        lifecycleToken <= m_retiredThroughLifecycleToken ||
        (lifecycleToken == m_lifecycleToken && m_target) ||
        m_lastPublicationToken == std::numeric_limits<PublicationToken>::max())
    {
      return 0;
    }

    const PublicationToken publicationToken = ++m_lastPublicationToken;
    if (lifecycleToken > m_highestLifecycleToken)
      m_highestLifecycleToken = lifecycleToken;
    m_lifecycleToken = lifecycleToken;
    m_publicationToken = publicationToken;
    m_target = std::move(target);
    return publicationToken;
  }

  std::shared_ptr<Target> Acquire(LifecycleToken lifecycleToken) const
  {
    std::lock_guard lock(m_mutex);
    if (lifecycleToken == 0 || lifecycleToken != m_lifecycleToken)
      return {};
    return m_target;
  }

  bool Retire(LifecycleToken lifecycleToken,
              PublicationToken publicationToken,
              const Target* expectedTarget)
  {
    std::lock_guard lock(m_mutex);
    if (lifecycleToken == 0 || publicationToken == 0 || lifecycleToken != m_lifecycleToken ||
        publicationToken != m_publicationToken || !m_target || m_target.get() != expectedTarget)
    {
      return false;
    }

    m_lifecycleToken = 0;
    m_publicationToken = 0;
    m_target.reset();
    return true;
  }

  bool RetireLifecycle(LifecycleToken lifecycleToken)
  {
    std::lock_guard lock(m_mutex);
    if (lifecycleToken == 0 || lifecycleToken < m_highestLifecycleToken ||
        lifecycleToken <= m_retiredThroughLifecycleToken)
    {
      return false;
    }

    m_highestLifecycleToken = lifecycleToken;
    m_retiredThroughLifecycleToken = lifecycleToken;
    m_lifecycleToken = 0;
    m_publicationToken = 0;
    m_target.reset();
    return true;
  }

private:
  mutable std::mutex m_mutex;
  LifecycleToken m_lifecycleToken{0};
  PublicationToken m_publicationToken{0};
  PublicationToken m_lastPublicationToken{0};
  LifecycleToken m_highestLifecycleToken{0};
  LifecycleToken m_retiredThroughLifecycleToken{0};
  std::shared_ptr<Target> m_target;
};

enum class JumpgateExternalBackDecision
{
  DISMISS_OSD,
  CANCEL_PENDING,
  STOP_PLAYBACK,
};

constexpr JumpgateExternalBackDecision SelectJumpgateExternalBackDecision(bool osdVisible,
                                                                          bool playbackPending)
{
  if (osdVisible)
    return JumpgateExternalBackDecision::DISMISS_OSD;
  return playbackPending ? JumpgateExternalBackDecision::CANCEL_PENDING
                         : JumpgateExternalBackDecision::STOP_PLAYBACK;
}

class CJumpgateBackCoordinator
{
public:
  enum class State
  {
    IDLE,
    PRESSED,
    LONG_CONSUMED,
    COMMIT_PENDING,
    DESTROYED,
  };

  enum class Action
  {
    NONE,
    PASS_THROUGH,
    CONSUME,
    DISPATCH_EXTERNAL_BACK,
    DISPATCH_KODI_BACK_SHORT,
    DISPATCH_KODI_BACK_LONG,
    OPEN_EXTERNAL_SETTINGS,
  };

  enum class Api36Source
  {
    GESTURE_LEFT,
    GESTURE_RIGHT,
    BUTTON,
  };

  static constexpr std::chrono::milliseconds LONG_PRESS_THRESHOLD{700};

  void OnCreated(bool externalMode = false);
  void SetExternalMode(bool externalMode);

  Action OnLegacyRawDown(uint64_t sequenceId, std::chrono::milliseconds heldDuration, bool repeat);
  Action OnLegacyRawUp(uint64_t sequenceId, std::chrono::milliseconds heldDuration, bool cancelled);

  Action OnApi36BackStarted(Api36Source source);
  Action OnApi36BackLongPress();
  Action OnApi36BackCancelled();
  Action OnApi36BackInvoked();
  Action OnApi36RawBack(uint64_t sequenceId,
                        std::chrono::milliseconds heldDuration,
                        bool down,
                        bool repeat,
                        bool cancelled);
  Action OnUnexpectedApi36RawBack();

  Action OnNativeReady();
  Action OnWindowLost();
  Action OnDestroyed();

  void OnActionDelivered(Action action);
  void OnActionFailed(Action action);
  void OnActionRejected(Action action);

  State GetState() const;

private:
  enum class SequenceSource
  {
    NONE,
    LEGACY_RAW,
    API36_GESTURE,
    API36_BUTTON,
  };

  enum class LegacyRoute
  {
    NONE,
    PASS_THROUGH,
    CONSUME,
  };

  enum class Api36ButtonRoute
  {
    NONE,
    STANDALONE,
    EXTERNAL,
  };

  Action CommitShortLocked(bool externalSequence);
  Action CommitApi36ButtonLongPressLocked();
  void PromoteApi36SequenceToButtonLocked();
  void ResetStateLocked();
  void ClearLegacySequenceLocked();

  mutable std::mutex m_mutex;
  State m_state{State::IDLE};
  SequenceSource m_source{SequenceSource::NONE};
  bool m_externalMode{false};
  bool m_nativeReady{false};
  bool m_hasLegacySequence{false};
  bool m_suppressCancelledApi36Invoke{false};
  bool m_api36SequenceExternal{false};
  bool m_hasApi36RawSequence{false};
  uint64_t m_api36RawSequenceId{0};
  bool m_pendingCommitExternal{false};
  bool m_pendingCommitInFlight{false};
  uint64_t m_legacySequenceId{0};
  LegacyRoute m_legacyRoute{LegacyRoute::NONE};
  Api36ButtonRoute m_api36ButtonRoute{Api36ButtonRoute::NONE};
};

class CJumpgateBackDispatcher
{
public:
  using LifecycleToken = uint64_t;
  using PublicationToken = uint64_t;
  static constexpr LifecycleToken INVALID_LIFECYCLE_TOKEN{0};
  static constexpr PublicationToken INVALID_PUBLICATION_TOKEN{0};

  class LifecycleOperation
  {
  public:
    LifecycleOperation() = default;
    ~LifecycleOperation();

    LifecycleOperation(LifecycleOperation&& other) noexcept;
    LifecycleOperation& operator=(LifecycleOperation&& other) noexcept;

    LifecycleOperation(const LifecycleOperation&) = delete;
    LifecycleOperation& operator=(const LifecycleOperation&) = delete;

    explicit operator bool() const
    {
      return m_dispatcher != nullptr && m_lifecycleToken != INVALID_LIFECYCLE_TOKEN;
    }
    LifecycleToken GetLifecycleToken() const { return m_lifecycleToken; }

  private:
    friend class CJumpgateBackDispatcher;

    LifecycleOperation(CJumpgateBackDispatcher& dispatcher, LifecycleToken lifecycleToken);
    void Reset() noexcept;

    CJumpgateBackDispatcher* m_dispatcher{nullptr};
    LifecycleToken m_lifecycleToken{INVALID_LIFECYCLE_TOKEN};
  };

  class CommandContext
  {
  public:
    LifecycleToken GetLifecycleToken() const { return m_lifecycleToken; }
    PublicationToken GetPublicationToken() const { return m_publicationToken; }
    uint64_t GetReadinessGeneration() const { return m_readinessGeneration; }

    template<typename Callback>
    bool Execute(Callback&& callback) const
    {
      if (m_fence == nullptr)
        return false;

      FenceState expected = FenceState::PENDING;
      if (!m_fence->state.compare_exchange_strong(expected, FenceState::EXECUTING,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
      {
        return false;
      }

      if (!BeginExecution())
      {
        m_fence->state.store(FenceState::FAILED, std::memory_order_release);
        Finish(false, false);
        return false;
      }

      try
      {
        bool delivered{true};
        if constexpr (std::is_same_v<std::invoke_result_t<Callback>, bool>)
          delivered = std::forward<Callback>(callback)();
        else
          std::forward<Callback>(callback)();

        EndExecution();
        m_fence->state.store(delivered ? FenceState::DELIVERED : FenceState::FAILED,
                             std::memory_order_release);
        Finish(delivered, false);
        return delivered;
      }
      catch (...)
      {
        EndExecution();
        m_fence->state.store(FenceState::FAILED, std::memory_order_release);
        Finish(false, true);
        throw;
      }
    }

    bool Cancel() const;
    bool Reject() const;
    bool IsPending() const
    {
      return m_fence != nullptr &&
             m_fence->state.load(std::memory_order_acquire) == FenceState::PENDING;
    }

  private:
    friend class CJumpgateBackDispatcher;

    enum class FenceState : uint8_t
    {
      PENDING,
      EXECUTING,
      CANCELLED,
      DELIVERED,
      FAILED,
    };

    struct Fence
    {
      std::atomic<FenceState> state{FenceState::PENDING};
      std::atomic_bool finalized{false};
      std::atomic_bool dispatchReturned{false};
      std::atomic_bool effectReleased{false};
      LifecycleToken lifecycleToken{INVALID_LIFECYCLE_TOKEN};
      PublicationToken publicationToken{INVALID_PUBLICATION_TOKEN};
      uint64_t readinessGeneration{0};
      CJumpgateBackCoordinator::Action action{CJumpgateBackCoordinator::Action::NONE};
    };

    bool BeginExecution() const;
    void EndExecution() const noexcept;
    bool Fail(bool retirePublication) const;
    void Finish(bool delivered,
                bool retirePublication,
                bool terminalRejection = false) const noexcept;

    CJumpgateBackDispatcher* m_dispatcher{nullptr};
    LifecycleToken m_lifecycleToken{INVALID_LIFECYCLE_TOKEN};
    PublicationToken m_publicationToken{INVALID_PUBLICATION_TOKEN};
    uint64_t m_readinessGeneration{0};
    CJumpgateBackCoordinator::Action m_action{CJumpgateBackCoordinator::Action::NONE};
    std::shared_ptr<Fence> m_fence;
  };

  class ISink
  {
  public:
    virtual ~ISink() = default;

    virtual bool DispatchExternalBack(const CommandContext& context) = 0;
    virtual bool DispatchKodiBack(const CommandContext& context, bool longPress) = 0;
    virtual bool OpenExternalSettings(const CommandContext& context) = 0;
  };

  LifecycleToken OnLifecycleStarted(bool initialExternalMode = false);
  PublicationToken PublishSink(LifecycleToken token, std::shared_ptr<ISink> sink);
  bool UnpublishSink(LifecycleToken token, PublicationToken publicationToken);
  bool OnLifecycleDestroyed(LifecycleToken token);

  bool SetExternalMode(LifecycleToken token, bool externalMode);
  bool SetWindowReady(LifecycleToken token, bool ready);

  bool OnLegacyRawDown(LifecycleToken token,
                       uint64_t sequenceId,
                       std::chrono::milliseconds heldDuration,
                       bool repeat);
  bool OnLegacyRawUp(LifecycleToken token,
                     uint64_t sequenceId,
                     std::chrono::milliseconds heldDuration,
                     bool cancelled);

  bool OnApi36BackStarted(LifecycleToken token, int sourceValue);
  bool OnApi36BackLongPress(LifecycleToken token);
  bool OnApi36BackCancelled(LifecycleToken token);
  bool OnApi36BackInvoked(LifecycleToken token);
  bool OnApi36RawBack(LifecycleToken token,
                      uint64_t sequenceId,
                      std::chrono::milliseconds heldDuration,
                      bool down,
                      bool repeat,
                      bool cancelled);
  bool OnUnexpectedApi36RawBack(LifecycleToken token);

  LifecycleOperation TryAcquireLifecycleOperation(LifecycleToken token);
  bool IsCurrentLifecycle(LifecycleToken token) const;
  bool HasPublishedSink(LifecycleToken token) const;
  bool IsWindowReady(LifecycleToken token) const;
  CJumpgateBackCoordinator::State GetState(LifecycleToken token) const;

private:
  struct SinkPublication
  {
    LifecycleToken lifecycleToken{INVALID_LIFECYCLE_TOKEN};
    PublicationToken publicationToken{INVALID_PUBLICATION_TOKEN};
    std::shared_ptr<ISink> sink;
    size_t inFlightEffects{0};
    std::unordered_map<std::thread::id, size_t> executingCommandThreads;
    bool retired{false};
  };

  struct EffectLease
  {
    std::shared_ptr<SinkPublication> publication;
    CJumpgateBackCoordinator::Action action{CJumpgateBackCoordinator::Action::NONE};
    CommandContext context;
  };

  bool IsCurrentLifecycleLocked(LifecycleToken token) const;
  bool IsCurrentOrPendingLifecycleLocked(LifecycleToken token) const;
  bool PrepareActionLocked(LifecycleToken token,
                           CJumpgateBackCoordinator::Action action,
                           EffectLease& lease);
  void ExecuteEffect(const EffectLease& lease);
  void FinalizeCommand(const CommandContext& context,
                       bool delivered,
                       bool retirePublication,
                       bool terminalRejection) noexcept;
  std::shared_ptr<ISink> FinalizeCommandLocked(const CommandContext& context,
                                               bool delivered,
                                               bool retirePublication,
                                               bool terminalRejection);
  void MarkDispatchReturned(const CommandContext& context) noexcept;
  void ReleaseEffect(const std::shared_ptr<SinkPublication>& publication);
  std::shared_ptr<ISink> ReleaseEffectLocked(const std::shared_ptr<SinkPublication>& publication);
  std::shared_ptr<ISink> RetireCurrentSinkLocked();
  std::shared_ptr<ISink> CollectRetiredPublicationLocked(
      const std::shared_ptr<SinkPublication>& publication);
  void ActivatePendingLifecycleLocked();
  bool HasRetiringPublicationEffectsLocked(LifecycleToken token) const;
  bool HasInFlightEffectsLocked(LifecycleToken token) const;
  bool IsExecutingLifecycleOnCurrentThread(LifecycleToken token) const;
  bool IsExecutingPublicationOnCurrentThread(PublicationToken token) const;
  bool BeginCommandExecution(LifecycleToken token,
                             PublicationToken publicationToken,
                             uint64_t readinessGeneration);
  void EndCommandExecution(LifecycleToken token, PublicationToken publicationToken) noexcept;
  void ReleaseLifecycleOperation(LifecycleToken token) noexcept;
  std::shared_ptr<ISink> CancelCommandFenceLocked(
      PublicationToken publicationToken = INVALID_PUBLICATION_TOKEN);
  void RefreshCommandFenceLocked();

  mutable std::mutex m_mutex;
  CJumpgateBackCoordinator m_coordinator;
  LifecycleToken m_lastLifecycleToken{INVALID_LIFECYCLE_TOKEN};
  LifecycleToken m_currentLifecycleToken{INVALID_LIFECYCLE_TOKEN};
  LifecycleToken m_pendingLifecycleToken{INVALID_LIFECYCLE_TOKEN};
  bool m_pendingLifecycleExternalMode{false};
  LifecycleToken m_activationBarrierToken{INVALID_LIFECYCLE_TOKEN};
  PublicationToken m_lastPublicationToken{INVALID_PUBLICATION_TOKEN};
  std::shared_ptr<SinkPublication> m_activePublication;
  bool m_windowReady{false};
  uint64_t m_readinessGeneration{0};
  std::shared_ptr<CommandContext::Fence> m_commandFence;
  std::unordered_map<LifecycleToken, size_t> m_inFlightEffects;
  std::unordered_map<PublicationToken, std::shared_ptr<SinkPublication>> m_publications;
};

} // namespace KODI::JUMPGATE
