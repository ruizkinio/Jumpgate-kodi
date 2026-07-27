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
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
constexpr auto ORIGIN = "https://bridge.example";
const std::string DEVICE_TOKEN(43, 'T');
const std::string SELECTOR(64, 'a');
const std::string BASE_NAME(64, 'c');
const std::string ARTIFACT = "artifact_00000001";
const std::string TEXT_PAYLOAD = "WEBVTT\n\nA\n";
const std::string TEXT_SHA256 = "56db31420cc3d2ec7c1fb467c036051bf463c64fe95f07a5536317476a1afc72";

bool EndsWith(const std::string& value, const std::string& suffix)
{
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::uint8_t> Bytes(const std::string& value)
{
  return {value.begin(), value.end()};
}

JumpgateSubtitleBinding Binding(std::uint64_t generation, std::string session = "session_00000001")
{
  return {generation, "profile_00000001", "device_00000001", ORIGIN, std::move(session)};
}

JumpgateSubtitleRequest Request(const JumpgateSubtitleBinding& binding)
{
  return {binding, CJumpgateSubtitleBearerAuthority{DEVICE_TOKEN}, {"en"}};
}

JumpgateSubtitleCompletion SuccessfulCompletion(const JumpgateSubtitleBinding& binding,
                                                bool vobSub = false)
{
  JumpgateSubtitleCompletion completion;
  completion.binding = binding;
  completion.status = JumpgateSubtitleResultStatus::Success;
  completion.httpStatus = 200;
  completion.artifact.selected = {SELECTOR, "English", "en", vobSub ? "vobsub" : "vtt", 1};
  if (vobSub)
  {
    completion.artifact.parts.push_back({1, "index", "application/x-vobsub", BASE_NAME + ".idx",
                                         std::string(64, 'd'), Bytes("# VobSub index\n")});
    completion.artifact.parts.push_back({2,
                                         "sub",
                                         "application/octet-stream",
                                         BASE_NAME + ".sub",
                                         std::string(64, 'e'),
                                         {0x00, 0x00, 0x01, 0xba}});
  }
  else
  {
    completion.artifact.parts.push_back(
        {1, "subtitle", "text/vtt", BASE_NAME + ".vtt", std::string(64, 'd'), Bytes(TEXT_PAYLOAD)});
  }
  return completion;
}

class CTemporaryDirectory final
{
public:
  CTemporaryDirectory()
  {
    static std::atomic<std::uint64_t> next{1};
    m_path = std::filesystem::temp_directory_path() /
             ("kodi-jumpgate-controller-test-" + std::to_string(next.fetch_add(1)));
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
    EXPECT_TRUE(std::filesystem::create_directories(m_path));
  }

  ~CTemporaryDirectory()
  {
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
    std::ostringstream value;
    value << std::hex << std::setfill('0') << std::setw(32) << next->fetch_add(1);
    return value.str();
  };
}

class CWorkflowExecutor final : public IAndroidJumpgateSubtitleHttpExecutor
{
public:
  bool Execute(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    if (cancellation.IsCancelled())
      return false;
    response.statusCode = 200;
    response.effectiveUrl = request.url;
    if (EndsWith(request.url, "/v1/subtitles/discover"))
    {
      std::unique_lock lock(m_mutex);
      ++m_discoverCalls;
      m_condition.notify_all();
      if (m_blockDiscoverAt > 0 && m_discoverCalls >= m_blockDiscoverAt)
      {
        m_discoverBlocked = true;
        m_condition.notify_all();
        m_condition.wait(lock, [&] { return m_releaseDiscover; });
      }
      response.contentType = "application/json; charset=utf-8";
      response.body = Bytes("{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR +
                            "\",\"language\":\"en\",\"format\":\"vtt\",\"label\":"
                            "\"English - VTT\",\"rank\":1}]}");
      return true;
    }
    if (EndsWith(request.url, "/v1/subtitles/resolve"))
    {
      response.contentType = "application/json; charset=utf-8";
      const std::string fileName = BASE_NAME + ".vtt";
      response.body = Bytes(
          "{\"schemaVersion\":2,\"status\":\"ready\",\"artifactId\":\"" + ARTIFACT +
          "\",\"expiresAt\":4102444800000,\"expiresAtUnit\":\"unix_ms\",\"parts\":[{"
          "\"partNumber\":1,\"role\":\"subtitle\",\"contentLength\":" +
          std::to_string(TEXT_PAYLOAD.size()) + ",\"contentType\":\"text/vtt\",\"fileName\":\"" +
          fileName + "\",\"path\":\"/v1/subtitles/session_00000001/" + ARTIFACT + "/1/" + fileName +
          "\",\"sha256\":\"" + TEXT_SHA256 + "\"}]}");
      return true;
    }
    response.contentType = "text/vtt";
    response.body = Bytes(TEXT_PAYLOAD);
    response.contentLength = response.body.size();
    response.contentEncoding = "identity";
    response.acceptRanges = "none";
    return true;
  }

  bool WaitForDiscover(int count)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [&] { return m_discoverCalls >= count; });
  }

  void BlockDiscoverAt(int count)
  {
    std::lock_guard lock(m_mutex);
    m_blockDiscoverAt = count;
  }

  bool WaitForBlockedDiscover()
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [&] { return m_discoverBlocked; });
  }

  void ReleaseDiscover()
  {
    std::lock_guard lock(m_mutex);
    m_releaseDiscover = true;
    m_condition.notify_all();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
  int m_discoverCalls{0};
  int m_blockDiscoverAt{0};
  bool m_discoverBlocked{false};
  bool m_releaseDiscover{false};
};

