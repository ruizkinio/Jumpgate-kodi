/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "input/actions/ActionIDs.h"
#include "input/actions/ActionTranslator.h"
#include "input/keyboard/KeyboardStat.h"
#include "input/keyboard/KeyboardTypes.h"
#include "input/keyboard/XBMC_vkeys.h"
#include "input/keymaps/keyboard/KeyboardTranslator.h"
#include "messaging/OwnedApplicationCallback.h"
#include "messaging/ThreadMessage.h"
#include "utils/JumpgateBackCoordinator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <tinyxml2.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{

using Action = CJumpgateBackCoordinator::Action;
using Api36Source = CJumpgateBackCoordinator::Api36Source;
using State = CJumpgateBackCoordinator::State;
using Dispatcher = CJumpgateBackDispatcher;
using LifecycleToken = Dispatcher::LifecycleToken;
using PublicationToken = Dispatcher::PublicationToken;

constexpr auto TEST_TIMEOUT = 2s;

CKey MakeKodiBackKey(bool longPress)
{
  const uint32_t modifiers = longPress ? CKey::MODIFIER_LONG : 0;
  const unsigned int held = longPress ? KODI::KEYBOARD::KEY_HOLD_TRESHOLD + 1 : 0;
  return CKey(XBMCK_BACKSPACE, XBMCVK_BACK, 0, 0, modifiers, 0, held);
}

class CRecordingBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([this] { ++backPairs; });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool longPress) override
  {
    return context.Execute([this, longPress] { longPress ? ++kodiLongBacks : ++kodiShortBacks; });
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([this] { ++settingsRequests; });
  }

  int backPairs{0};
  int kodiShortBacks{0};
  int kodiLongBacks{0};
  int settingsRequests{0};
};

class CBlockingBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    std::unique_lock lock(m_mutex);
    m_dispatchStarted = true;
    m_condition.notify_all();
    m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_releaseDispatch; });
    lock.unlock();
    return context.Execute([this] { ++backPairs; });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([this] { ++settingsRequests; });
  }

  bool WaitForDispatch()
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_dispatchStarted; });
  }

  void ReleaseDispatch()
  {
    std::unique_lock lock(m_mutex);
    m_releaseDispatch = true;
    m_condition.notify_all();
  }

  std::atomic<int> backPairs{0};
  std::atomic<int> settingsRequests{0};

private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_dispatchStarted{false};
  bool m_releaseDispatch{false};
};

class CExecutingBlockingBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return ExecuteBlocking(context, backPairs);
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return ExecuteBlocking(context, kodiBacks);
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return ExecuteBlocking(context, settingsRequests);
  }

  bool WaitForDispatch()
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_dispatchStarted; });
  }

  void ReleaseDispatch()
  {
    std::unique_lock lock(m_mutex);
    m_releaseDispatch = true;
    m_condition.notify_all();
  }

  std::atomic<int> backPairs{0};
  std::atomic<int> kodiBacks{0};
  std::atomic<int> settingsRequests{0};

private:
  bool ExecuteBlocking(const Dispatcher::CommandContext& context, std::atomic<int>& counter)
  {
    return context.Execute(
        [this, &counter]
        {
          std::unique_lock lock(m_mutex);
          m_dispatchStarted = true;
          m_condition.notify_all();
          m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_releaseDispatch; });
          lock.unlock();
          ++counter;
        });
  }

  std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_dispatchStarted{false};
  bool m_releaseDispatch{false};
};

class CReentrantBackSink final : public Dispatcher::ISink
{
public:
  CReentrantBackSink(Dispatcher& dispatcher, LifecycleToken token)
    : m_dispatcher(dispatcher),
      m_token(token)
  {
  }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          ++backPairs;
          pairReentryAccepted = m_dispatcher.SetWindowReady(m_token, false);
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          ++settingsRequests;
          settingsReentryAccepted = m_dispatcher.SetExternalMode(m_token, false);
        });
  }

  int backPairs{0};
  int settingsRequests{0};
  bool pairReentryAccepted{false};
  bool settingsReentryAccepted{false};

private:
  Dispatcher& m_dispatcher;
  LifecycleToken m_token;
};

class CSynchronousUnpublishBackSink final : public Dispatcher::ISink
{
public:
  CSynchronousUnpublishBackSink(Dispatcher& dispatcher, LifecycleToken token)
    : m_dispatcher(dispatcher),
      m_token(token)
  {
  }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          entered = true;
          unpublishResult = m_dispatcher.UnpublishSink(m_token, publicationToken);
          unpublishReturned = true;
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  std::atomic<bool> entered{false};
  std::atomic<bool> unpublishReturned{false};
  bool unpublishResult{false};
  PublicationToken publicationToken{Dispatcher::INVALID_PUBLICATION_TOKEN};

private:
  Dispatcher& m_dispatcher;
  LifecycleToken m_token;
};

class CSynchronousDestroyBackSink final : public Dispatcher::ISink
{
public:
  CSynchronousDestroyBackSink(Dispatcher& dispatcher, LifecycleToken token)
    : m_dispatcher(dispatcher),
      m_token(token)
  {
  }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          entered = true;
          destroyResult = m_dispatcher.OnLifecycleDestroyed(m_token);
          destroyReturned = true;
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  std::atomic<bool> entered{false};
  std::atomic<bool> destroyReturned{false};
  bool destroyResult{false};

private:
  Dispatcher& m_dispatcher;
  LifecycleToken m_token;
};

class CThrowingBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] { throw std::runtime_error("initial Back effect failed"); });
  }
  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }
};

class CBarrierThrowingBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          std::unique_lock lock(m_mutex);
          ++m_dispatches;
          m_condition.notify_all();
          m_condition.wait(lock, [this] { return m_releaseDispatch; });
          throw std::runtime_error("deferred Back effect failed");
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  bool WaitForDispatches(int expected)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, TEST_TIMEOUT,
                                [this, expected] { return m_dispatches >= expected; });
  }

  void ReleaseDispatch()
  {
    std::unique_lock lock(m_mutex);
    m_releaseDispatch = true;
    m_condition.notify_all();
  }

  int Dispatches() const
  {
    std::unique_lock lock(m_mutex);
    return m_dispatches;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  int m_dispatches{0};
  bool m_releaseDispatch{false};
};

class CKodiKeyboardBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([this] { ++m_pairs; });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool longPress) override
  {
    return context.Execute(
        [this, longPress]
        {
          m_translatedDowns.emplace_back(MakeKodiBackKey(longPress));
          ++m_ups;
        });
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([this] { ++m_settingsRequests; });
  }

  const std::vector<CKey>& TranslatedDowns() const { return m_translatedDowns; }
  int Ups() const { return m_ups; }
  int Pairs() const { return m_pairs; }
  int SettingsRequests() const { return m_settingsRequests; }

private:
  std::vector<CKey> m_translatedDowns;
  int m_ups{0};
  int m_pairs{0};
  int m_settingsRequests{0};
};

class CDelayedKodiKeyboardBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool longPress) override
  {
    return context.Execute([this, longPress] { m_queuedLongPresses.push_back(longPress); });
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  void Drain()
  {
    for (const bool longPress : m_queuedLongPresses)
      m_translatedDowns.emplace_back(MakeKodiBackKey(longPress));
    m_queuedLongPresses.clear();
  }

  const std::vector<CKey>& TranslatedDowns() const { return m_translatedDowns; }

private:
  std::vector<bool> m_queuedLongPresses;
  std::vector<CKey> m_translatedDowns;
};

class CDeferredBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    deferredContext = context;
    return true;
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    deferredContext = context;
    return true;
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    deferredContext = context;
    return true;
  }

  bool Execute()
  {
    return deferredContext && deferredContext->Execute([this] { ++executions; });
  }

  bool Cancel() { return deferredContext && deferredContext->Cancel(); }

  std::optional<Dispatcher::CommandContext> deferredContext;
  int executions{0};
};

class CRejectOnceBackSink final : public Dispatcher::ISink
{
public:
  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    ++dispatchAttempts;
    if (rejectNext)
    {
      rejectNext = false;
      context.Reject();
      return false;
    }
    return context.Execute([this] { ++executions; });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  bool rejectNext{true};
  int dispatchAttempts{0};
  int executions{0};
};

class CCallbackProbe final : public KODI::MESSAGING::IApplicationCallback
{
public:
  void Execute() override { ++executions; }
  void Cancel() noexcept override { ++cancellations; }

  std::atomic<int> executions{0};
  std::atomic<int> cancellations{0};
};

class CBlockingCallbackProbe final : public KODI::MESSAGING::IApplicationCallback
{
public:
  void Execute() override
  {
    std::unique_lock lock(m_mutex);
    ++executions;
    m_started = true;
    m_condition.notify_all();
    m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_released; });
  }

  void Cancel() noexcept override { ++cancellations; }

  bool WaitForExecution()
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_started; });
  }

  void Release()
  {
    std::lock_guard lock(m_mutex);
    m_released = true;
    m_condition.notify_all();
  }

  std::atomic<int> executions{0};
  std::atomic<int> cancellations{0};

private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_started{false};
  bool m_released{false};
};

struct CTargetProbe
{
  explicit CTargetProbe(int value) : id(value) {}
  int id;
};

