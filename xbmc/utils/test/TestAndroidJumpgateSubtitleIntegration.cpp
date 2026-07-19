/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "platform/android/activity/AndroidJumpgateSubtitleTransport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
JumpgateSubtitleBinding Binding(std::uint64_t generation, std::string session = "session_00000001")
{
  return {generation, "profile_00000001", "device_00000001", "https://bridge.example",
          std::move(session)};
}

JumpgateSubtitleCompletion Completion(const JumpgateSubtitleBinding& binding,
                                      JumpgateSubtitleResultStatus status,
                                      std::vector<std::uint8_t> bytes = {'[', 'S', 'c', 'r', 'i',
                                                                         'p', 't', ' ', 'I', 'n',
                                                                         'f', 'o', ']'})
{
  JumpgateSubtitleCompletion completion;
  completion.binding = binding;
  completion.status = status;
  completion.httpStatus = status == JumpgateSubtitleResultStatus::RePairRequired ? 401 : 200;
  if (status == JumpgateSubtitleResultStatus::Success)
  {
    completion.artifact.selected = {std::string(64, 'a'), "English", "en", "ass", 1};
    completion.artifact.parts.push_back({1, "subtitle", "text/x-ssa", std::string(64, 'b') + ".ass",
                                         std::string(64, 'c'), std::move(bytes)});
  }
  return completion;
}

JumpgateSubtitleCompletion VobSubCompletion(const JumpgateSubtitleBinding& binding)
{
  JumpgateSubtitleCompletion completion;
  completion.binding = binding;
  completion.status = JumpgateSubtitleResultStatus::Success;
  completion.httpStatus = 200;
  completion.artifact.selected = {std::string(64, 'a'), "English", "en", "vobsub", 1};
  completion.artifact.parts.push_back({1,
                                       "index",
                                       "application/x-vobsub",
                                       std::string(64, 'b') + ".idx",
                                       std::string(64, 'c'),
                                       {'#', ' ', 'V', 'o', 'b', 'S', 'u', 'b', '\n'}});
  completion.artifact.parts.push_back({2,
                                       "sub",
                                       "application/octet-stream",
                                       std::string(64, 'd') + ".sub",
                                       std::string(64, 'e'),
                                       {0x00, 0x00, 0x01, 0xba, 0x44, 0x02}});
  return completion;
}

class CAlwaysValidAnchor final : public IAndroidJumpgateSubtitleArtifactAnchor
{
public:
  bool Validate() const override { return true; }
  std::string InjectionPath(const std::string& fileName) const override
  {
    return "test://pinned/" + fileName;
  }
};

AndroidJumpgateStagedArtifact Staged(const JumpgateSubtitleBinding& binding, std::string directory)
{
  return {binding,
          std::move(directory),
          "jumpgate.en.ass",
          "test://pinned/jumpgate.en.ass",
          "en",
          "ass",
          std::make_shared<CAlwaysValidAnchor>()};
}

class CTemporaryDirectory final
{
public:
  CTemporaryDirectory()
  {
    std::error_code error;
    const std::filesystem::path parent = std::filesystem::temp_directory_path(error);
    if (error)
      throw std::runtime_error("Failed to locate the temporary directory: " + error.message());

    std::random_device random;
    std::uniform_int_distribution<std::uint64_t> nonce;
    constexpr int maxAttempts = 256;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
      std::ostringstream name;
      name << "kodi-jumpgate-subtitle-test-" << std::hex << std::setfill('0') << std::setw(16)
           << nonce(random) << '-' << attempt;
      std::filesystem::path candidate = parent / name.str();
      error.clear();
      // Atomic creation, rather than the random suffix, establishes ownership.
      if (!std::filesystem::create_directory(candidate, error))
      {
        if (!error || error == std::errc::file_exists)
          continue;
        throw std::runtime_error("Failed to create temporary directory '" + candidate.string() +
                                 "': " + error.message());
      }

#if !defined(_WIN32)
      std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace, error);
      if (error)
      {
        const std::string message = "Failed to secure temporary directory '" +
                                    candidate.string() + "': " + error.message();
        std::error_code cleanupError;
        std::filesystem::remove_all(candidate, cleanupError);
        throw std::runtime_error(message);
      }
#endif
      m_path = std::move(candidate);
      return;
    }

    throw std::runtime_error("Failed to acquire an isolated temporary directory after " +
                             std::to_string(maxAttempts) + " atomic attempts");
  }

  ~CTemporaryDirectory()
  {
    if (m_path.empty())
      return;
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  const std::filesystem::path& Path() const { return m_path; }

private:
  std::filesystem::path m_path;
};