class CBlockingExecutor final : public IAndroidJumpgateSubtitleHttpExecutor
{
public:
  explicit CBlockingExecutor(bool safeCancellation) : m_safeCancellation(safeCancellation) {}

  bool Execute(const JumpgateSubtitleHttpRequest&,
               JumpgateSubtitleHttpResponse&,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    std::unique_lock lock(m_mutex);
    m_executeThread = std::this_thread::get_id();
    m_entered = true;
    m_condition.notify_all();
    while (!m_released && (m_safeCancellation || !cancellation.IsCancelled()))
      m_condition.wait_for(lock, 1ms);
    return false;
  }

  bool SupportsSafeConcurrentCancellation() const noexcept override { return m_safeCancellation; }

  void RequestSafeCancellation() override
  {
    std::lock_guard lock(m_mutex);
    ++m_safeCancellationCalls;
    m_released = true;
    m_condition.notify_all();
  }

  bool WaitUntilEntered()
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [&] { return m_entered; });
  }

  void Release()
  {
    std::lock_guard lock(m_mutex);
    m_released = true;
    m_condition.notify_all();
  }

  int SafeCancellationCalls() const
  {
    std::lock_guard lock(m_mutex);
    return m_safeCancellationCalls;
  }

  std::thread::id ExecuteThread() const
  {
    std::lock_guard lock(m_mutex);
    return m_executeThread;
  }

private:
  bool m_safeCancellation{false};
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_entered{false};
  bool m_released{false};
  int m_safeCancellationCalls{0};
  std::thread::id m_executeThread;
};

class CFixedBodyExecutor final : public IAndroidJumpgateSubtitleHttpExecutor
{
public:
  explicit CFixedBodyExecutor(std::size_t size) : m_size(size) {}

  bool Execute(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken&) override
  {
    response.statusCode = 200;
    response.effectiveUrl = request.url;
    response.contentType = "text/vtt";
    response.contentLength = m_size;
    response.body.assign(m_size, 0x41);
    return true;
  }

private:
  std::size_t m_size{0};
};

class CTestArtifactAnchor final : public IAndroidJumpgateSubtitleArtifactAnchor
{
public:
  CTestArtifactAnchor(std::string identity, std::vector<std::string> files)
    : m_identity(std::move(identity)),
      m_files(files.begin(), files.end())
  {
  }

  bool Validate() const override
  {
    std::lock_guard lock(m_mutex);
    return m_replacedFiles.empty();
  }

  std::string InjectionPath(const std::string& fileName) const override
  {
    std::lock_guard lock(m_mutex);
    if (!m_files.contains(fileName))
      return {};
    return "test://pinned/" + m_identity + "/" + fileName;
  }

  void ReplaceDirectoryPath()
  {
    std::lock_guard lock(m_mutex);
    m_directoryPathReplaced = true;
  }

  void ReplaceFile(const std::string& fileName)
  {
    std::lock_guard lock(m_mutex);
    if (m_files.contains(fileName))
      m_replacedFiles.emplace(fileName);
  }

private:
  std::string m_identity;
  std::unordered_set<std::string> m_files;
  mutable std::mutex m_mutex;
  std::unordered_set<std::string> m_replacedFiles;
  bool m_directoryPathReplaced{false};
};