class CKodiThreadTeardownBackSink final : public Dispatcher::ISink
{
public:
  enum class Teardown
  {
    UNPUBLISH,
    DESTROY_LIFECYCLE,
  };

  CKodiThreadTeardownBackSink(Dispatcher& dispatcher,
                              LifecycleToken lifecycleToken,
                              Teardown teardown)
    : m_dispatcher(dispatcher),
      m_lifecycleToken(lifecycleToken),
      m_teardown(teardown)
  {
  }

  ~CKodiThreadTeardownBackSink() override { Join(); }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    m_destinationThread = std::thread(
        [this, context]
        {
          const bool delivered = context.Execute(
              [this]
              {
                {
                  std::unique_lock lock(m_mutex);
                  m_callbackEntered = true;
                  m_condition.notify_all();
                }

                const bool result =
                    m_teardown == Teardown::UNPUBLISH
                        ? m_dispatcher.UnpublishSink(m_lifecycleToken, publicationToken)
                        : m_dispatcher.OnLifecycleDestroyed(m_lifecycleToken);
                std::unique_lock lock(m_mutex);
                m_teardownResult = result;
                m_teardownReturned = true;
              });

          std::unique_lock lock(m_mutex);
          m_commandDelivered = delivered;
          m_destinationFinished = true;
          m_condition.notify_all();
        });

    std::unique_lock lock(m_mutex);
    if (!m_condition.wait_for(lock, TEST_TIMEOUT, [this] { return m_destinationFinished; }))
    {
      m_dispatchTimedOut = true;
      return false;
    }
    return m_commandDelivered;
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }

  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  void Join()
  {
    if (m_destinationThread.joinable())
      m_destinationThread.join();
  }

  bool CallbackEntered() const
  {
    std::unique_lock lock(m_mutex);
    return m_callbackEntered;
  }

  bool DispatchTimedOut() const
  {
    std::unique_lock lock(m_mutex);
    return m_dispatchTimedOut;
  }

  bool TeardownReturned() const
  {
    std::unique_lock lock(m_mutex);
    return m_teardownReturned;
  }

  bool TeardownResult() const
  {
    std::unique_lock lock(m_mutex);
    return m_teardownResult;
  }

  PublicationToken publicationToken{Dispatcher::INVALID_PUBLICATION_TOKEN};

private:
  Dispatcher& m_dispatcher;
  const LifecycleToken m_lifecycleToken;
  const Teardown m_teardown;
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::thread m_destinationThread;
  bool m_callbackEntered{false};
  bool m_dispatchTimedOut{false};
  bool m_teardownReturned{false};
  bool m_teardownResult{false};
  bool m_commandDelivered{false};
  bool m_destinationFinished{false};
};

struct SelfReleaseState
{
  std::atomic<bool> aliveAfterOwnerReset{false};
  std::atomic<bool> destroyed{false};
};

class CSelfReleasingBackSink final : public Dispatcher::ISink
{
public:
  CSelfReleasingBackSink(Dispatcher& dispatcher,
                         LifecycleToken token,
                         std::shared_ptr<SelfReleaseState> state)
    : m_dispatcher(dispatcher),
      m_token(token),
      m_state(std::move(state))
  {
  }

  ~CSelfReleasingBackSink() override { m_state->destroyed = true; }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          unpublishResult = m_dispatcher.UnpublishSink(m_token, publicationToken);
          m_owner->reset();
          m_state->aliveAfterOwnerReset = !m_state->destroyed.load();
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  std::shared_ptr<CSelfReleasingBackSink>* m_owner{nullptr};
  PublicationToken publicationToken{Dispatcher::INVALID_PUBLICATION_TOKEN};
  bool unpublishResult{false};

private:
  Dispatcher& m_dispatcher;
  LifecycleToken m_token;
  std::shared_ptr<SelfReleaseState> m_state;
};

class CRecreatingBackSink final : public Dispatcher::ISink
{
public:
  CRecreatingBackSink(Dispatcher& dispatcher, std::shared_ptr<CRecordingBackSink> replacement)
    : m_dispatcher(dispatcher),
      m_replacement(replacement)
  {
  }

  bool DispatchExternalBack(const Dispatcher::CommandContext& context) override
  {
    return context.Execute(
        [this]
        {
          newToken = m_dispatcher.OnLifecycleStarted();
          newLifecycleWasCurrentInsideEffect = m_dispatcher.IsCurrentLifecycle(newToken);
          replacementPublishedInsideEffect = m_dispatcher.PublishSink(newToken, m_replacement) !=
                                             Dispatcher::INVALID_PUBLICATION_TOKEN;
        });
  }

  bool DispatchKodiBack(const Dispatcher::CommandContext& context, bool) override
  {
    return context.Execute([] {});
  }
  bool OpenExternalSettings(const Dispatcher::CommandContext& context) override
  {
    return context.Execute([] {});
  }

  LifecycleToken newToken{Dispatcher::INVALID_LIFECYCLE_TOKEN};
  bool newLifecycleWasCurrentInsideEffect{false};
  bool replacementPublishedInsideEffect{false};

private:
  Dispatcher& m_dispatcher;
  std::shared_ptr<CRecordingBackSink> m_replacement;
};

class CLifecycleBoundBackProducer
{
public:
  CLifecycleBoundBackProducer(Dispatcher& dispatcher, LifecycleToken token)
    : m_dispatcher(dispatcher),
      m_token(token)
  {
  }

  bool SetExternalMode(bool externalMode)
  {
    return m_dispatcher.SetExternalMode(m_token, externalMode);
  }

  bool SetWindowReady(bool ready) { return m_dispatcher.SetWindowReady(m_token, ready); }

  bool OnLegacyRawDown(uint64_t sequenceId)
  {
    return m_dispatcher.OnLegacyRawDown(m_token, sequenceId, 0ms, false);
  }

  bool OnLegacyRawUp(uint64_t sequenceId)
  {
    return m_dispatcher.OnLegacyRawUp(m_token, sequenceId, 100ms, false);
  }

private:
  Dispatcher& m_dispatcher;
  const LifecycleToken m_token;
};

template<typename Predicate>
bool WaitUntil(Predicate predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
  while (!predicate())
  {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}

void MakeReady(CJumpgateBackCoordinator& coordinator)
{
  ASSERT_EQ(coordinator.OnNativeReady(), Action::NONE);
}

} // namespace

TEST(TestJumpgateBackCoordinator, StandaloneLegacyRawSequencePassesThroughUnchanged)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(false);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(10, 0ms, false), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawDown(10, 700ms, true), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(10, 750ms, false), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator,
     StandaloneLegacySequenceKeepsPassingThroughAfterModeEntersExternal)
{
  CJumpgateBackCoordinator coordinator;
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(11, 0ms, false), Action::PASS_THROUGH);
  coordinator.SetExternalMode(true);
  EXPECT_EQ(coordinator.OnLegacyRawDown(11, 800ms, true), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(11, 900ms, false), Action::PASS_THROUGH);

  EXPECT_EQ(coordinator.OnLegacyRawDown(12, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(12, 100ms, false), Action::DISPATCH_EXTERNAL_BACK);
}

TEST(TestJumpgateBackCoordinator, ExternalLegacySequenceKeepsConsumingAfterModeReturnsStandalone)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(13, 0ms, false), Action::CONSUME);
  coordinator.SetExternalMode(false);
  EXPECT_EQ(coordinator.OnLegacyRawDown(13, 800ms, true), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(13, 900ms, false), Action::CONSUME);

  EXPECT_EQ(coordinator.OnLegacyRawDown(14, 0ms, false), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(14, 100ms, false), Action::PASS_THROUGH);
}

TEST(TestJumpgateBackCoordinator,
     MismatchedStandaloneUpIsConsumedWithoutClearingPassThroughRouteAcrossModeFlip)
{
  CJumpgateBackCoordinator coordinator;
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(15, 0ms, false), Action::PASS_THROUGH);
  coordinator.SetExternalMode(true);
  EXPECT_EQ(coordinator.OnLegacyRawUp(999, 50ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawDown(15, 60ms, true), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(15, 100ms, false), Action::PASS_THROUGH);
}

TEST(TestJumpgateBackCoordinator,
     MismatchedExternalUpIsConsumedWithoutClearingConsumedRouteAcrossModeFlip)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(16, 0ms, false), Action::CONSUME);
  coordinator.SetExternalMode(false);
  EXPECT_EQ(coordinator.OnLegacyRawUp(998, 50ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawDown(16, 60ms, true), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(16, 100ms, false), Action::CONSUME);

  EXPECT_EQ(coordinator.OnLegacyRawDown(17, 0ms, false), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(17, 100ms, false), Action::PASS_THROUGH);
}

TEST(TestJumpgateBackCoordinator, ExternalLegacyShortReleaseDispatchesExactlyOneCommand)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(20, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::PRESSED);
  EXPECT_EQ(coordinator.OnLegacyRawDown(20, 699ms, true), Action::CONSUME);
  const Action command = coordinator.OnLegacyRawUp(20, 699ms, false);
  EXPECT_EQ(command, Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  coordinator.OnActionDelivered(command);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
  EXPECT_EQ(coordinator.OnLegacyRawUp(20, 699ms, false), Action::CONSUME);
}