CAndroidJumpgateSubtitleFileStore::TokenGenerator SequentialTokens()
{
  auto next = std::make_shared<std::atomic<std::uint64_t>>(1);
  return [next]
  {
    std::ostringstream token;
    token << std::hex << std::setfill('0') << std::setw(32) << next->fetch_add(1);
    return token.str();
  };
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
{
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<AndroidJumpgateStageCompletion> WaitForStage(
    CAndroidJumpgateSubtitleStageWorker& worker,
    const JumpgateSubtitleBinding& binding,
    std::chrono::milliseconds timeout = 2s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    std::optional<AndroidJumpgateStageCompletion> completion = worker.TakeCompletion(binding);
    if (completion)
      return completion;
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

struct CBlockingRunnerState
{
  std::mutex mutex;
  std::condition_variable condition;
  bool opened{false};
};

class CBlockingRunnerTransport final : public IJumpgateSubtitleTransport
{
public:
  explicit CBlockingRunnerTransport(std::shared_ptr<CBlockingRunnerState> state)
    : m_state(std::move(state))
  {
  }

  bool Perform(const JumpgateSubtitleHttpRequest&,
               JumpgateSubtitleHttpResponse&,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    {
      std::lock_guard lock(m_state->mutex);
      m_state->opened = true;
      m_state->condition.notify_all();
    }
    while (!cancellation.IsCancelled())
      std::this_thread::sleep_for(1ms);
    {
      std::lock_guard lock(m_state->mutex);
      m_state->opened = false;
      m_state->condition.notify_all();
    }
    return false;
  }

private:
  std::shared_ptr<CBlockingRunnerState> m_state;
};
} // namespace

TEST(TestAndroidJumpgateSubtitleIntegration, EarlyCompletionStagesBeforeExactPlaybackStart)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding binding = Binding(1);
  ASSERT_TRUE(lifecycle.PrepareGeneration(binding.generation));
  ASSERT_TRUE(lifecycle.Bind(binding));
  ASSERT_TRUE(
      lifecycle.AcceptStaged(Staged(binding, "C:/temp/jg-1-00000000000000000000000000000001")));
  EXPECT_FALSE(lifecycle.TakeInjection(binding));

  ASSERT_TRUE(lifecycle.MarkPlaybackReady(binding.generation));
  std::optional<AndroidJumpgateStagedArtifact> injection = lifecycle.TakeInjection(binding);
  ASSERT_TRUE(injection);
  EXPECT_EQ(injection->language, "en");
}

TEST(TestAndroidJumpgateSubtitleIntegration, InjectedAnchorSurvivesReplacementUntilTerminalCommit)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding binding = Binding(23);
  ASSERT_TRUE(lifecycle.PrepareGeneration(binding.generation));
  ASSERT_TRUE(lifecycle.Bind(binding));
  AndroidJumpgateStagedArtifact artifact =
      Staged(binding, "C:/temp/jg-23-00000000000000000000000000000023");
  std::weak_ptr<IAndroidJumpgateSubtitleArtifactAnchor> retainedAnchor = artifact.anchor;
  ASSERT_TRUE(lifecycle.AcceptStaged(std::move(artifact)));
  ASSERT_TRUE(lifecycle.MarkPlaybackReady(binding.generation));
  std::optional<AndroidJumpgateStagedArtifact> injection = lifecycle.TakeInjection(binding);
  ASSERT_TRUE(injection);
  injection.reset();
  EXPECT_FALSE(retainedAnchor.expired());

  ASSERT_TRUE(lifecycle.PrepareGeneration(24));
  EXPECT_FALSE(retainedAnchor.expired());
  const std::vector<std::string> cleanup = lifecycle.CommitTerminal(binding.generation);
  ASSERT_EQ(cleanup.size(), 1U);
  EXPECT_EQ(cleanup.front(), "C:/temp/jg-23-00000000000000000000000000000023");
  EXPECT_TRUE(retainedAnchor.expired());
}

TEST(TestAndroidJumpgateSubtitleIntegration, ReplacementRejectsEveryStaleBindingField)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding first = Binding(1);
  ASSERT_TRUE(lifecycle.PrepareGeneration(first.generation));
  ASSERT_TRUE(lifecycle.Bind(first));
  ASSERT_TRUE(
      lifecycle.AcceptStaged(Staged(first, "C:/temp/jg-1-00000000000000000000000000000001")));

  const JumpgateSubtitleBinding replacement = Binding(2, "session_00000002");
  ASSERT_TRUE(lifecycle.PrepareGeneration(replacement.generation));
  ASSERT_TRUE(lifecycle.Bind(replacement));
  ASSERT_TRUE(lifecycle.MarkPlaybackReady(replacement.generation));
  EXPECT_EQ(lifecycle.TakeAction(first).type, AndroidJumpgateSubtitleActionType::None);

  for (int field = 0; field < 4; ++field)
  {
    JumpgateSubtitleBinding stale = replacement;
    if (field == 0)
      stale.profileId = "profile_00000002";
    else if (field == 1)
      stale.deviceId = "device_00000002";
    else if (field == 2)
      stale.bridgeOrigin = "https://other.example";
    else
      stale.sessionId = "session_00000003";
    EXPECT_FALSE(lifecycle.AcceptStaged(Staged(stale, "C:/temp/stale")));
    EXPECT_FALSE(lifecycle.TakeInjection(stale));
  }
}