class CTestSubtitleFileStore final : public IAndroidJumpgateSubtitleFileStore
{
public:
  std::optional<AndroidJumpgateStagedArtifact> Stage(
      const JumpgateSubtitleCompletion& completion,
      const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    {
      std::unique_lock lock(m_mutex);
      ++m_stageCalls;
      m_stageEntered = true;
      m_condition.notify_all();
      m_condition.wait(lock, [&] { return !m_blockStage || m_releaseStage; });
    }
    if (cancellation.IsCancelled() || completion.status != JumpgateSubtitleResultStatus::Success ||
        completion.artifact.parts.empty())
    {
      return std::nullopt;
    }

    std::vector<std::string> files;
    std::string injectionFile;
    if (completion.artifact.parts.size() == 2)
    {
      injectionFile = "jumpgate." + completion.artifact.selected.language + ".idx";
      files = {injectionFile, "jumpgate." + completion.artifact.selected.language + ".sub"};
    }
    else
    {
      const std::string& source = completion.artifact.parts.front().fileName;
      const std::size_t dot = source.rfind('.');
      injectionFile = "jumpgate." + completion.artifact.selected.language +
                      (dot == std::string::npos ? ".srt" : source.substr(dot));
      files = {injectionFile};
    }

    std::shared_ptr<CTestArtifactAnchor> anchor;
    std::string directory;
    {
      std::lock_guard lock(m_mutex);
      directory = "test-directory-" + std::to_string(completion.binding.generation) + "-" +
                  std::to_string(++m_nextIdentity);
      anchor = std::make_shared<CTestArtifactAnchor>(directory, std::move(files));
      m_latestAnchor = anchor;
    }
    return AndroidJumpgateStagedArtifact{completion.binding,
                                         directory,
                                         injectionFile,
                                         anchor->InjectionPath(injectionFile),
                                         completion.artifact.selected.language,
                                         completion.artifact.selected.format,
                                         std::move(anchor)};
  }

  AndroidJumpgateCleanupReport RemoveGenerationDirectory(const std::string& directory) override
  {
    std::lock_guard lock(m_mutex);
    m_cleanedDirectories.emplace_back(directory);
    m_condition.notify_all();
    return {};
  }

  AndroidJumpgateCleanupReport SweepStartupOrphans(AndroidJumpgateCleanupBudget) override
  {
    std::lock_guard lock(m_mutex);
    ++m_sweeps;
    return {};
  }

  void BlockStage()
  {
    std::lock_guard lock(m_mutex);
    m_blockStage = true;
    m_releaseStage = false;
    m_stageEntered = false;
  }

  bool WaitForStage(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [&] { return m_stageEntered; });
  }

  void ReleaseStage()
  {
    std::lock_guard lock(m_mutex);
    m_releaseStage = true;
    m_blockStage = false;
    m_condition.notify_all();
  }

  std::shared_ptr<CTestArtifactAnchor> LatestAnchor() const
  {
    std::lock_guard lock(m_mutex);
    return m_latestAnchor;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<std::string> m_cleanedDirectories;
  std::shared_ptr<CTestArtifactAnchor> m_latestAnchor;
  std::uint64_t m_nextIdentity{0};
  std::size_t m_stageCalls{0};
  std::size_t m_sweeps{0};
  bool m_blockStage{false};
  bool m_releaseStage{false};
  bool m_stageEntered{false};
};

class CAnchorAwareCleanupStore final : public IAndroidJumpgateSubtitleFileStore
{
public:
  explicit CAnchorAwareCleanupStore(bool blockStage = false, bool blockFirstInUse = false)
    : m_blockStage(blockStage),
      m_blockFirstInUse(blockFirstInUse)
  {
  }

  std::optional<AndroidJumpgateStagedArtifact> Stage(
      const JumpgateSubtitleCompletion& completion,
      const CJumpgateSubtitleCancellationToken&) override
  {
    if (completion.status != JumpgateSubtitleResultStatus::Success ||
        completion.artifact.parts.empty())
    {
      return std::nullopt;
    }

    const std::string directory =
        "anchor-aware-directory-" + std::to_string(completion.binding.generation);
    const std::string injectionFile = "jumpgate.en.vtt";
    auto anchor =
        std::make_shared<CTestArtifactAnchor>(directory, std::vector<std::string>{injectionFile});
    {
      std::unique_lock lock(m_mutex);
      m_latestAnchor = anchor;
      m_directory = directory;
      m_stageEntered = true;
      m_condition.notify_all();
      m_condition.wait(lock, [this] { return !m_blockStage || m_releaseStage; });
    }
    return AndroidJumpgateStagedArtifact{completion.binding,
                                         directory,
                                         injectionFile,
                                         anchor->InjectionPath(injectionFile),
                                         completion.artifact.selected.language,
                                         completion.artifact.selected.format,
                                         std::move(anchor)};
  }

  AndroidJumpgateCleanupReport RemoveGenerationDirectory(const std::string& directory) override
  {
    std::unique_lock lock(m_mutex);
    ++m_cleanupAttempts;
    AndroidJumpgateCleanupReport report;
    if (directory != m_directory)
    {
      report.containmentRejected = true;
      return report;
    }
    if (!m_latestAnchor.expired())
    {
      report.inUse = true;
      ++m_inUseAttempts;
      m_condition.notify_all();
      if (m_blockFirstInUse && m_inUseAttempts == 1)
        m_condition.wait(lock, [this] { return m_releaseFirstInUse; });
      return report;
    }
    m_removed = true;
    m_condition.notify_all();
    return report;
  }