TEST(TestJumpgateBackCoordinator, ExternalShortBackNeverUsesFullscreenKodiKeyPair)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(21, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(21, 100ms, false), Action::DISPATCH_EXTERNAL_BACK);
}

TEST(TestJumpgateBackCoordinator, ExternalLegacyRepeatConsumesLongPressOnce)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(30, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawDown(30, 700ms, true), Action::OPEN_EXTERNAL_SETTINGS);
  EXPECT_EQ(coordinator.GetState(), State::LONG_CONSUMED);
  EXPECT_EQ(coordinator.OnLegacyRawDown(30, 900ms, true), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(30, 950ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, ExternalLegacyLongReleaseNeverDispatchesShortBack)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(31, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(31, 700ms, false), Action::OPEN_EXTERNAL_SETTINGS);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
}

TEST(TestJumpgateBackCoordinator, CancelledMatchingLegacyUpCannotCommit)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(42, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(42, 100ms, true), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
}

TEST(TestJumpgateBackCoordinator, LegacyGestureGeneratedPairUsesTheSameCommitPath)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnLegacyRawDown(0, 0ms, false), Action::CONSUME);
  const Action command = coordinator.OnLegacyRawUp(0, 1ms, false);
  EXPECT_EQ(command, Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  coordinator.OnActionDelivered(command);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, EarlyCommitIsStoredOnceAndFlushedOnceWhenReady)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);

  EXPECT_EQ(coordinator.OnLegacyRawDown(50, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(50, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);

  EXPECT_EQ(coordinator.OnLegacyRawDown(51, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(51, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);

  const Action pendingCommit = coordinator.OnNativeReady();
  EXPECT_EQ(pendingCommit, Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  coordinator.OnActionDelivered(pendingCommit);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
}

TEST(TestJumpgateBackCoordinator, TransientWindowLossPreservesPendingCommitAndRawRoute)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);

  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::GESTURE_LEFT), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackCancelled(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.OnWindowLost(), Action::NONE);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  EXPECT_EQ(coordinator.OnWindowLost(), Action::NONE);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  const Action api36PendingCommit = coordinator.OnNativeReady();
  EXPECT_EQ(api36PendingCommit, Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
  coordinator.OnActionDelivered(api36PendingCommit);

  EXPECT_EQ(coordinator.OnLegacyRawDown(52, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnWindowLost(), Action::NONE);
  EXPECT_EQ(coordinator.OnLegacyRawUp(999, 50ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(52, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  const Action rawPendingCommit = coordinator.OnNativeReady();
  EXPECT_EQ(rawPendingCommit, Action::DISPATCH_EXTERNAL_BACK);
  coordinator.OnActionDelivered(rawPendingCommit);
}

TEST(TestJumpgateBackDispatcher, LifecycleTokensAreMonotonicAndStaleActivityCannotAffectNewOwner)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CRecordingBackSink>();
  auto newSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  ASSERT_NE(oldToken, Dispatcher::INVALID_LIFECYCLE_TOKEN);
  const PublicationToken oldPublication = dispatcher.PublishSink(oldToken, oldSink);
  ASSERT_NE(oldPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));

  const LifecycleToken newToken = dispatcher.OnLifecycleStarted();
  ASSERT_GT(newToken, oldToken);
  ASSERT_TRUE(dispatcher.SetExternalMode(newToken, true));
  EXPECT_TRUE(dispatcher.OnApi36BackStarted(newToken, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(newToken));
  EXPECT_EQ(dispatcher.GetState(newToken), State::COMMIT_PENDING);

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(oldToken));
  EXPECT_TRUE(dispatcher.OnApi36BackCancelled(oldToken));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(oldToken));
  EXPECT_TRUE(dispatcher.OnUnexpectedApi36RawBack(oldToken));
  EXPECT_FALSE(dispatcher.SetExternalMode(oldToken, false));
  EXPECT_FALSE(dispatcher.SetWindowReady(oldToken, false));
  EXPECT_FALSE(dispatcher.OnLifecycleDestroyed(oldToken));
  EXPECT_FALSE(dispatcher.UnpublishSink(oldToken, oldPublication));
  EXPECT_EQ(dispatcher.GetState(newToken), State::COMMIT_PENDING);

  ASSERT_NE(dispatcher.PublishSink(newToken, newSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(newToken, true));
  EXPECT_EQ(oldSink->backPairs, 0);
  EXPECT_EQ(newSink->backPairs, 1);
  EXPECT_EQ(dispatcher.GetState(newToken), State::IDLE);
}

TEST(TestJumpgateBackDispatcher,
     EarlyCommitSurvivesDuplicateWindowLossAndFlushesOnceAfterPublicationAndReadiness)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, false));
  EXPECT_TRUE(dispatcher.SetWindowReady(token, false));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  EXPECT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(sink->backPairs, 0);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(sink->backPairs, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(sink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, PublicationFailsIfInitialPendingEffectDestroysLifecycle)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CSynchronousDestroyBackSink>(dispatcher, token);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  ASSERT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  EXPECT_EQ(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(sink->destroyReturned);
  EXPECT_TRUE(sink->destroyResult);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(token));
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));

  auto replacementSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(replacementToken, replacementSink),
            Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(replacementToken, true));
  EXPECT_EQ(replacementSink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, PublicationExceptionRetiresFailedInitialSink)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto throwingSink = std::make_shared<CThrowingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  ASSERT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  EXPECT_THROW(dispatcher.PublishSink(token, throwingSink), std::runtime_error);
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
  EXPECT_NE(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(replacementSink->backPairs, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(replacementSink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, DeferredWindowReadyExceptionRetiresFailedSinkAndRetriesExactlyOnce)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto throwingSink = std::make_shared<CThrowingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, throwingSink), Dispatcher::INVALID_PUBLICATION_TOKEN);

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  ASSERT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  EXPECT_THROW(dispatcher.SetWindowReady(token, true), std::runtime_error);
  ASSERT_FALSE(dispatcher.HasPublishedSink(token));
  EXPECT_FALSE(dispatcher.IsWindowReady(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  ASSERT_NE(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(replacementSink->backPairs, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(replacementSink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher,
     ConcurrentReadinessQueuedDuringFailureCannotReadmitFailedPublication)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto throwingSink = std::make_shared<CBarrierThrowingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, throwingSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));

  std::promise<void> failureObserved;
  auto failureReady = failureObserved.get_future().share();
  std::atomic<bool> firstThrew{false};
  std::atomic<bool> duplicateThrew{false};
  std::thread firstReady(
      [&]
      {
        try
        {
          dispatcher.SetWindowReady(token, true);
        }
        catch (const std::runtime_error&)
        {
          firstThrew = true;
        }
        failureObserved.set_value();
      });
  ASSERT_TRUE(throwingSink->WaitForDispatches(1));

  std::thread duplicateReady(
      [&]
      {
        failureReady.wait();
        try
        {
          dispatcher.SetWindowReady(token, true);
        }
        catch (const std::runtime_error&)
        {
          duplicateThrew = true;
        }
      });

  throwingSink->ReleaseDispatch();
  firstReady.join();
  duplicateReady.join();

  EXPECT_TRUE(firstThrew);
  EXPECT_FALSE(duplicateThrew);
  EXPECT_EQ(throwingSink->Dispatches(), 1);
  ASSERT_FALSE(dispatcher.HasPublishedSink(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);

  ASSERT_NE(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(replacementSink->backPairs, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
}

TEST(TestJumpgateBackDispatcher,
     NewerOverlappingReadinessSurvivesOlderEffectFailureAndFlushesReplacement)
{
  Dispatcher dispatcher;
  auto failingSink = std::make_shared<CBarrierThrowingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, failingSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));

  ASSERT_TRUE(dispatcher.OnLegacyRawDown(token, 701, 0ms, false));
  ASSERT_TRUE(dispatcher.OnLegacyRawUp(token, 701, 100ms, false));

  auto failingReadiness = std::async(std::launch::async,
                                     [&]
                                     {
                                       try
                                       {
                                         dispatcher.SetWindowReady(token, true);
                                       }
                                       catch (const std::runtime_error&)
                                       {
                                         return true;
                                       }
                                       return false;
                                     });
  ASSERT_TRUE(failingSink->WaitForDispatches(1));

  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_TRUE(dispatcher.IsWindowReady(token));
  failingSink->ReleaseDispatch();
  ASSERT_EQ(failingReadiness.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(failingReadiness.get());
  EXPECT_TRUE(dispatcher.IsWindowReady(token));

  auto replacement = std::make_shared<CRecordingBackSink>();
  ASSERT_NE(dispatcher.PublishSink(token, replacement), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(replacement->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, LifecycleReplacementCancelsFailingDeferredPublicationRetry)
{
  Dispatcher dispatcher;
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  auto throwingSink = std::make_shared<CBarrierThrowingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  ASSERT_TRUE(dispatcher.SetExternalMode(oldToken, true));
  ASSERT_NE(dispatcher.PublishSink(oldToken, throwingSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 0));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(oldToken));

  std::atomic<bool> firstThrew{false};
  std::thread readyThread(
      [&]
      {
        try
        {
          dispatcher.SetWindowReady(oldToken, true);
        }
        catch (const std::runtime_error&)
        {
          firstThrew = true;
        }
      });
  ASSERT_TRUE(throwingSink->WaitForDispatches(1));

  std::atomic<bool> lifecycleStarted{false};
  LifecycleToken newToken{Dispatcher::INVALID_LIFECYCLE_TOKEN};
  std::thread lifecycleThread(
      [&]
      {
        newToken = dispatcher.OnLifecycleStarted();
        lifecycleStarted = true;
      });
  ASSERT_TRUE(WaitUntil([&] { return !dispatcher.IsCurrentLifecycle(oldToken); }));
  ASSERT_TRUE(WaitUntil([&] { return lifecycleStarted.load(); }));
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(newToken));

  throwingSink->ReleaseDispatch();
  readyThread.join();
  lifecycleThread.join();

  EXPECT_TRUE(firstThrew);
  ASSERT_TRUE(lifecycleStarted);
  ASSERT_GT(newToken, oldToken);
  ASSERT_TRUE(dispatcher.IsCurrentLifecycle(newToken));
  ASSERT_NE(dispatcher.PublishSink(newToken, replacementSink),
            Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(newToken, true));
  EXPECT_EQ(replacementSink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, SameLifecycleCanReplaceUnpublishedSinkAndFlushPendingCommit)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CRecordingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  const PublicationToken oldPublication = dispatcher.PublishSink(token, oldSink);
  ASSERT_NE(oldPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, false));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
  EXPECT_TRUE(dispatcher.UnpublishSink(token, oldPublication));
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));

  EXPECT_NE(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(oldSink->backPairs, 0);
  EXPECT_EQ(replacementSink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, PublicationRejectsConcurrentSinkAndAllowsExactReplacement)
{
  Dispatcher dispatcher;
  auto firstSink = std::make_shared<CRecordingBackSink>();
  auto secondSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();

  const PublicationToken firstPublication = dispatcher.PublishSink(token, firstSink);
  ASSERT_NE(firstPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(dispatcher.PublishSink(token, firstSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(dispatcher.PublishSink(token, secondSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(dispatcher.HasPublishedSink(token));

  EXPECT_TRUE(dispatcher.UnpublishSink(token, firstPublication));
  const PublicationToken secondPublication = dispatcher.PublishSink(token, secondSink);
  ASSERT_NE(secondPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_FALSE(dispatcher.UnpublishSink(token, firstPublication));
  EXPECT_TRUE(dispatcher.HasPublishedSink(token));

  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted();
  EXPECT_GT(replacementToken, token);
  EXPECT_FALSE(dispatcher.UnpublishSink(token, secondPublication));
  EXPECT_EQ(dispatcher.PublishSink(token, firstSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_NE(dispatcher.PublishSink(replacementToken, firstSink),
            Dispatcher::INVALID_PUBLICATION_TOKEN);
}

TEST(TestJumpgateBackDispatcher, SameAddressPlacementNewSinkGetsFreshPublicationIdentity)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  alignas(CRecordingBackSink) unsigned char storage[sizeof(CRecordingBackSink)];

  auto* firstSink = new (storage) CRecordingBackSink;
  auto firstOwner = std::shared_ptr<Dispatcher::ISink>(firstSink, [](Dispatcher::ISink*) {});
  const PublicationToken firstPublication = dispatcher.PublishSink(token, firstOwner);
  ASSERT_NE(firstPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(dispatcher.UnpublishSink(token, firstPublication));
  firstOwner.reset();
  firstSink->~CRecordingBackSink();

  auto* replacementSink = new (storage) CRecordingBackSink;
  auto replacementOwner =
      std::shared_ptr<Dispatcher::ISink>(replacementSink, [](Dispatcher::ISink*) {});
  const PublicationToken replacementPublication = dispatcher.PublishSink(token, replacementOwner);
  EXPECT_NE(replacementPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  if (dispatcher.HasPublishedSink(token))
  {
    EXPECT_TRUE(dispatcher.UnpublishSink(token, replacementPublication));
  }
  replacementOwner.reset();
  replacementSink->~CRecordingBackSink();
}

TEST(TestJumpgateBackDispatcher, JniSourceAdapterKeepsGesturesShortAndButtonLongPressExclusive)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  for (const int gestureSource : {0, 1})
  {
    EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, gestureSource));
    EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
    EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  }
  EXPECT_EQ(sink->backPairs, 2);
  EXPECT_EQ(sink->settingsRequests, 0);

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->backPairs, 2);
  EXPECT_EQ(sink->settingsRequests, 1);
}

TEST(TestJumpgateBackDispatcher, SinkEffectsCanReenterWindowAndExternalModeWithoutDeadlock)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CReentrantBackSink>(dispatcher, token);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->backPairs, 1);
  EXPECT_TRUE(sink->pairReentryAccepted);
  EXPECT_FALSE(dispatcher.IsWindowReady(token));

  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->settingsRequests, 1);
  EXPECT_TRUE(sink->settingsReentryAccepted);
  EXPECT_EQ(sink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, SynchronousUnpublishInsideEffectNeverWaitsForItself)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CSynchronousUnpublishBackSink>(dispatcher, token);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  sink->publicationToken = dispatcher.PublishSink(token, sink);
  ASSERT_NE(sink->publicationToken, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_TRUE(sink->entered);
  EXPECT_TRUE(sink->unpublishReturned);
  EXPECT_TRUE(sink->unpublishResult);
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));
}

TEST(TestJumpgateBackDispatcher, SynchronousLifecycleDestroyInsideEffectNeverWaitsForItself)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CSynchronousDestroyBackSink>(dispatcher, token);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_TRUE(sink->entered);
  EXPECT_TRUE(sink->destroyReturned);
  EXPECT_TRUE(sink->destroyResult);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(token));
}

TEST(TestJumpgateBackDispatcher, KodiThreadUnpublishInsideSynchronousCallbackNeverSelfWaits)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CKodiThreadTeardownBackSink>(
      dispatcher, token, CKodiThreadTeardownBackSink::Teardown::UNPUBLISH);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  sink->publicationToken = dispatcher.PublishSink(token, sink);
  ASSERT_NE(sink->publicationToken, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  sink->Join();

  EXPECT_TRUE(sink->CallbackEntered());
  EXPECT_FALSE(sink->DispatchTimedOut());
  EXPECT_TRUE(sink->TeardownReturned());
  EXPECT_TRUE(sink->TeardownResult());
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));
}