TEST(TestAndroidJumpgateSubtitleIntegration, AuthenticationExpiryNotifiesOncePerGeneration)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding binding = Binding(3);
  ASSERT_TRUE(lifecycle.PrepareGeneration(binding.generation));
  ASSERT_TRUE(lifecycle.Bind(binding));

  ASSERT_TRUE(lifecycle.AcceptStatus(binding, JumpgateSubtitleResultStatus::RePairRequired, 401));
  EXPECT_EQ(lifecycle.TakeAction(binding).type, AndroidJumpgateSubtitleActionType::NotifyRePair);
  ASSERT_TRUE(lifecycle.AcceptStatus(binding, JumpgateSubtitleResultStatus::RePairRequired, 401));
  EXPECT_EQ(lifecycle.TakeAction(binding).type, AndroidJumpgateSubtitleActionType::None);
}

TEST(TestAndroidJumpgateSubtitleIntegration, StaleAndProtocolFailuresRemainVideoSafe)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding binding = Binding(4);
  ASSERT_TRUE(lifecycle.PrepareGeneration(binding.generation));
  ASSERT_TRUE(lifecycle.Bind(binding));

  ASSERT_TRUE(lifecycle.AcceptStatus(binding, JumpgateSubtitleResultStatus::Stale, 404));
  EXPECT_EQ(lifecycle.TakeAction(binding).type, AndroidJumpgateSubtitleActionType::None);

  ASSERT_TRUE(lifecycle.AcceptStatus(binding, JumpgateSubtitleResultStatus::ProtocolFailure, 0));
  EXPECT_EQ(lifecycle.TakeAction(binding).type, AndroidJumpgateSubtitleActionType::LogFailure);
}

TEST(TestAndroidJumpgateSubtitleIntegration, CleanupWaitsForTerminalAndPlayerQuiescence)
{
  CAndroidJumpgateSubtitleLifecycle lifecycle;
  const JumpgateSubtitleBinding first = Binding(4);
  ASSERT_TRUE(lifecycle.PrepareGeneration(first.generation));
  ASSERT_TRUE(lifecycle.Bind(first));
  ASSERT_TRUE(
      lifecycle.AcceptStaged(Staged(first, "C:/temp/jg-4-00000000000000000000000000000004")));

  ASSERT_TRUE(lifecycle.PrepareGeneration(5));
  EXPECT_TRUE(lifecycle.Shutdown(true).empty());
  const std::vector<std::string> terminalCleanup = lifecycle.CommitTerminal(4);
  ASSERT_EQ(terminalCleanup.size(), 1u);
  EXPECT_EQ(terminalCleanup.front(), "C:/temp/jg-4-00000000000000000000000000000004");

  const JumpgateSubtitleBinding second = Binding(6);
  ASSERT_TRUE(lifecycle.PrepareGeneration(second.generation));
  ASSERT_TRUE(lifecycle.Bind(second));
  ASSERT_TRUE(
      lifecycle.AcceptStaged(Staged(second, "C:/temp/jg-6-00000000000000000000000000000006")));
  EXPECT_TRUE(lifecycle.Shutdown(true).empty());
  const std::vector<std::string> finalCleanup = lifecycle.Shutdown(false);
  ASSERT_EQ(finalCleanup.size(), 1u);
  EXPECT_EQ(finalCleanup.front(), "C:/temp/jg-6-00000000000000000000000000000006");
}

TEST(TestAndroidJumpgateSubtitleIntegration, ProviderPolicyPreventsDuplicatesAndHonorsIsolation)
{
  EXPECT_EQ(SelectAndroidJumpgateSubtitleProvider(false, true, true, true, true),
            AndroidJumpgateSubtitleProvider::Standalone);
  EXPECT_EQ(SelectAndroidJumpgateSubtitleProvider(true, false, false, true, false),
            AndroidJumpgateSubtitleProvider::OpenSubtitles);
  EXPECT_EQ(SelectAndroidJumpgateSubtitleProvider(true, true, true, true, false),
            AndroidJumpgateSubtitleProvider::BridgePending);
  EXPECT_EQ(SelectAndroidJumpgateSubtitleProvider(true, true, true, true, true),
            AndroidJumpgateSubtitleProvider::Bridge);
  EXPECT_EQ(SelectAndroidJumpgateSubtitleProvider(true, true, true, false, true),
            AndroidJumpgateSubtitleProvider::Disabled);
  EXPECT_NE(SelectAndroidJumpgateSubtitleProvider(true, true, true, true, true),
            AndroidJumpgateSubtitleProvider::OpenSubtitles);
}

TEST(TestAndroidJumpgateSubtitleIntegration, ActiveRequestStopDrainsThroughFinalRegistry)
{
  auto state = std::make_shared<CBlockingRunnerState>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  CJumpgateSubtitleCoordinator coordinator(std::make_shared<CBlockingRunnerTransport>(state), {},
                                           registry);
  JumpgateSubtitleRequest request{
      Binding(9), CJumpgateSubtitleBearerAuthority(std::string(43, 'T')), {"en"}};
  ASSERT_TRUE(coordinator.Queue(std::move(request)));
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 500ms, [&state] { return state->opened; }));
  }

  const bool stoppedSynchronously = coordinator.Stop(0ms);
  EXPECT_TRUE(stoppedSynchronously || registry->Pending() == 1u);
  EXPECT_TRUE(registry->JoinAllFor(1s));
  EXPECT_EQ(registry->Pending(), 0u);
}