  AndroidJumpgateCleanupReport SweepStartupOrphans(AndroidJumpgateCleanupBudget) override
  {
    std::lock_guard lock(m_mutex);
    ++m_sweeps;
    return {};
  }

  bool WaitForStage(std::chrono::milliseconds timeout = 1s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_stageEntered; });
  }

  void ReleaseStage()
  {
    std::lock_guard lock(m_mutex);
    m_releaseStage = true;
    m_condition.notify_all();
  }

  std::shared_ptr<CTestArtifactAnchor> LatestAnchor() const
  {
    std::lock_guard lock(m_mutex);
    return m_latestAnchor.lock();
  }

  bool WaitForInUseAttempt(std::chrono::milliseconds timeout = 1s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_inUseAttempts > 0; });
  }

  void ReleaseFirstInUse()
  {
    std::lock_guard lock(m_mutex);
    m_releaseFirstInUse = true;
    m_condition.notify_all();
  }

  bool WaitForRemoval(std::chrono::milliseconds timeout = 1s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_removed; });
  }

  bool Removed() const
  {
    std::lock_guard lock(m_mutex);
    return m_removed;
  }

  std::size_t CleanupAttempts() const
  {
    std::lock_guard lock(m_mutex);
    return m_cleanupAttempts;
  }

  std::size_t InUseAttempts() const
  {
    std::lock_guard lock(m_mutex);
    return m_inUseAttempts;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::weak_ptr<CTestArtifactAnchor> m_latestAnchor;
  std::string m_directory;
  std::size_t m_cleanupAttempts{0};
  std::size_t m_inUseAttempts{0};
  std::size_t m_sweeps{0};
  bool m_blockStage{false};
  bool m_blockFirstInUse{false};
  bool m_stageEntered{false};
  bool m_releaseStage{false};
  bool m_releaseFirstInUse{false};
  bool m_removed{false};
};

void PopulateDeliveryRequest(JumpgateSubtitleHttpRequest& request, std::size_t maximumBytes)
{
  request.method = JumpgateSubtitleHttpMethod::Get;
  request.url = std::string{ORIGIN} + "/v1/subtitles/session_00000001/" + ARTIFACT + "/1/" +
                BASE_NAME + ".vtt";
  request.authorization = "Bearer " + DEVICE_TOKEN;
  request.maximumResponseBytes = maximumBytes;
}

AndroidJumpgateSubtitleControllerDependencies ControllerDependencies(
    const std::shared_ptr<IAndroidJumpgateSubtitleFileStore>& store,
    const std::shared_ptr<CJumpgateThreadRegistry>& registry,
    const std::shared_ptr<IAndroidJumpgateSubtitleHttpExecutor>& executor,
    std::vector<std::string>& injections)
{
  AndroidJumpgateSubtitleControllerDependencies dependencies;
  dependencies.fileStore = store;
  dependencies.registry = registry;
  dependencies.transportFactory = [executor](const std::string& origin)
  { return std::make_shared<CAndroidJumpgateSubtitleTransport>(origin, executor); };
  dependencies.subtitleInjector = [&injections](const std::string& path)
  { injections.emplace_back(path); };
  dependencies.rePairNotifier = [] {};
  dependencies.failureLogger = [](JumpgateSubtitleResultStatus, int) {};
  return dependencies;
}

bool PumpUntilInjected(CAndroidJumpgateSubtitleController& controller,
                       const JumpgateSubtitleBinding& binding,
                       const std::vector<std::string>& injections,
                       std::size_t count)
{
  for (int attempt = 0; attempt < 400; ++attempt)
  {
    controller.Process(binding);
    if (injections.size() >= count)
      return true;
    std::this_thread::sleep_for(5ms);
  }
  return false;
}
} // namespace

TEST(TestAndroidJumpgateSubtitleTransportProduction, RejectsOverflowWithoutRetainingSentinel)
{
  const std::size_t maximum = ANDROID_JUMPGATE_MAX_PART_BYTES;
  auto overflow = std::make_shared<CFixedBodyExecutor>(maximum + 1);
  CAndroidJumpgateSubtitleTransport transport{ORIGIN, overflow};
  JumpgateSubtitleHttpRequest request;
  PopulateDeliveryRequest(request, maximum);
  JumpgateSubtitleHttpResponse response;
  EXPECT_FALSE(transport.Perform(request, response, {}));
  EXPECT_TRUE(response.body.empty());

  auto exact = std::make_shared<CFixedBodyExecutor>(maximum);
  CAndroidJumpgateSubtitleTransport exactTransport{ORIGIN, exact};
  EXPECT_TRUE(exactTransport.Perform(request, response, {}));
  EXPECT_EQ(response.body.size(), maximum);
}

TEST(TestAndroidJumpgateSubtitleTransportProduction, DefaultCurlExecutorOptsIntoSafeCancellation)
{
  CAndroidJumpgateSubtitleTransport transport{ORIGIN};
  EXPECT_TRUE(transport.RequestSafeCancellation());
}