TEST(TestJumpgateBackDispatcher, KodiThreadLifecycleDestroyInsideSynchronousCallbackNeverSelfWaits)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto sink = std::make_shared<CKodiThreadTeardownBackSink>(
      dispatcher, token, CKodiThreadTeardownBackSink::Teardown::DESTROY_LIFECYCLE);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  sink->publicationToken = dispatcher.PublishSink(token, sink);
  ASSERT_NE(sink->publicationToken, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  sink->Join();

  EXPECT_TRUE(sink->CallbackEntered());
  EXPECT_FALSE(sink->DispatchTimedOut());
  EXPECT_TRUE(sink->TeardownReturned());
  EXPECT_TRUE(sink->TeardownResult());
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(token));
}

TEST(TestJumpgateBackDispatcher, SelfUnpublishPinsOwnedSinkUntilCallbackReturns)
{
  Dispatcher dispatcher;
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  auto state = std::make_shared<SelfReleaseState>();
  auto sink = std::make_shared<CSelfReleasingBackSink>(dispatcher, token, state);
  sink->m_owner = &sink;
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  sink->publicationToken = dispatcher.PublishSink(token, sink);
  ASSERT_NE(sink->publicationToken, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink, nullptr);
  EXPECT_TRUE(state->aliveAfterOwnerReset);
  EXPECT_TRUE(state->destroyed);
  EXPECT_FALSE(dispatcher.HasPublishedSink(token));
}

TEST(TestJumpgateBackDispatcher, LifecycleStartInsideEffectDefersActivationAndPublication)
{
  Dispatcher dispatcher;
  auto replacement = std::make_shared<CRecordingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  auto oldSink = std::make_shared<CRecreatingBackSink>(dispatcher, replacement);
  ASSERT_TRUE(dispatcher.SetExternalMode(oldToken, true));
  ASSERT_NE(dispatcher.PublishSink(oldToken, oldSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(oldToken));
  ASSERT_GT(oldSink->newToken, oldToken);
  EXPECT_FALSE(oldSink->newLifecycleWasCurrentInsideEffect);
  EXPECT_TRUE(oldSink->replacementPublishedInsideEffect);

  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(oldSink->newToken));
  EXPECT_TRUE(dispatcher.HasPublishedSink(oldSink->newToken));
}

TEST(TestJumpgateBackDispatcher, ActivityRecreationWaitsForAdmittedOldEffectBeforeActivation)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CBlockingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  ASSERT_TRUE(dispatcher.SetExternalMode(oldToken, true));
  ASSERT_NE(dispatcher.PublishSink(oldToken, oldSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 0));

  std::thread invokeThread([&] { dispatcher.OnApi36BackInvoked(oldToken); });
  ASSERT_TRUE(oldSink->WaitForDispatch());

  std::atomic<bool> lifecycleStartReturned{false};
  LifecycleToken newToken{Dispatcher::INVALID_LIFECYCLE_TOKEN};
  std::thread lifecycleStart(
      [&]
      {
        newToken = dispatcher.OnLifecycleStarted();
        lifecycleStartReturned = true;
      });
  ASSERT_TRUE(WaitUntil([&] { return !dispatcher.IsCurrentLifecycle(oldToken); }));
  ASSERT_TRUE(WaitUntil([&] { return lifecycleStartReturned.load(); }));
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(newToken));

  oldSink->ReleaseDispatch();
  invokeThread.join();
  lifecycleStart.join();
  EXPECT_TRUE(lifecycleStartReturned);
  EXPECT_GT(newToken, oldToken);
  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(newToken));
}