TEST(TestAndroidJumpgateSubtitleIntegration, RegistryDrainingWorkerRetainsCapacitySlot)
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool release{false};
    bool finished{false};
    bool waitEntered{false};
  };
  auto state = std::make_shared<State>();
  CJumpgateThreadRegistry registry{1};
  auto reservation = registry.Reserve();
  ASSERT_TRUE(reservation);
  std::thread first(
      [state]
      {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&] { return state->release; });
        state->finished = true;
        state->condition.notify_all();
      });
  registry.Adopt(first, std::move(reservation),
                 [state](std::chrono::milliseconds timeout)
                 {
                   std::unique_lock lock(state->mutex);
                   state->waitEntered = true;
                   state->condition.notify_all();
                   return state->condition.wait_for(lock, timeout, [&] { return state->finished; });
                 });

  std::atomic_bool drained{false};
  std::thread drain([&] { drained.store(registry.JoinAllFor(2s)); });
  {
    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->condition.wait_for(lock, 1s, [&] { return state->waitEntered; }));
  }
  EXPECT_EQ(registry.Occupied(), 1U);
  EXPECT_FALSE(registry.Reserve());
  EXPECT_FALSE(registry.JoinAllFor(20ms));
  {
    std::lock_guard lock(state->mutex);
    state->release = true;
    state->condition.notify_all();
  }
  drain.join();
  EXPECT_TRUE(drained.load());
  EXPECT_EQ(registry.Occupied(), 0U);
  EXPECT_TRUE(registry.Reserve());
}

TEST(TestAndroidJumpgateSubtitleIntegration, RegistryFinalDrainClosesAdmissionForLateMutation)
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool release{false};
    bool finished{false};
  };
  auto state = std::make_shared<State>();
  CJumpgateThreadRegistry registry{1};
  auto reservation = registry.Reserve();
  ASSERT_TRUE(reservation);
  std::thread worker(
      [state]
      {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&] { return state->release; });
        state->finished = true;
        state->condition.notify_all();
      });

  std::atomic_bool drained{false};
  std::thread finalDrain([&] { drained.store(registry.JoinAll(2s)); });
  for (int attempt = 0; attempt < 200 && !registry.AdmissionClosed(); ++attempt)
    std::this_thread::sleep_for(1ms);
  ASSERT_TRUE(registry.AdmissionClosed());
  EXPECT_FALSE(registry.Reserve());

  registry.Adopt(worker, std::move(reservation),
                 [state](std::chrono::milliseconds timeout)
                 {
                   std::unique_lock lock(state->mutex);
                   return state->condition.wait_for(lock, timeout, [&] { return state->finished; });
                 });
  {
    std::lock_guard lock(state->mutex);
    state->release = true;
    state->condition.notify_all();
  }
  finalDrain.join();
  EXPECT_TRUE(drained.load());
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.Occupied(), 0U);
}

TEST(TestAndroidJumpgateSubtitleIntegration,
     RegistryTimeoutLateCompletionSelfReapsWithoutSecondDrain)
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool release{false};
    bool finished{false};
  };
  auto state = std::make_shared<State>();
  CJumpgateThreadRegistry registry{1};
  auto reservation = registry.Reserve();
  ASSERT_TRUE(reservation);
  std::thread worker(
      [state]
      {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&] { return state->release; });
        state->finished = true;
        state->condition.notify_all();
      });
  registry.Adopt(worker, std::move(reservation),
                 [state](std::chrono::milliseconds timeout)
                 {
                   std::unique_lock lock(state->mutex);
                   return state->condition.wait_for(lock, timeout, [&] { return state->finished; });
                 });

  EXPECT_FALSE(registry.JoinAll(20ms));
  EXPECT_TRUE(registry.AdmissionClosed());
  EXPECT_EQ(registry.Pending(), 1U);
  EXPECT_EQ(registry.Occupied(), 1U);
  EXPECT_FALSE(registry.Reserve());
  {
    std::lock_guard lock(state->mutex);
    state->release = true;
    state->condition.notify_all();
  }
  for (int attempt = 0; attempt < 1000 && (registry.Pending() != 0 || registry.Occupied() != 0);
       ++attempt)
  {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(registry.Pending(), 0U);
  EXPECT_EQ(registry.Occupied(), 0U);
  EXPECT_TRUE(registry.AdmissionClosed());
  EXPECT_FALSE(registry.Reserve());
}