TEST(TestAndroidJumpgateSubtitleFileStoreProduction, WindowsFailsClosedWithoutFilesystemMutation)
{
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-specific fail-closed contract";
#else
  CTemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "production-root";
  CAndroidJumpgateSubtitleFileStore store(root, SequentialTokens());
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_FALSE(store.Stage(SuccessfulCompletion(Binding(90))));
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_TRUE(
      store.RemoveGenerationDirectory((root / "jg-90-00000000000000000000000000000001").string())
          .containmentRejected);
  EXPECT_TRUE(store.SweepStartupOrphans({16, 1024, 10ms}).containmentRejected);
  EXPECT_FALSE(std::filesystem::exists(root));
#endif
}

TEST(TestAndroidJumpgateSubtitleFileStoreProduction, FakeAnchorPinsDirectoryAndRejectsFileMutation)
{
  auto store = std::make_shared<CTestSubtitleFileStore>();
  const auto staged = store->Stage(SuccessfulCompletion(Binding(91)), {});
  ASSERT_TRUE(staged);
  auto anchor = store->LatestAnchor();
  ASSERT_TRUE(anchor);
  const std::string pinnedPath = anchor->InjectionPath(staged->injectionFileName);
  anchor->ReplaceDirectoryPath();
  EXPECT_TRUE(anchor->Validate());
  EXPECT_EQ(anchor->InjectionPath(staged->injectionFileName), pinnedPath);
  anchor->ReplaceFile(staged->injectionFileName);
  EXPECT_FALSE(anchor->Validate());
}

TEST(TestAndroidJumpgateSubtitleFileStoreProduction, FakeVobSubPairUsesOnePinnedDirectory)
{
  auto store = std::make_shared<CTestSubtitleFileStore>();
  const auto staged = store->Stage(SuccessfulCompletion(Binding(92), true), {});
  ASSERT_TRUE(staged);
  ASSERT_EQ(staged->injectionFileName, "jumpgate.en.idx");
  auto anchor = store->LatestAnchor();
  ASSERT_TRUE(anchor);
  const std::string indexPath = anchor->InjectionPath("jumpgate.en.idx");
  const std::string subPath = anchor->InjectionPath("jumpgate.en.sub");
  ASSERT_FALSE(indexPath.empty());
  ASSERT_FALSE(subPath.empty());
  EXPECT_EQ(indexPath.substr(0, indexPath.rfind('/')), subPath.substr(0, subPath.rfind('/')));
  EXPECT_TRUE(anchor->Validate());
  anchor->ReplaceFile("jumpgate.en.sub");
  EXPECT_FALSE(anchor->Validate());
}

TEST(TestAndroidJumpgateSubtitleFileStoreProduction,
     CancelledStaleStageReleasesAnchorBeforeBoundedCleanup)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CAnchorAwareCleanupStore>(true);
  CAndroidJumpgateSubtitleStageWorker worker(store, registry);
  const JumpgateSubtitleBinding binding = Binding(102);
  ASSERT_TRUE(worker.Queue(SuccessfulCompletion(binding)));
  ASSERT_TRUE(store->WaitForStage());
  ASSERT_TRUE(worker.Cancel(binding));

  const auto started = std::chrono::steady_clock::now();
  store->ReleaseStage();
  ASSERT_TRUE(store->WaitForRemoval(750ms));
  ASSERT_TRUE(worker.WaitForCleanupIdle(750ms));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_LT(elapsed, 750ms);
  EXPECT_EQ(store->InUseAttempts(), 0U);
  EXPECT_EQ(store->CleanupAttempts(), 1U);
  EXPECT_EQ(worker.PendingCleanupCount(), 0U);
  EXPECT_TRUE(worker.Stop(1s));
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleTransportProduction, UnsafeExecutorIsNeverCancelledConcurrently)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CBlockingExecutor>(false);
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};
  const JumpgateSubtitleBinding binding = Binding(1);
  ASSERT_TRUE(controller.PrepareGeneration(1));
  ASSERT_TRUE(controller.Queue(Request(binding)));
  ASSERT_TRUE(executor->WaitUntilEntered());

  controller.Stop(false, false, 0ms);
  EXPECT_EQ(executor->SafeCancellationCalls(), 0);
  EXPECT_NE(executor->ExecuteThread(), std::this_thread::get_id());
  EXPECT_TRUE(registry->JoinAllFor(2s));
}