TEST(TestJumpgateBackDispatcher, StaleActivityProducerKeepsOriginalRawAndWindowToken)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CRecordingBackSink>();
  auto newSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  CLifecycleBoundBackProducer oldProducer(dispatcher, oldToken);
  ASSERT_TRUE(oldProducer.SetExternalMode(true));
  ASSERT_NE(dispatcher.PublishSink(oldToken, oldSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(oldProducer.SetWindowReady(true));

  const LifecycleToken newToken = dispatcher.OnLifecycleStarted();
  CLifecycleBoundBackProducer newProducer(dispatcher, newToken);
  ASSERT_TRUE(newProducer.SetExternalMode(true));
  ASSERT_NE(dispatcher.PublishSink(newToken, newSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(newProducer.SetWindowReady(true));

  EXPECT_TRUE(oldProducer.OnLegacyRawDown(700));
  EXPECT_TRUE(oldProducer.OnLegacyRawUp(700));
  EXPECT_FALSE(oldProducer.SetExternalMode(false));
  EXPECT_FALSE(oldProducer.SetWindowReady(false));
  EXPECT_TRUE(dispatcher.IsWindowReady(newToken));
  EXPECT_EQ(oldSink->backPairs, 0);
  EXPECT_EQ(newSink->backPairs, 0);

  EXPECT_TRUE(newProducer.OnLegacyRawDown(701));
  EXPECT_TRUE(newProducer.OnLegacyRawUp(701));
  EXPECT_EQ(newSink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, WindowLossReturnsWhileAdmittedEffectFinishesOutsideMutex)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CExecutingBlockingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));

  std::thread invokeThread([&] { dispatcher.OnApi36BackInvoked(token); });
  const bool effectStarted = sink->WaitForDispatch();
  EXPECT_TRUE(effectStarted);

  std::atomic<bool> windowReturned{false};
  std::thread windowThread(
      [&]
      {
        dispatcher.SetWindowReady(token, false);
        windowReturned = true;
      });
  EXPECT_TRUE(WaitUntil([&] { return windowReturned.load(); }));
  EXPECT_FALSE(dispatcher.IsWindowReady(token));

  sink->ReleaseDispatch();
  invokeThread.join();
  windowThread.join();
  EXPECT_EQ(sink->backPairs, 1);

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
  EXPECT_EQ(sink->backPairs, 2);
}

TEST(TestJumpgateBackDispatcher, WindowLossCancelsAdmittedCommandBeforeDestinationExecution)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CBlockingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnLegacyRawDown(token, 702, 0ms, false));

  auto dispatch = std::async(std::launch::async,
                             [&] { return dispatcher.OnLegacyRawUp(token, 702, 100ms, false); });
  ASSERT_TRUE(sink->WaitForDispatch());
  ASSERT_TRUE(dispatcher.SetWindowReady(token, false));
  sink->ReleaseDispatch();
  ASSERT_EQ(dispatch.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(dispatch.get());
  EXPECT_EQ(sink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, UnpublishReturnsButBlocksReplacementUntilAdmittedEffectRetires)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CExecutingBlockingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  const PublicationToken publication = dispatcher.PublishSink(token, sink);
  ASSERT_NE(publication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));

  std::thread invokeThread([&] { dispatcher.OnApi36BackInvoked(token); });
  const bool effectStarted = sink->WaitForDispatch();
  EXPECT_TRUE(effectStarted);

  std::atomic<bool> unpublishReturned{false};
  std::thread unpublishThread(
      [&]
      {
        dispatcher.UnpublishSink(token, publication);
        unpublishReturned = true;
      });
  EXPECT_TRUE(WaitUntil([&] { return !dispatcher.HasPublishedSink(token); }));
  EXPECT_TRUE(WaitUntil([&] { return unpublishReturned.load(); }));
  EXPECT_EQ(dispatcher.PublishSink(token, std::make_shared<CRecordingBackSink>()),
            Dispatcher::INVALID_PUBLICATION_TOKEN);

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 0));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->backPairs, 0);

  sink->ReleaseDispatch();
  invokeThread.join();
  unpublishThread.join();
  EXPECT_TRUE(unpublishReturned);
  EXPECT_EQ(sink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, DuplicateUnpublishIsIdempotentWhileLeaseBlocksRepublish)
{
  Dispatcher dispatcher;
  auto retiringSink = std::make_shared<CExecutingBlockingBackSink>();
  auto replacementSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  const PublicationToken retiringPublication = dispatcher.PublishSink(token, retiringSink);
  ASSERT_NE(retiringPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));

  std::thread invokeThread([&] { dispatcher.OnApi36BackInvoked(token); });
  const bool dispatchStarted = retiringSink->WaitForDispatch();
  if (!dispatchStarted)
  {
    retiringSink->ReleaseDispatch();
    invokeThread.join();
    FAIL() << "Back command did not begin destination execution";
  }

  std::atomic<bool> firstUnpublishResult{false};
  std::thread firstUnpublishThread(
      [&] { firstUnpublishResult = dispatcher.UnpublishSink(token, retiringPublication); });
  const bool publicationRetired = WaitUntil([&] { return !dispatcher.HasPublishedSink(token); });
  if (!publicationRetired)
  {
    retiringSink->ReleaseDispatch();
    invokeThread.join();
    firstUnpublishThread.join();
    FAIL() << "Back sink publication did not retire";
  }

  EXPECT_TRUE(dispatcher.UnpublishSink(token, retiringPublication));
  EXPECT_EQ(dispatcher.PublishSink(token, retiringSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);

  retiringSink->ReleaseDispatch();
  invokeThread.join();
  firstUnpublishThread.join();
  EXPECT_TRUE(firstUnpublishResult);

  EXPECT_NE(dispatcher.PublishSink(token, replacementSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_TRUE(dispatcher.SetWindowReady(token, true));
}

TEST(TestJumpgateBackDispatcher, LifecycleDestroyInvalidatesTokenThenWaitsForAdmittedEffect)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CExecutingBlockingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));

  std::thread invokeThread([&] { dispatcher.OnApi36BackInvoked(token); });
  const bool effectStarted = sink->WaitForDispatch();
  EXPECT_TRUE(effectStarted);

  std::atomic<bool> destructionReturned{false};
  std::thread destructionThread(
      [&]
      {
        dispatcher.OnLifecycleDestroyed(token);
        destructionReturned = true;
      });
  EXPECT_TRUE(WaitUntil([&] { return !dispatcher.IsCurrentLifecycle(token); }));
  EXPECT_TRUE(WaitUntil([&] { return destructionReturned.load(); }));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->backPairs, 0);

  sink->ReleaseDispatch();
  invokeThread.join();
  destructionThread.join();
  EXPECT_TRUE(destructionReturned);
  EXPECT_EQ(sink->backPairs, 1);
  EXPECT_EQ(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
}

TEST(TestJumpgateBackDispatcher, UnpublishHasBoundedDrainWhenDestinationRemainsBlocked)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CExecutingBlockingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  const PublicationToken publication = dispatcher.PublishSink(token, sink);
  ASSERT_NE(publication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 0));

  auto invoke =
      std::async(std::launch::async, [&] { return dispatcher.OnApi36BackInvoked(token); });
  ASSERT_TRUE(sink->WaitForDispatch());
  auto unpublish =
      std::async(std::launch::async, [&] { return dispatcher.UnpublishSink(token, publication); });

  const std::future_status drainStatus = unpublish.wait_for(500ms);
  sink->ReleaseDispatch();
  ASSERT_EQ(invoke.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(invoke.get());
  ASSERT_EQ(unpublish.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(unpublish.get());
  EXPECT_EQ(drainStatus, std::future_status::ready);
}

TEST(TestJumpgateBackDispatcher, LifecycleReplacementHasBoundedDrainAndActivatesNewOwner)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CExecutingBlockingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(oldToken, oldSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetExternalMode(oldToken, true));
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 0));

  auto invoke =
      std::async(std::launch::async, [&] { return dispatcher.OnApi36BackInvoked(oldToken); });
  ASSERT_TRUE(oldSink->WaitForDispatch());
  auto replacement =
      std::async(std::launch::async, [&] { return dispatcher.OnLifecycleStarted(); });

  const std::future_status replacementStatus = replacement.wait_for(500ms);
  LifecycleToken newToken = Dispatcher::INVALID_LIFECYCLE_TOKEN;
  if (replacementStatus == std::future_status::ready)
    newToken = replacement.get();

  EXPECT_EQ(replacementStatus, std::future_status::ready);
  ASSERT_NE(newToken, Dispatcher::INVALID_LIFECYCLE_TOKEN);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(newToken));

  oldSink->ReleaseDispatch();
  ASSERT_EQ(invoke.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(invoke.get());
  if (replacementStatus != std::future_status::ready)
  {
    ASSERT_EQ(replacement.wait_for(TEST_TIMEOUT), std::future_status::ready);
    newToken = replacement.get();
  }

  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(newToken));
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(oldToken));
  EXPECT_NE(dispatcher.PublishSink(newToken, std::make_shared<CRecordingBackSink>()),
            Dispatcher::INVALID_PUBLICATION_TOKEN);
}