TEST(TestAndroidJumpgateSubtitleIntegration, StagingAndSecureClearRunOffCallingThread)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  std::mutex observationMutex;
  std::condition_variable observationCondition;
  std::thread::id publishThread;
  std::thread::id clearThread;
  bool clearObserved = false;
  auto store = std::make_shared<CAndroidJumpgateSubtitleFileStore>(
      temporary.Path(), SequentialTokens(), CAndroidJumpgateSubtitleFileStore::Clock{},
      [&](const std::filesystem::path&)
      {
        std::lock_guard lock(observationMutex);
        publishThread = std::this_thread::get_id();
      });
  CAndroidJumpgateSubtitleStageWorker worker(store, std::make_shared<CJumpgateThreadRegistry>(),
                                             [&](const JumpgateSubtitleCompletion& cleared)
                                             {
                                               std::lock_guard lock(observationMutex);
                                               clearThread = std::this_thread::get_id();
                                               clearObserved = cleared.artifact.parts.empty();
                                               observationCondition.notify_all();
                                             });

  const std::thread::id callingThread = std::this_thread::get_id();
  const JumpgateSubtitleBinding binding = Binding(10);
  ASSERT_TRUE(worker.Queue(Completion(binding, JumpgateSubtitleResultStatus::Success)));
  std::optional<AndroidJumpgateStageCompletion> completion = WaitForStage(worker, binding);
  ASSERT_TRUE(completion);
  ASSERT_TRUE(completion->artifact);
  {
    std::unique_lock lock(observationMutex);
    ASSERT_TRUE(
        observationCondition.wait_for(lock, 500ms, [&clearObserved] { return clearObserved; }));
  }
  EXPECT_NE(publishThread, callingThread);
  EXPECT_EQ(publishThread, clearThread);
  EXPECT_EQ(
      ReadBytes(completion->artifact->injectionPath),
      (std::vector<std::uint8_t>{'[', 'S', 'c', 'r', 'i', 'p', 't', ' ', 'I', 'n', 'f', 'o', ']'}));
  EXPECT_TRUE(
      std::regex_match(std::filesystem::path(completion->artifact->directory).filename().string(),
                       std::regex{"^jg-10-[a-f0-9]{32}$"}));
  completion->artifact->anchor.reset();
  ASSERT_TRUE(worker.QueueCleanup(completion->artifact->directory));
  EXPECT_TRUE(worker.Stop(2s));
  EXPECT_FALSE(std::filesystem::exists(completion->artifact->directory));
}

TEST(TestAndroidJumpgateSubtitleIntegration, PostStopQueueRejectsWithoutSpawningOrMovingPayload)
{
  CTemporaryDirectory temporary;
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store =
      std::make_shared<CAndroidJumpgateSubtitleFileStore>(temporary.Path(), SequentialTokens());
  CAndroidJumpgateSubtitleStageWorker worker(store, registry);

  ASSERT_TRUE(worker.Stop(1s));
  JumpgateSubtitleCompletion completion =
      Completion(Binding(17), JumpgateSubtitleResultStatus::Success);
  EXPECT_FALSE(worker.Queue(std::move(completion)));
  EXPECT_FALSE(completion.artifact.parts.empty());
  EXPECT_EQ(registry->Pending(), 0U);
}