TEST(TestAndroidJumpgateSubtitleTransportProduction, FutureSafeCancellationContractIsConsumed)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CBlockingExecutor>(true);
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};
  const JumpgateSubtitleBinding binding = Binding(2);
  ASSERT_TRUE(controller.PrepareGeneration(2));
  ASSERT_TRUE(controller.Queue(Request(binding)));
  ASSERT_TRUE(executor->WaitUntilEntered());

  controller.Stop(false, true, 1s);
  EXPECT_GE(executor->SafeCancellationCalls(), 1);
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction, ProfileStopRestartRequeuesAndInjects)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};

  const JumpgateSubtitleBinding first = Binding(3);
  ASSERT_TRUE(controller.PrepareGeneration(first.generation));
  ASSERT_TRUE(controller.Queue(Request(first)));
  controller.MarkPlaybackReady(first.generation);
  ASSERT_TRUE(PumpUntilInjected(controller, first, injections, 1));

  controller.Stop(false, true, 1s);
  EXPECT_FALSE(controller.PrepareGeneration(4));
  ASSERT_TRUE(controller.Restart(1s));
  const JumpgateSubtitleBinding second = Binding(4);
  ASSERT_TRUE(controller.PrepareGeneration(second.generation));
  ASSERT_TRUE(controller.Queue(Request(second)));
  controller.MarkPlaybackReady(second.generation);
  ASSERT_TRUE(PumpUntilInjected(controller, second, injections, 2));
  EXPECT_NE(injections[0], injections[1]);
  controller.OnPlaybackTerminal(second.generation);
  controller.Stop(false, true, 1s);
}

TEST(TestAndroidJumpgateSubtitleControllerProduction,
     RevalidatesPinnedIdentityImmediatelyBeforeInjection)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};

  const JumpgateSubtitleBinding directoryReplacement = Binding(93);
  ASSERT_TRUE(controller.PrepareGeneration(directoryReplacement.generation));
  ASSERT_TRUE(controller.Queue(Request(directoryReplacement)));
  for (int attempt = 0; attempt < 400 && !store->LatestAnchor(); ++attempt)
  {
    controller.Process(directoryReplacement);
    std::this_thread::sleep_for(2ms);
  }
  auto firstAnchor = store->LatestAnchor();
  ASSERT_TRUE(firstAnchor);
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    controller.Process(directoryReplacement);
    std::this_thread::sleep_for(1ms);
  }
  firstAnchor->ReplaceDirectoryPath();
  controller.MarkPlaybackReady(directoryReplacement.generation);
  ASSERT_TRUE(PumpUntilInjected(controller, directoryReplacement, injections, 1));
  controller.OnPlaybackTerminal(directoryReplacement.generation);

  const JumpgateSubtitleBinding fileReplacement = Binding(94);
  ASSERT_TRUE(controller.PrepareGeneration(fileReplacement.generation));
  ASSERT_TRUE(controller.Queue(Request(fileReplacement)));
  std::shared_ptr<CTestArtifactAnchor> secondAnchor;
  for (int attempt = 0; attempt < 400; ++attempt)
  {
    controller.Process(fileReplacement);
    secondAnchor = store->LatestAnchor();
    if (secondAnchor && secondAnchor != firstAnchor)
      break;
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(secondAnchor);
  ASSERT_NE(secondAnchor, firstAnchor);
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    controller.Process(fileReplacement);
    std::this_thread::sleep_for(1ms);
  }
  secondAnchor->ReplaceFile("jumpgate.en.vtt");
  controller.MarkPlaybackReady(fileReplacement.generation);
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    controller.Process(fileReplacement);
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(injections.size(), 1U);
  controller.Stop(false, true, 1s);
}