TEST(TestJumpgateBackDispatcher, DeferredAcceptanceCompletesOnlyAtDestinationExecution)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CDeferredBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
  EXPECT_EQ(sink->executions, 0);

  EXPECT_TRUE(sink->Execute());
  EXPECT_EQ(sink->executions, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
  EXPECT_FALSE(sink->Execute());
}

TEST(TestJumpgateBackDispatcher,
     DestinationQueueRejectionReleasesCommitAndNextBackExecutesExactlyOnce)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRejectOnceBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->dispatchAttempts, 1);
  EXPECT_EQ(sink->executions, 0);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);

  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->dispatchAttempts, 1);
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->dispatchAttempts, 2);
  EXPECT_EQ(sink->executions, 1);
  EXPECT_EQ(dispatcher.GetState(token), State::IDLE);
}

TEST(TestJumpgateBackDispatcher, RejectedCommitCannotReplayIntoReplacementLifecycle)
{
  Dispatcher dispatcher;
  auto rejectedSink = std::make_shared<CRejectOnceBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(dispatcher.PublishSink(oldToken, rejectedSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(oldToken));
  ASSERT_EQ(dispatcher.GetState(oldToken), State::IDLE);

  auto replacementSink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(dispatcher.PublishSink(replacementToken, replacementSink),
            Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(replacementToken, true));
  EXPECT_EQ(replacementSink->backPairs, 0);

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(replacementToken, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(replacementToken));
  EXPECT_EQ(replacementSink->backPairs, 1);
  EXPECT_EQ(rejectedSink->executions, 0);
}

TEST(TestJumpgateBackDispatcher,
     ReplacementPublicationIsOwnedWhileActivationWaitsForOldDestinationExecution)
{
  Dispatcher dispatcher;
  auto oldSink = std::make_shared<CBlockingBackSink>();
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(dispatcher.PublishSink(oldToken, oldSink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(oldToken, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(oldToken, 2));

  auto invoke =
      std::async(std::launch::async, [&] { return dispatcher.OnApi36BackInvoked(oldToken); });
  ASSERT_TRUE(oldSink->WaitForDispatch());

  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted(true);
  ASSERT_NE(replacementToken, Dispatcher::INVALID_LIFECYCLE_TOKEN);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(replacementToken));

  auto replacementSink = std::make_shared<CRecordingBackSink>();
  const PublicationToken replacementPublication =
      dispatcher.PublishSink(replacementToken, replacementSink);
  EXPECT_NE(replacementPublication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  EXPECT_EQ(replacementSink->backPairs, 0);

  oldSink->ReleaseDispatch();
  ASSERT_EQ(invoke.wait_for(TEST_TIMEOUT), std::future_status::ready);
  EXPECT_TRUE(invoke.get());
  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(replacementToken));
  EXPECT_TRUE(dispatcher.SetWindowReady(replacementToken, true));
  EXPECT_TRUE(dispatcher.OnApi36BackStarted(replacementToken, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(replacementToken));
  EXPECT_EQ(replacementSink->backPairs, 1);
}

TEST(TestJumpgateBackDispatcher, DeferredCancellationRetiresEffectWithoutDestinationExecution)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CDeferredBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted(true);
  const PublicationToken publication = dispatcher.PublishSink(token, sink);
  ASSERT_NE(publication, Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));

  EXPECT_TRUE(sink->Cancel());
  EXPECT_EQ(sink->executions, 0);
  EXPECT_TRUE(dispatcher.UnpublishSink(token, publication));
  EXPECT_EQ(dispatcher.GetState(token), State::COMMIT_PENDING);
}

TEST(TestJumpgateOwnedApplicationCallback, RejectionStopCleanupAndBoundsAreOwned)
{
  using Gate = KODI::MESSAGING::CBoundedApplicationCallbackGate;
  using Owned = KODI::MESSAGING::COwnedApplicationCallback;

  Gate gate{2};
  auto first = gate.TryReserve();
  auto second = gate.TryReserve();
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_FALSE(gate.TryReserve());
  EXPECT_EQ(gate.Pending(), 2u);

  first.reset();
  EXPECT_EQ(gate.Pending(), 1u);
  auto replacement = gate.TryReserve();
  ASSERT_TRUE(replacement);
  gate.Stop();
  EXPECT_FALSE(gate.TryReserve());

  auto cleanupProbe = std::make_shared<CCallbackProbe>();
  {
    auto pending = std::make_shared<Owned>(cleanupProbe);
    EXPECT_TRUE(pending->IsPending());
  }
  EXPECT_EQ(cleanupProbe->executions, 0);
  EXPECT_EQ(cleanupProbe->cancellations, 1);

  auto executionProbe = std::make_shared<CCallbackProbe>();
  auto executed = std::make_shared<Owned>(executionProbe);
  EXPECT_TRUE(executed->Execute());
  EXPECT_FALSE(executed->Execute());
  EXPECT_FALSE(executed->Cancel());
  EXPECT_EQ(executionProbe->executions, 1);
  EXPECT_EQ(executionProbe->cancellations, 0);
}

TEST(TestJumpgateOwnedApplicationCallback,
     AsyncExecutorBoundsAStalledDestinationAndCancelsQueuedWorkOnStop)
{
  using Executor = KODI::MESSAGING::CBoundedApplicationCallbackExecutor;

  Executor executor{2};
  auto blocking = std::make_shared<CBlockingCallbackProbe>();
  auto queuedFirst = std::make_shared<CCallbackProbe>();
  auto queuedSecond = std::make_shared<CCallbackProbe>();
  auto rejected = std::make_shared<CCallbackProbe>();

  ASSERT_TRUE(executor.Post(blocking));
  ASSERT_TRUE(blocking->WaitForExecution());
  EXPECT_TRUE(executor.Post(queuedFirst));
  EXPECT_TRUE(executor.Post(queuedSecond));
  EXPECT_FALSE(executor.Post(rejected));
  EXPECT_EQ(executor.Pending(), 2u);
  EXPECT_EQ(rejected->executions, 0);
  EXPECT_EQ(rejected->cancellations, 1);

  executor.Stop();
  EXPECT_EQ(queuedFirst->executions, 0);
  EXPECT_EQ(queuedFirst->cancellations, 1);
  EXPECT_EQ(queuedSecond->executions, 0);
  EXPECT_EQ(queuedSecond->cancellations, 1);
  EXPECT_FALSE(executor.Post(std::make_shared<CCallbackProbe>()));

  blocking->Release();
  ASSERT_TRUE(WaitUntil([&] { return executor.IsStopped() && executor.Pending() == 0; }));
  EXPECT_EQ(blocking->executions, 1);
  EXPECT_EQ(blocking->cancellations, 0);
}

TEST(TestJumpgateOwnedApplicationCallback, ThreadMessagePayloadCancelsOrTransfersExactlyOnce)
{
  struct Payload
  {
    explicit Payload(int& destructionCount) : destructions(destructionCount) {}
    ~Payload() { ++destructions; }
    int& destructions;
  };

  int destructions = 0;
  int cancellations = 0;
  KODI::MESSAGING::ThreadMessage cancelledMessage{1};
  auto cancelledHandle = cancelledMessage.SetOwnedPayload(std::make_unique<Payload>(destructions),
                                                          [&] { ++cancellations; });
  ASSERT_TRUE(cancelledHandle);
  EXPECT_TRUE(cancelledHandle->Cancel());
  EXPECT_FALSE(cancelledHandle->Cancel());
  EXPECT_EQ(destructions, 1);
  EXPECT_EQ(cancellations, 1);
  EXPECT_FALSE(cancelledMessage.TakeOwnedPayload<Payload>());

  KODI::MESSAGING::ThreadMessage executedMessage{2};
  auto executedHandle = executedMessage.SetOwnedPayload(std::make_unique<Payload>(destructions),
                                                        [&] { ++cancellations; });
  ASSERT_TRUE(executedHandle);
  auto payload = executedMessage.TakeOwnedPayload<Payload>();
  ASSERT_TRUE(payload);
  EXPECT_FALSE(executedHandle->Cancel());
  payload.reset();
  EXPECT_EQ(destructions, 2);
  EXPECT_EQ(cancellations, 1);
}

TEST(TestJumpgateLifecycleTargetRegistry, StaleRetirementCannotClearOrAcquireReplacement)
{
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  auto oldTarget = std::make_shared<CTargetProbe>(1);
  const auto oldPublication = registry.Publish(10, oldTarget);
  ASSERT_NE(oldPublication, 0u);
  ASSERT_EQ(registry.Acquire(10)->id, 1);

  auto newTarget = std::make_shared<CTargetProbe>(2);
  const auto newPublication = registry.Publish(11, newTarget);
  ASSERT_GT(newPublication, oldPublication);
  EXPECT_FALSE(registry.Acquire(10));
  ASSERT_EQ(registry.Acquire(11)->id, 2);

  EXPECT_FALSE(registry.Retire(10, oldPublication, oldTarget.get()));
  ASSERT_EQ(registry.Acquire(11)->id, 2);
  EXPECT_TRUE(registry.Retire(11, newPublication, newTarget.get()));
  EXPECT_FALSE(registry.Acquire(11));
}

TEST(TestJumpgateBackDispatcher,
     LifecycleOperationDefersReplacementActivationUntilAdmittedCallbackReturns)
{
  Dispatcher dispatcher;
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  auto operation = dispatcher.TryAcquireLifecycleOperation(oldToken);
  ASSERT_TRUE(operation);

  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted();
  ASSERT_GT(replacementToken, oldToken);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(oldToken));
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(replacementToken));
  EXPECT_FALSE(dispatcher.TryAcquireLifecycleOperation(oldToken));
  EXPECT_FALSE(dispatcher.TryAcquireLifecycleOperation(replacementToken));

  operation = {};
  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(replacementToken));
}