TEST(TestAndroidJumpgateSubtitleIntegration, VobSubPublishesSubThenIndexAndInjectsIndexOnly)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  std::vector<std::filesystem::path> published;
  CAndroidJumpgateSubtitleFileStore store(
      temporary.Path(), SequentialTokens(), CAndroidJumpgateSubtitleFileStore::Clock{},
      [&](const std::filesystem::path& path) { published.emplace_back(path); });
  const JumpgateSubtitleCompletion completion = VobSubCompletion(Binding(11));

  std::optional<AndroidJumpgateStagedArtifact> staged = store.Stage(completion);
  ASSERT_TRUE(staged);
  ASSERT_EQ(published.size(), 2u);
  EXPECT_EQ(published[0].extension(), ".sub");
  EXPECT_EQ(published[1].extension(), ".idx");
  EXPECT_EQ(std::filesystem::path(staged->injectionPath).extension(), ".idx");
  EXPECT_EQ(published[0].stem(), published[1].stem());
  EXPECT_EQ(ReadBytes(published[0]), completion.artifact.parts[1].bytes);
  EXPECT_EQ(ReadBytes(published[1]), completion.artifact.parts[0].bytes);
  ASSERT_TRUE(staged->anchor);
  EXPECT_TRUE(staged->anchor->Validate());
  const std::string indexPath = staged->anchor->InjectionPath(staged->injectionFileName);
  const std::string subPath = staged->anchor->InjectionPath("jumpgate.en.sub");
  ASSERT_FALSE(indexPath.empty());
  ASSERT_FALSE(subPath.empty());
  EXPECT_EQ(indexPath.substr(0, indexPath.rfind('/')), subPath.substr(0, subPath.rfind('/')));
  const AndroidJumpgateCleanupReport retainedCleanup =
      store.RemoveGenerationDirectory(staged->directory);
  EXPECT_TRUE(retainedCleanup.inUse);
  const std::string directory = staged->directory;
  staged.reset();
  EXPECT_FALSE(store.RemoveGenerationDirectory(directory).inUse);
  EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(TestAndroidJumpgateSubtitleIntegration, PinnedDirectorySurvivesPostStagePathReplacement)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CAndroidJumpgateSubtitleFileStore store(temporary.Path(), SequentialTokens());
  const auto staged = store.Stage(Completion(Binding(19), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(staged);
  const std::vector<std::uint8_t> expected = ReadBytes(staged->injectionPath);
  const std::filesystem::path original = staged->directory;
  const std::filesystem::path moved = original.string() + ".moved";
  std::error_code error;
  std::filesystem::rename(original, moved, error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(std::filesystem::create_directory(original));
  std::ofstream{original / staged->injectionFileName} << "attacker replacement";

  ASSERT_TRUE(staged->anchor);
  EXPECT_TRUE(staged->anchor->Validate());
  EXPECT_EQ(ReadBytes(staged->anchor->InjectionPath(staged->injectionFileName)), expected);
}

TEST(TestAndroidJumpgateSubtitleIntegration,
     TextFileFdAndGenerationLockPreventOwnCleanupOrReplacement)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CAndroidJumpgateSubtitleFileStore store(temporary.Path(), SequentialTokens());
  auto staged = store.Stage(Completion(Binding(20), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(staged);
  EXPECT_TRUE(std::regex_match(staged->injectionPath, std::regex{"^/proc/self/fd/[0-9]+$"}));
  const std::filesystem::path original =
      std::filesystem::path(staged->directory) / staged->injectionFileName;
  const std::filesystem::path moved = original.string() + ".moved";
  std::error_code error;
  std::filesystem::rename(original, moved, error);
  EXPECT_TRUE(error);

  ASSERT_TRUE(staged->anchor);
  EXPECT_TRUE(staged->anchor->Validate());
  const AndroidJumpgateCleanupReport retainedCleanup =
      store.RemoveGenerationDirectory(staged->directory);
  EXPECT_TRUE(retainedCleanup.inUse);
  EXPECT_TRUE(std::filesystem::exists(original));

  const std::string directory = staged->directory;
  staged.reset();
  const AndroidJumpgateCleanupReport releasedCleanup = store.RemoveGenerationDirectory(directory);
  EXPECT_FALSE(releasedCleanup.inUse);
  EXPECT_FALSE(releasedCleanup.containmentRejected);
  EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(TestAndroidJumpgateSubtitleIntegration, CapsEachPartAtEightMiBAndAggregateAtTwelveMiB)
{
  JumpgateSubtitleCompletion completion =
      Completion(Binding(12), JumpgateSubtitleResultStatus::Success,
                 std::vector<std::uint8_t>(ANDROID_JUMPGATE_MAX_PART_BYTES, 0x41));
  completion.artifact.parts.push_back(
      {2, "sub", "application/octet-stream", std::string(64, 'd') + ".sub", std::string(64, 'e'),
       std::vector<std::uint8_t>(
           ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES - ANDROID_JUMPGATE_MAX_PART_BYTES, 0x42)});
  EXPECT_TRUE(IsWithinAndroidJumpgateArtifactCap(completion));

  completion.artifact.parts[1].bytes.push_back(0x42);
  EXPECT_FALSE(IsWithinAndroidJumpgateArtifactCap(completion));
  completion.artifact.parts[1].bytes.resize(ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES -
                                            ANDROID_JUMPGATE_MAX_PART_BYTES);
  completion.artifact.parts[0].bytes.push_back(0x41);
  EXPECT_FALSE(IsWithinAndroidJumpgateArtifactCap(completion));
}

TEST(TestAndroidJumpgateSubtitleIntegration, TransportBudgetRejectsAggregateBeforeSecondDownload)
{
  CAndroidJumpgateArtifactBudget budget;
  constexpr std::size_t indexBytes = ANDROID_JUMPGATE_MAX_PART_BYTES;
  constexpr std::size_t subBytes =
      ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES - ANDROID_JUMPGATE_MAX_PART_BYTES;
  EXPECT_TRUE(budget.Reserve("session/artifact", "1/index.idx", indexBytes));
  EXPECT_TRUE(budget.Reserve("session/artifact", "1/index.idx", indexBytes));
  EXPECT_TRUE(budget.Reserve("session/artifact", "2/sub.sub", subBytes));
  EXPECT_FALSE(budget.Reserve("session/artifact", "3/extra.sub", 1));

  budget.Reset();
  EXPECT_FALSE(
      budget.Reserve("session/artifact", "1/part.sub", ANDROID_JUMPGATE_MAX_PART_BYTES + 1));
}

TEST(TestAndroidJumpgateSubtitleIntegration, CleanupRejectsEscapeAndNeverRecurses)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows cleanup is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CTemporaryDirectory outside;
  CAndroidJumpgateSubtitleFileStore store(temporary.Path(), SequentialTokens());
  std::optional<AndroidJumpgateStagedArtifact> staged =
      store.Stage(Completion(Binding(13), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(staged);

  const std::filesystem::path outsideFile = outside.Path() / "keep.txt";
  std::ofstream{outsideFile} << "keep";
  AndroidJumpgateCleanupReport rejected = store.RemoveGenerationDirectory(outside.Path().string());
  EXPECT_TRUE(rejected.containmentRejected);
  EXPECT_TRUE(std::filesystem::exists(outsideFile));

  const std::filesystem::path nested =
      std::filesystem::path(staged->directory) / "unexpected-directory";
  std::error_code permissionError;
  std::filesystem::permissions(staged->directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, permissionError);
  ASSERT_FALSE(permissionError) << permissionError.message();
  staged->anchor.reset();
  ASSERT_TRUE(std::filesystem::create_directory(nested));
  const std::filesystem::path nestedFile = nested / "keep.txt";
  std::ofstream{nestedFile} << "keep";
  store.RemoveGenerationDirectory(staged->directory);
  EXPECT_TRUE(std::filesystem::exists(nestedFile));
  EXPECT_TRUE(std::filesystem::exists(staged->directory));
}

TEST(TestAndroidJumpgateSubtitleIntegration, RootOpenRejectsIntermediateSymlink)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CTemporaryDirectory outside;
  const std::filesystem::path linkedParent = temporary.Path() / "linked-parent";
  std::error_code error;
  std::filesystem::create_directory_symlink(outside.Path(), linkedParent, error);
  if (error)
    GTEST_SKIP() << "Directory symlink creation is unavailable: " << error.message();

  CAndroidJumpgateSubtitleFileStore store(linkedParent / "subtitle-root", SequentialTokens());
  EXPECT_FALSE(store.Stage(Completion(Binding(21), JumpgateSubtitleResultStatus::Success)));
  EXPECT_FALSE(std::filesystem::exists(outside.Path() / "subtitle-root"));
}

TEST(TestAndroidJumpgateSubtitleIntegration, RootTraversalAllowsExecuteOnlyAncestor)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  const std::filesystem::path ancestor = temporary.Path() / "execute-only";
  const std::filesystem::path root = ancestor / "owned-root";
  ASSERT_TRUE(std::filesystem::create_directories(root));
  std::error_code error;
  std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error.message();
  std::filesystem::permissions(ancestor, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error.message();

  CAndroidJumpgateSubtitleFileStore store(root, SequentialTokens());
  auto staged = store.Stage(Completion(Binding(22), JumpgateSubtitleResultStatus::Success));
  std::filesystem::permissions(ancestor, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(staged);
  EXPECT_EQ(
      ReadBytes(staged->injectionPath),
      (std::vector<std::uint8_t>{'[', 'S', 'c', 'r', 'i', 'p', 't', ' ', 'I', 'n', 'f', 'o', ']'}));
  const std::string directory = staged->directory;
  staged.reset();
  EXPECT_FALSE(store.RemoveGenerationDirectory(directory).containmentRejected);
}

TEST(TestAndroidJumpgateSubtitleIntegration, CleanupUnlinksSymlinkWithoutFollowingTarget)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows cleanup is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CTemporaryDirectory outside;
  CAndroidJumpgateSubtitleFileStore store(temporary.Path(), SequentialTokens());
  std::optional<AndroidJumpgateStagedArtifact> staged =
      store.Stage(Completion(Binding(14), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(staged);

  const std::filesystem::path outsideFile = outside.Path() / "keep.txt";
  std::ofstream{outsideFile} << "keep";
  const std::filesystem::path link = std::filesystem::path(staged->directory) / "outside-link";
  std::error_code error;
  std::filesystem::permissions(staged->directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error.message();
  staged->anchor.reset();
  std::filesystem::create_symlink(outsideFile, link, error);
  if (error)
    GTEST_SKIP() << "Symlink creation is unavailable: " << error.message();

  store.RemoveGenerationDirectory(staged->directory);
  EXPECT_TRUE(std::filesystem::exists(outsideFile));
  EXPECT_FALSE(std::filesystem::symlink_status(link).type() == std::filesystem::file_type::symlink);
  EXPECT_FALSE(std::filesystem::exists(staged->directory));
}

TEST(TestAndroidJumpgateSubtitleIntegration, CleanupRejectsHardlinkLinkCountAnomaly)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows cleanup is fail-closed";
#endif
  CTemporaryDirectory temporary;
  CAndroidJumpgateSubtitleFileStore store(temporary.Path(), SequentialTokens());
  std::optional<AndroidJumpgateStagedArtifact> staged =
      store.Stage(Completion(Binding(15), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(staged);
  const std::filesystem::path alias =
      std::filesystem::path(staged->directory) / "hardlink-alias.ass";
  std::error_code error;
  std::filesystem::permissions(staged->directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error.message();
  staged->anchor.reset();
  std::filesystem::create_hard_link(
      std::filesystem::path(staged->directory) / staged->injectionFileName, alias, error);
  if (error)
    GTEST_SKIP() << "Hardlink creation is unavailable: " << error.message();

  const AndroidJumpgateCleanupReport report = store.RemoveGenerationDirectory(staged->directory);
  EXPECT_TRUE(report.containmentRejected);
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(staged->directory) /
                                      staged->injectionFileName));
  EXPECT_TRUE(std::filesystem::exists(alias));
}

TEST(TestAndroidJumpgateSubtitleIntegration, PublicationRejectsHardlinkMutation)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  bool mutationAttempted = false;
  std::error_code mutationError;
  std::filesystem::path alias;
  CAndroidJumpgateSubtitleFileStore store(
      temporary.Path(), SequentialTokens(), CAndroidJumpgateSubtitleFileStore::Clock{},
      CAndroidJumpgateSubtitleFileStore::PublishObserver{},
      [&](const char* operation, const std::filesystem::path& temporaryPath)
      {
        if (mutationAttempted || std::string{operation} != "before-publish")
          return;
        mutationAttempted = true;
        alias = temporaryPath.parent_path() / "hardlink-alias.part";
        std::filesystem::create_hard_link(temporaryPath, alias, mutationError);
      });

  const std::optional<AndroidJumpgateStagedArtifact> staged =
      store.Stage(Completion(Binding(18), JumpgateSubtitleResultStatus::Success));
  ASSERT_TRUE(mutationAttempted);
  if (mutationError)
    GTEST_SKIP() << "Hardlink creation is unavailable: " << mutationError.message();
  EXPECT_FALSE(staged);
  EXPECT_TRUE(std::filesystem::exists(alias));
}

TEST(TestAndroidJumpgateSubtitleIntegration, GenerationPathReplacementFailsClosed)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows staging is fail-closed";
#endif
  CTemporaryDirectory temporary;
  bool replacementAttempted = false;
  bool replacementSucceeded = false;
  std::filesystem::path movedDirectory;
  CAndroidJumpgateSubtitleFileStore store(
      temporary.Path(), SequentialTokens(), CAndroidJumpgateSubtitleFileStore::Clock{},
      CAndroidJumpgateSubtitleFileStore::PublishObserver{},
      [&](const char* operation, const std::filesystem::path& directory)
      {
        if (replacementAttempted || std::string{operation} != "generation-opened")
          return;
        replacementAttempted = true;
        movedDirectory = directory.string() + ".moved";
        std::error_code error;
        std::filesystem::rename(directory, movedDirectory, error);
        replacementSucceeded = !error;
        if (replacementSucceeded)
        {
          ASSERT_TRUE(std::filesystem::create_directory(directory));
          std::ofstream{directory / "attacker-marker"} << "keep";
        }
      });

  const std::optional<AndroidJumpgateStagedArtifact> staged =
      store.Stage(Completion(Binding(16), JumpgateSubtitleResultStatus::Success));
  EXPECT_TRUE(replacementAttempted);
  EXPECT_TRUE(replacementSucceeded);
  EXPECT_FALSE(staged);
  EXPECT_TRUE(std::filesystem::exists(temporary.Path() / "jg-16-00000000000000000000000000000001" /
                                      "attacker-marker"));
}

TEST(TestAndroidJumpgateSubtitleIntegration, StartupSweepHonorsEntryByteAndTimeBudgets)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Production Windows cleanup is fail-closed";
#endif
  CTemporaryDirectory entryTemporary;
  CAndroidJumpgateSubtitleFileStore entryStore(entryTemporary.Path(), SequentialTokens());
  ASSERT_TRUE(entryStore.Stage(Completion(Binding(15), JumpgateSubtitleResultStatus::Success)));
  const AndroidJumpgateCleanupReport entryReport = entryStore.SweepStartupOrphans({1, 1024, 1s});
  EXPECT_TRUE(entryReport.entryLimitHit);
  EXPECT_LE(entryReport.entriesVisited, 1u);

  CTemporaryDirectory byteTemporary;
  CAndroidJumpgateSubtitleFileStore byteStore(byteTemporary.Path(), SequentialTokens());
  ASSERT_TRUE(byteStore.Stage(Completion(Binding(16), JumpgateSubtitleResultStatus::Success)));
  const AndroidJumpgateCleanupReport byteReport = byteStore.SweepStartupOrphans({32, 1, 1s});
  EXPECT_TRUE(byteReport.byteLimitHit);
  EXPECT_LE(byteReport.bytesRemoved, 1u);

  CTemporaryDirectory timeTemporary;
  auto ticks = std::make_shared<std::atomic<std::int64_t>>(0);
  CAndroidJumpgateSubtitleFileStore timeStore(
      timeTemporary.Path(), SequentialTokens(),
      [ticks] { return std::chrono::steady_clock::time_point{10ms * ticks->fetch_add(1)}; });
  ASSERT_TRUE(timeStore.Stage(Completion(Binding(17), JumpgateSubtitleResultStatus::Success)));
  const AndroidJumpgateCleanupReport timeReport = timeStore.SweepStartupOrphans({32, 1024, 5ms});
  EXPECT_TRUE(timeReport.timeLimitHit);
}