TEST(TestAndroidJumpgateSubtitleControllerProduction,
     PostInjectionTerminalRetriesUntilLegitimateAnchorReleases)
{
  struct InjectionGate
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool release{false};
  };
  auto gate = std::make_shared<InjectionGate>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CAnchorAwareCleanupStore>(false, true);
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  std::shared_ptr<CTestArtifactAnchor> retainedAnchor;
  auto dependencies = ControllerDependencies(store, registry, executor, injections);
  dependencies.subtitleInjector = [store, gate, &retainedAnchor](const std::string&)
  {
    std::unique_lock lock(gate->mutex);
    retainedAnchor = store->LatestAnchor();
    gate->entered = true;
    gate->condition.notify_all();
    gate->condition.wait(lock, [&] { return gate->release; });
  };
  CAndroidJumpgateSubtitleController controller{std::move(dependencies)};
  const JumpgateSubtitleBinding binding = Binding(103);
  ASSERT_TRUE(controller.PrepareGeneration(binding.generation));
  ASSERT_TRUE(controller.Queue(Request(binding)));
  controller.MarkPlaybackReady(binding.generation);

  std::thread process(
      [&]
      {
        for (int attempt = 0; attempt < 500; ++attempt)
        {
          controller.Process(binding);
          {
            std::lock_guard lock(gate->mutex);
            if (gate->entered)
              return;
          }
          std::this_thread::sleep_for(2ms);
        }
      });
  bool injectorEntered = false;
  {
    std::unique_lock lock(gate->mutex);
    injectorEntered = gate->condition.wait_for(lock, 2s, [&] { return gate->entered; });
  }

  std::atomic_bool terminalStarted{false};
  std::atomic_bool terminalFinished{false};
  std::thread terminal;
  if (injectorEntered)
  {
    terminal = std::thread(
        [&]
        {
          terminalStarted.store(true);
          controller.OnPlaybackTerminal(binding.generation);
          terminalFinished.store(true);
        });
    for (int attempt = 0; attempt < 500 && !terminalStarted.load(); ++attempt)
      std::this_thread::sleep_for(1ms);
  }
  EXPECT_FALSE(terminalFinished.load());
  EXPECT_FALSE(store->Removed());
  {
    std::lock_guard lock(gate->mutex);
    gate->release = true;
    gate->condition.notify_all();
  }
  process.join();
  if (terminal.joinable())
    terminal.join();

  const auto cleanupStarted = std::chrono::steady_clock::now();
  const bool observedInUse = store->WaitForInUseAttempt(750ms);
  const bool retainedDuringCleanup = retainedAnchor && !store->Removed();
  const std::size_t pendingWhileRetained = controller.PendingCleanupCount();
  retainedAnchor.reset();
  store->ReleaseFirstInUse();
  const bool removed = store->WaitForRemoval(750ms);
  const bool idle = controller.WaitForCleanupIdle(750ms);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - cleanupStarted);

  ASSERT_TRUE(injectorEntered);
  ASSERT_TRUE(observedInUse);
  EXPECT_TRUE(retainedDuringCleanup);
  EXPECT_EQ(pendingWhileRetained, 1U);
  EXPECT_TRUE(removed);
  EXPECT_TRUE(idle);
  EXPECT_LT(elapsed, 750ms);
  EXPECT_GE(store->CleanupAttempts(), 2U);
  EXPECT_LE(store->CleanupAttempts(), 4U);
  EXPECT_EQ(controller.PendingCleanupCount(), 0U);
  controller.Stop(false, true, 1s);
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction,
     ConcurrentRestartProcessAndCleanupUseStableWorkerSnapshots)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};

  const JumpgateSubtitleBinding first = Binding(95);
  ASSERT_TRUE(controller.PrepareGeneration(first.generation));
  ASSERT_TRUE(controller.Queue(Request(first)));
  controller.MarkPlaybackReady(first.generation);
  ASSERT_TRUE(PumpUntilInjected(controller, first, injections, 1));

  store->BlockStage();
  const JumpgateSubtitleBinding blocked = Binding(96);
  ASSERT_TRUE(controller.PrepareGeneration(blocked.generation));
  ASSERT_TRUE(controller.Queue(Request(blocked)));
  for (int attempt = 0; attempt < 400; ++attempt)
  {
    controller.Process(blocked);
    if (store->WaitForStage(10ms))
      break;
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(store->WaitForStage());

  std::atomic_bool restartResult{false};
  std::thread process([&] { controller.Process(blocked); });
  std::thread cleanup([&] { controller.OnPlaybackTerminal(first.generation); });
  std::thread restart([&] { restartResult.store(controller.Restart(2s)); });
  std::this_thread::sleep_for(20ms);
  store->ReleaseStage();
  process.join();
  cleanup.join();
  restart.join();
  ASSERT_TRUE(restartResult.load());

  const JumpgateSubtitleBinding replacement = Binding(97);
  ASSERT_TRUE(controller.PrepareGeneration(replacement.generation));
  ASSERT_TRUE(controller.Queue(Request(replacement)));
  controller.MarkPlaybackReady(replacement.generation);
  ASSERT_TRUE(PumpUntilInjected(controller, replacement, injections, 2));
  controller.OnPlaybackTerminal(replacement.generation);
  controller.Stop(false, true, 1s);
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction,
     OlderBlockedRestartCannotEraseNewerPublishedGeneration)
{
  struct RestartBarrier
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool reached{false};
    bool release{false};
  };
  auto barrier = std::make_shared<RestartBarrier>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  auto dependencies = ControllerDependencies(store, registry, executor, injections);
  dependencies.restartTransitionBarrier = [barrier](std::uint64_t)
  {
    std::unique_lock lock(barrier->mutex);
    barrier->reached = true;
    barrier->condition.notify_all();
    barrier->condition.wait(lock, [&] { return barrier->release; });
  };
  CAndroidJumpgateSubtitleController controller{std::move(dependencies)};

  store->BlockStage();
  const JumpgateSubtitleBinding blocked = Binding(100);
  ASSERT_TRUE(controller.PrepareGeneration(blocked.generation));
  ASSERT_TRUE(controller.Queue(Request(blocked)));
  for (int attempt = 0; attempt < 400 && !store->WaitForStage(5ms); ++attempt)
    controller.Process(blocked);
  ASSERT_TRUE(store->WaitForStage());

  std::atomic_bool olderResult{true};
  std::thread olderRestart([&] { olderResult.store(controller.Restart(2s)); });
  bool barrierReached = false;
  {
    std::unique_lock lock(barrier->mutex);
    barrierReached = barrier->condition.wait_for(lock, 1s, [&] { return barrier->reached; });
  }

  const bool newerRestarted = barrierReached && controller.Restart(1s);
  const JumpgateSubtitleBinding replacement = Binding(101);
  const bool replacementPrepared =
      newerRestarted && controller.PrepareGeneration(replacement.generation);
  const bool replacementQueued = replacementPrepared && controller.Queue(Request(replacement));
  if (replacementQueued)
    controller.MarkPlaybackReady(replacement.generation);

  {
    std::lock_guard lock(barrier->mutex);
    barrier->release = true;
    barrier->condition.notify_all();
  }
  store->ReleaseStage();
  olderRestart.join();
  ASSERT_TRUE(barrierReached);
  ASSERT_TRUE(newerRestarted);
  ASSERT_TRUE(replacementPrepared);
  ASSERT_TRUE(replacementQueued);
  EXPECT_FALSE(olderResult.load());
  ASSERT_TRUE(PumpUntilInjected(controller, replacement, injections, 1));
  controller.OnPlaybackTerminal(replacement.generation);
  controller.Stop(false, true, 1s);
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction, LaterStopPreventsRestartWorkerPublication)
{
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto store = std::make_shared<CTestSubtitleFileStore>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};

  store->BlockStage();
  const JumpgateSubtitleBinding binding = Binding(98);
  ASSERT_TRUE(controller.PrepareGeneration(binding.generation));
  ASSERT_TRUE(controller.Queue(Request(binding)));
  for (int attempt = 0; attempt < 400; ++attempt)
  {
    controller.Process(binding);
    if (store->WaitForStage(10ms))
      break;
  }
  ASSERT_TRUE(store->WaitForStage());

  std::atomic_bool restartResult{true};
  std::thread restart([&] { restartResult.store(controller.Restart(2s)); });
  std::this_thread::sleep_for(20ms);
  controller.Stop(false, false, 0ms);
  store->ReleaseStage();
  restart.join();

  EXPECT_FALSE(restartResult.load());
  EXPECT_FALSE(controller.PrepareGeneration(99));
  EXPECT_FALSE(controller.Queue(Request(Binding(99))));
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction, ReplacementCannotInjectStaleArtifact)
{
  auto store = std::make_shared<CTestSubtitleFileStore>();
  store->BlockStage();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};
  const JumpgateSubtitleBinding first = Binding(5);
  ASSERT_TRUE(controller.PrepareGeneration(first.generation));
  ASSERT_TRUE(controller.Queue(Request(first)));
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    controller.Process(first);
    if (store->WaitForStage(10ms))
      break;
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(store->WaitForStage());

  const JumpgateSubtitleBinding replacement = Binding(6, "session_00000002");
  ASSERT_TRUE(controller.PrepareGeneration(replacement.generation));
  controller.MarkPlaybackReady(replacement.generation);
  store->ReleaseStage();
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    controller.Process(first);
    controller.Process(replacement);
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_TRUE(injections.empty());
  controller.Stop(false, true, 1s);
  EXPECT_TRUE(registry->JoinAllFor(1s));
}