TEST(TestJumpgateLifecycleTargetRegistry,
     LifecycleOperationKeepsAcquiredTargetAliveAcrossRegistryReplacement)
{
  Dispatcher dispatcher;
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  auto oldTarget = std::make_shared<CTargetProbe>(11);
  ASSERT_NE(registry.Publish(oldToken, oldTarget), 0u);

  auto operation = dispatcher.TryAcquireLifecycleOperation(oldToken);
  ASSERT_TRUE(operation);
  auto acquiredTarget = registry.Acquire(oldToken);
  ASSERT_TRUE(acquiredTarget);

  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted();
  auto replacementTarget = std::make_shared<CTargetProbe>(12);
  ASSERT_NE(registry.Publish(replacementToken, replacementTarget), 0u);
  EXPECT_FALSE(registry.Acquire(oldToken));
  EXPECT_EQ(acquiredTarget->id, 11);
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(replacementToken));

  operation = {};
  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(replacementToken));
  ASSERT_TRUE(registry.Acquire(replacementToken));
  EXPECT_EQ(registry.Acquire(replacementToken)->id, 12);
}

TEST(TestJumpgateLifecycleTargetRegistry,
     ReplacementBetweenLifecycleLeaseAndRegistryAcquireRejectsStaleTarget)
{
  Dispatcher dispatcher;
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  const LifecycleToken oldToken = dispatcher.OnLifecycleStarted();
  auto oldTarget = std::make_shared<CTargetProbe>(13);
  ASSERT_NE(registry.Publish(oldToken, oldTarget), 0u);

  auto operation = dispatcher.TryAcquireLifecycleOperation(oldToken);
  ASSERT_TRUE(operation);

  const LifecycleToken replacementToken = dispatcher.OnLifecycleStarted();
  auto replacementTarget = std::make_shared<CTargetProbe>(14);
  ASSERT_NE(registry.Publish(replacementToken, replacementTarget), 0u);

  EXPECT_FALSE(registry.Acquire(oldToken));
  EXPECT_FALSE(dispatcher.IsCurrentLifecycle(replacementToken));

  operation = {};
  EXPECT_TRUE(dispatcher.IsCurrentLifecycle(replacementToken));
  ASSERT_TRUE(registry.Acquire(replacementToken));
  EXPECT_EQ(registry.Acquire(replacementToken)->id, 14);
}

TEST(TestJumpgateLifecycleTargetRegistry, StalePublicationCannotReplaceNewerLifecycleTarget)
{
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  auto newerTarget = std::make_shared<CTargetProbe>(4);
  const auto newerPublication = registry.Publish(31, newerTarget);
  ASSERT_NE(newerPublication, 0u);

  auto staleTarget = std::make_shared<CTargetProbe>(5);
  EXPECT_EQ(registry.Publish(30, staleTarget), 0u);
  ASSERT_EQ(registry.Acquire(31)->id, 4);

  EXPECT_TRUE(registry.Retire(31, newerPublication, newerTarget.get()));
  EXPECT_EQ(registry.Publish(30, staleTarget), 0u);
  EXPECT_FALSE(registry.Acquire(30));
}

TEST(TestJumpgateLifecycleTargetRegistry, ExactLifecycleRetirementRejectsStaleJniProducer)
{
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  auto target = std::make_shared<CTargetProbe>(3);
  const auto publication = registry.Publish(20, target);
  ASSERT_NE(publication, 0u);

  EXPECT_FALSE(registry.RetireLifecycle(19));
  ASSERT_EQ(registry.Acquire(20)->id, 3);
  EXPECT_TRUE(registry.RetireLifecycle(20));
  EXPECT_FALSE(registry.Acquire(20));
  EXPECT_FALSE(registry.Retire(20, publication, target.get()));
}

TEST(TestJumpgateLifecycleTargetRegistry,
     LifecycleRetirementBeforePublicationPermanentlyFencesToken)
{
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  EXPECT_TRUE(registry.RetireLifecycle(40));

  auto retiredTarget = std::make_shared<CTargetProbe>(6);
  EXPECT_EQ(registry.Publish(40, retiredTarget), 0u);
  EXPECT_EQ(registry.Publish(39, retiredTarget), 0u);

  auto replacement = std::make_shared<CTargetProbe>(7);
  const auto publication = registry.Publish(41, replacement);
  ASSERT_NE(publication, 0u);
  ASSERT_EQ(registry.Acquire(41)->id, 7);
}

TEST(TestJumpgateLifecycleTargetRegistry, ExactInstanceRetirementAllowsOnlySameOrNewerLifecycle)
{
  CJumpgateLifecycleTargetRegistry<CTargetProbe> registry;
  auto first = std::make_shared<CTargetProbe>(8);
  const auto firstPublication = registry.Publish(50, first);
  ASSERT_NE(firstPublication, 0u);
  EXPECT_TRUE(registry.Retire(50, firstPublication, first.get()));

  auto sameLifecycle = std::make_shared<CTargetProbe>(9);
  const auto replacementPublication = registry.Publish(50, sameLifecycle);
  ASSERT_GT(replacementPublication, firstPublication);
  ASSERT_EQ(registry.Acquire(50)->id, 9);

  auto duplicate = std::make_shared<CTargetProbe>(10);
  EXPECT_EQ(registry.Publish(50, duplicate), 0u);
  EXPECT_EQ(registry.Publish(49, duplicate), 0u);
  ASSERT_EQ(registry.Acquire(50)->id, 9);
}

TEST(TestJumpgateBackCoordinator, WarmReturnToStandaloneClearsExternalPendingCommit)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);

  EXPECT_EQ(coordinator.OnLegacyRawDown(53, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(53, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);

  coordinator.SetExternalMode(false);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
  EXPECT_EQ(coordinator.OnLegacyRawDown(54, 0ms, false), Action::PASS_THROUGH);
  EXPECT_EQ(coordinator.OnLegacyRawUp(54, 100ms, false), Action::PASS_THROUGH);
}

TEST(TestJumpgateBackCoordinator, PendingStandaloneApi36CommitKeepsItsOriginAcrossWarmTransition)
{
  CJumpgateBackCoordinator coordinator;

  const Action earlyDown = coordinator.OnApi36BackStarted(Api36Source::BUTTON);
  EXPECT_EQ(earlyDown, Action::CONSUME);
  coordinator.OnActionFailed(earlyDown);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);

  coordinator.SetExternalMode(true);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  const Action pendingCommit = coordinator.OnNativeReady();
  EXPECT_EQ(pendingCommit, Action::DISPATCH_KODI_BACK_SHORT);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
  coordinator.OnActionDelivered(pendingCommit);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, ColdExternalModeIsEstablishedBeforeFirstApi36Sequence)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);

  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  EXPECT_EQ(coordinator.OnNativeReady(), Action::DISPATCH_EXTERNAL_BACK);
}

TEST(TestJumpgateBackCoordinator, SlowApi36GesturesNeverBecomeLongPresses)
{
  for (const Api36Source source : {Api36Source::GESTURE_LEFT, Api36Source::GESTURE_RIGHT})
  {
    CJumpgateBackCoordinator coordinator;
    coordinator.SetExternalMode(true);
    MakeReady(coordinator);

    EXPECT_EQ(coordinator.OnApi36BackStarted(source), Action::CONSUME);
    EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::CONSUME);
    EXPECT_EQ(coordinator.GetState(), State::PRESSED);
    const Action command = coordinator.OnApi36BackInvoked();
    EXPECT_EQ(command, Action::DISPATCH_EXTERNAL_BACK);
    EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
    coordinator.OnActionDelivered(command);
    EXPECT_EQ(coordinator.GetState(), State::IDLE);
  }
}

TEST(TestJumpgateBackCoordinator, ShortApi36ButtonDispatchesInsteadOfBeingDropped)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  const Action command = coordinator.OnApi36BackInvoked();
  EXPECT_EQ(command, Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  coordinator.OnActionDelivered(command);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, ExternalApi36ButtonLongPressOpensSettingsOnce)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::OPEN_EXTERNAL_SETTINGS);
  EXPECT_EQ(coordinator.GetState(), State::LONG_CONSUMED);
  EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, Api36EdgeLeftWithRawHardwareEvidenceSupportsButtonLongPress)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::GESTURE_LEFT), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36RawBack(800, 0ms, true, false, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36RawBack(800, 700ms, true, true, false),
            Action::OPEN_EXTERNAL_SETTINGS);
  EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
}

TEST(TestJumpgateBackCoordinator, Api24Through35RawAndApi36CallbackRawRoutesDispatchOnce)
{
  CJumpgateBackCoordinator legacy;
  legacy.OnCreated(true);
  MakeReady(legacy);
  EXPECT_EQ(legacy.OnLegacyRawDown(801, 0ms, false), Action::CONSUME);
  EXPECT_EQ(legacy.OnLegacyRawUp(801, 100ms, false), Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(legacy.OnLegacyRawUp(801, 100ms, false), Action::CONSUME);

  CJumpgateBackCoordinator api36;
  api36.OnCreated(true);
  MakeReady(api36);
  EXPECT_EQ(api36.OnApi36BackStarted(Api36Source::GESTURE_LEFT), Action::CONSUME);
  EXPECT_EQ(api36.OnApi36RawBack(802, 0ms, true, false, false), Action::CONSUME);
  EXPECT_EQ(api36.OnApi36RawBack(802, 100ms, false, false, false), Action::CONSUME);
  EXPECT_EQ(api36.OnApi36BackInvoked(), Action::DISPATCH_EXTERNAL_BACK);
  EXPECT_EQ(api36.OnApi36BackInvoked(), Action::CONSUME);
}

TEST(TestJumpgateBackCoordinator, ExternalBackDecisionUnwindsKodiUiBeforeCancelOrStop)
{
  using Decision = JumpgateExternalBackDecision;
  EXPECT_EQ(SelectJumpgateExternalBackDecision(true, true, true), Decision::NAVIGATE_KODI_UI);
  EXPECT_EQ(SelectJumpgateExternalBackDecision(true, true, false), Decision::NAVIGATE_KODI_UI);
  EXPECT_EQ(SelectJumpgateExternalBackDecision(false, true, true), Decision::DISMISS_OSD);
  EXPECT_EQ(SelectJumpgateExternalBackDecision(false, true, false), Decision::DISMISS_OSD);
  EXPECT_EQ(SelectJumpgateExternalBackDecision(false, false, true), Decision::CANCEL_PENDING);
  EXPECT_EQ(SelectJumpgateExternalBackDecision(false, false, false), Decision::STOP_PLAYBACK);
}

TEST(TestJumpgateBackCoordinator, StandaloneApi36ButtonEmitsOneExplicitLongpressCommand)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(false);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::DISPATCH_KODI_BACK_LONG);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackDispatcher, StandaloneApi36ButtonDispatchesOneShortOrLongKodiCommand)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_EQ(sink->kodiShortBacks, 0);
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->kodiShortBacks, 1);
  EXPECT_EQ(sink->kodiLongBacks, 0);
  EXPECT_EQ(sink->backPairs, 0);

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
  EXPECT_EQ(sink->kodiShortBacks, 1);
  EXPECT_EQ(sink->kodiLongBacks, 1);
  EXPECT_EQ(sink->settingsRequests, 0);
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->kodiShortBacks, 1);
  EXPECT_EQ(sink->kodiLongBacks, 1);
  EXPECT_EQ(sink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, StandaloneApi36ButtonCancelDispatchesNoKodiCommand)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.OnApi36BackCancelled(token));
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->kodiShortBacks, 0);
  EXPECT_EQ(sink->kodiLongBacks, 0);
  EXPECT_EQ(sink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, StandaloneApi36ButtonRouteSurvivesExternalModeFlip)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  EXPECT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(dispatcher.SetExternalMode(token, true));
  EXPECT_TRUE(dispatcher.OnApi36BackLongPress(token));
  EXPECT_EQ(sink->kodiShortBacks, 0);
  EXPECT_EQ(sink->kodiLongBacks, 1);
  EXPECT_EQ(sink->settingsRequests, 0);
  EXPECT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcher, StandaloneApi36ShortRouteCannotCancelWarmExternalOwner)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CRecordingBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->kodiShortBacks, 1);
  EXPECT_EQ(sink->backPairs, 0);
}

TEST(TestJumpgateBackDispatcherIntegration,
     StandaloneApi36HoldProducesKodiLongpressTranslationAcrossInvokeAndCancel)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CKodiKeyboardBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  EXPECT_TRUE(sink->TranslatedDowns().empty());
  ASSERT_TRUE(dispatcher.OnApi36BackLongPress(token));
  ASSERT_EQ(sink->TranslatedDowns().size(), 1u);
  EXPECT_NE(sink->TranslatedDowns().back().GetModifiers() & CKey::MODIFIER_LONG, 0u);

  tinyxml2::XMLDocument keymap;
  ASSERT_EQ(keymap.LoadFile("system/keymaps/keyboard.xml"), tinyxml2::XML_SUCCESS);
  const auto* fullscreenVideo = keymap.RootElement()->FirstChildElement("FullscreenVideo");
  ASSERT_NE(fullscreenVideo, nullptr);
  const auto* keyboard = fullscreenVideo->FirstChildElement("keyboard");
  ASSERT_NE(keyboard, nullptr);
  const tinyxml2::XMLElement* longBackspace = nullptr;
  for (const auto* candidate = keyboard->FirstChildElement("backspace"); candidate != nullptr;
       candidate = candidate->NextSiblingElement("backspace"))
  {
    const char* modifier = candidate->Attribute("mod");
    if (modifier != nullptr && std::string{modifier} == "longpress")
    {
      longBackspace = candidate;
      break;
    }
  }
  ASSERT_NE(longBackspace, nullptr);
  EXPECT_EQ(KODI::KEYMAP::CKeyboardTranslator::TranslateButton(longBackspace),
            sink->TranslatedDowns().back().GetButtonCode());
  unsigned int actionId{ACTION_NONE};
  ASSERT_TRUE(KODI::ACTION::CActionTranslator::TranslateString(longBackspace->GetText(), actionId));
  EXPECT_EQ(actionId, ACTION_STOP);

  ASSERT_TRUE(dispatcher.SetExternalMode(token, true));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->Ups(), 1);
  EXPECT_EQ(sink->Pairs(), 0);
  EXPECT_EQ(sink->SettingsRequests(), 0);

  ASSERT_TRUE(dispatcher.SetExternalMode(token, false));
  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackLongPress(token));
  ASSERT_EQ(sink->TranslatedDowns().size(), 2u);
  EXPECT_NE(sink->TranslatedDowns().back().GetModifiers() & CKey::MODIFIER_LONG, 0u);
  ASSERT_TRUE(dispatcher.OnApi36BackCancelled(token));
  EXPECT_EQ(sink->Ups(), 2);
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));
  EXPECT_EQ(sink->TranslatedDowns().size(), 2u);
  EXPECT_EQ(sink->Ups(), 2);
  EXPECT_EQ(sink->Pairs(), 0);
  EXPECT_EQ(sink->SettingsRequests(), 0);
}

TEST(TestJumpgateBackDispatcherIntegration,
     DelayedKodiQueueDrainPreservesApi36StandaloneLongpressWithoutWallClockTiming)
{
  Dispatcher dispatcher;
  auto sink = std::make_shared<CDelayedKodiKeyboardBackSink>();
  const LifecycleToken token = dispatcher.OnLifecycleStarted();
  ASSERT_NE(dispatcher.PublishSink(token, sink), Dispatcher::INVALID_PUBLICATION_TOKEN);
  ASSERT_TRUE(dispatcher.SetWindowReady(token, true));

  ASSERT_TRUE(dispatcher.OnApi36BackStarted(token, 2));
  ASSERT_TRUE(dispatcher.OnApi36BackLongPress(token));
  ASSERT_TRUE(dispatcher.OnApi36BackInvoked(token));

  sink->Drain();
  ASSERT_EQ(sink->TranslatedDowns().size(), 1u);
  EXPECT_NE(sink->TranslatedDowns().back().GetModifiers() & CKey::MODIFIER_LONG, 0u);
}

TEST(TestJumpgateBackCoordinator, UnexpectedApi36RawBackIsAlwaysConsumed)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(false);
  MakeReady(coordinator);

  EXPECT_EQ(coordinator.OnUnexpectedApi36RawBack(), Action::CONSUME);
  coordinator.SetExternalMode(true);
  EXPECT_EQ(coordinator.OnUnexpectedApi36RawBack(), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::IDLE);
}

TEST(TestJumpgateBackCoordinator, TeardownSuppressesActiveAndPendingInput)
{
  CJumpgateBackCoordinator coordinator;
  coordinator.SetExternalMode(true);

  EXPECT_EQ(coordinator.OnLegacyRawDown(60, 0ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnLegacyRawUp(60, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.GetState(), State::COMMIT_PENDING);
  EXPECT_EQ(coordinator.OnDestroyed(), Action::NONE);
  EXPECT_EQ(coordinator.GetState(), State::DESTROYED);

  EXPECT_EQ(coordinator.OnNativeReady(), Action::NONE);
  EXPECT_EQ(coordinator.OnLegacyRawUp(60, 100ms, false), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackStarted(Api36Source::BUTTON), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackLongPress(), Action::CONSUME);
  EXPECT_EQ(coordinator.OnApi36BackInvoked(), Action::CONSUME);
}