TEST(TestAndroidJumpgateSubtitleControllerProduction, StopUsesOneTotalDeadlineAcrossWorkers)
{
  auto store = std::make_shared<CTestSubtitleFileStore>();
  store->BlockStage();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  auto executor = std::make_shared<CWorkflowExecutor>();
  std::vector<std::string> injections;
  CAndroidJumpgateSubtitleController controller{
      ControllerDependencies(store, registry, executor, injections)};

  const JumpgateSubtitleBinding first = Binding(7);
  ASSERT_TRUE(controller.PrepareGeneration(first.generation));
  ASSERT_TRUE(controller.Queue(Request(first)));
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    controller.Process(first);
    if (store->WaitForStage(10ms))
      break;
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(store->WaitForStage());

  executor->BlockDiscoverAt(2);
  const JumpgateSubtitleBinding second = Binding(8);
  ASSERT_TRUE(controller.PrepareGeneration(second.generation));
  ASSERT_TRUE(controller.Queue(Request(second)));
  ASSERT_TRUE(executor->WaitForBlockedDiscover());

  const auto started = std::chrono::steady_clock::now();
  controller.Stop(false, true, 120ms);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_LT(elapsed, 220ms);
  EXPECT_GE(registry->Pending(), 2U);

  executor->ReleaseDiscover();
  store->ReleaseStage();
  EXPECT_TRUE(registry->JoinAllFor(2s));
}
