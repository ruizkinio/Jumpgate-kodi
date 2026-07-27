/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/JumpgateSubtitleCoordinator.h"
#include "utils/JumpgateThreadRegistry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace KODI::JUMPGATE
{

inline constexpr std::size_t ANDROID_JUMPGATE_MAX_PART_BYTES = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t ANDROID_JUMPGATE_MAX_ARTIFACT_BYTES = 12ULL * 1024ULL * 1024ULL;

class IAndroidJumpgateSubtitleArtifactAnchor
{
public:
  virtual ~IAndroidJumpgateSubtitleArtifactAnchor() = default;

  // Anchors prevent this component's asynchronous cleanup from invalidating a delayed player
  // open. Hostile same-UID or in-process code can directly mutate player/process state and is
  // outside this boundary.
  virtual bool Validate() const = 0;
  virtual std::string InjectionPath(const std::string& fileName) const = 0;
};

struct AndroidJumpgateStagedArtifact
{
  JumpgateSubtitleBinding binding;
  std::string directory;
  std::string injectionFileName;
  std::string injectionPath;
  std::string language;
  std::string format;
  std::shared_ptr<IAndroidJumpgateSubtitleArtifactAnchor> anchor;
};

struct AndroidJumpgateStageCompletion
{
  JumpgateSubtitleBinding binding;
  std::optional<AndroidJumpgateStagedArtifact> artifact;
};

struct AndroidJumpgateCleanupBudget
{
  std::size_t maximumEntries{0};
  std::uintmax_t maximumBytes{0};
  std::chrono::milliseconds maximumElapsed{0};
};

struct AndroidJumpgateCleanupReport
{
  std::size_t entriesVisited{0};
  std::uintmax_t bytesRemoved{0};
  bool containmentRejected{false};
  bool inUse{false};
  bool entryLimitHit{false};
  bool byteLimitHit{false};
  bool timeLimitHit{false};
};

void SecureClearAndroidJumpgateSubtitleCompletion(JumpgateSubtitleCompletion& completion);
bool IsWithinAndroidJumpgateArtifactCap(const JumpgateSubtitleCompletion& completion);

class CAndroidJumpgateArtifactBudget final
{
public:
  bool Reserve(const std::string& artifactKey,
               const std::string& partKey,
               std::size_t maximumBytes);
  void Reset();

private:
  std::mutex m_mutex;
  std::string m_artifactKey;
  std::unordered_map<std::string, std::size_t> m_parts;
  std::size_t m_reservedBytes{0};
};

class IAndroidJumpgateSubtitleFileStore
{
public:
  virtual ~IAndroidJumpgateSubtitleFileStore() = default;

  virtual std::optional<AndroidJumpgateStagedArtifact> Stage(
      const JumpgateSubtitleCompletion& completion,
      const CJumpgateSubtitleCancellationToken& cancellation = {}) = 0;
  virtual AndroidJumpgateCleanupReport RemoveGenerationDirectory(const std::string& directory) = 0;
  virtual AndroidJumpgateCleanupReport SweepStartupOrphans(AndroidJumpgateCleanupBudget budget = {
                                                               128, 32ULL * 1024ULL * 1024ULL,
                                                               std::chrono::milliseconds{50}}) = 0;
};

class CAndroidJumpgateSubtitleFileStore final : public IAndroidJumpgateSubtitleFileStore
{
public:
  using TokenGenerator = std::function<std::string()>;
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  using PublishObserver = std::function<void(const std::filesystem::path&)>;
  using SecurityObserver = std::function<void(const char*, const std::filesystem::path&)>;

  explicit CAndroidJumpgateSubtitleFileStore(std::filesystem::path root,
                                             TokenGenerator tokenGenerator = {},
                                             Clock clock = {},
                                             PublishObserver publishObserver = {},
                                             SecurityObserver securityObserver = {});

  std::optional<AndroidJumpgateStagedArtifact> Stage(
      const JumpgateSubtitleCompletion& completion,
      const CJumpgateSubtitleCancellationToken& cancellation = {}) override;
  AndroidJumpgateCleanupReport RemoveGenerationDirectory(const std::string& directory) override;
  AndroidJumpgateCleanupReport SweepStartupOrphans(AndroidJumpgateCleanupBudget budget = {
                                                       128, 32ULL * 1024ULL * 1024ULL,
                                                       std::chrono::milliseconds{50}}) override;

  const std::filesystem::path& Root() const { return m_root; }

private:
  struct CleanupTracker;

  // Budgets bound local-filesystem entries and bytes. A single stalled kernel/filesystem syscall
  // cannot be preempted here, so special://temp must resolve to local app-private storage.
  bool EnsureRoot();
  bool IsContainedGenerationDirectory(const std::filesystem::path& directory) const;
  AndroidJumpgateCleanupReport RemoveGenerationDirectory(const std::filesystem::path& directory,
                                                         CleanupTracker& tracker);
#if !defined(_WIN32)
  AndroidJumpgateCleanupReport RemoveGenerationDirectoryAt(int rootFd,
                                                           const std::string& directoryName,
                                                           CleanupTracker& tracker);
#endif

  std::filesystem::path m_root;
  TokenGenerator m_tokenGenerator;
  Clock m_clock;
  PublishObserver m_publishObserver;
  SecurityObserver m_securityObserver;
};

class CAndroidJumpgateSubtitleStageWorker final
{
public:
  using ClearObserver = std::function<void(const JumpgateSubtitleCompletion&)>;

  explicit CAndroidJumpgateSubtitleStageWorker(
      std::shared_ptr<IAndroidJumpgateSubtitleFileStore> fileStore,
      std::shared_ptr<CJumpgateThreadRegistry> registry = CJumpgateThreadRegistry::Global(),
      ClearObserver clearObserver = {});
  ~CAndroidJumpgateSubtitleStageWorker();

  CAndroidJumpgateSubtitleStageWorker(const CAndroidJumpgateSubtitleStageWorker&) = delete;
  CAndroidJumpgateSubtitleStageWorker& operator=(const CAndroidJumpgateSubtitleStageWorker&) =
      delete;

  // True transfers payload ownership to the worker for staging or secure discard.
  bool Queue(JumpgateSubtitleCompletion&& completion);
  bool Cancel(const JumpgateSubtitleBinding& binding);
  std::optional<AndroidJumpgateStageCompletion> TakeCompletion(
      const JumpgateSubtitleBinding& binding);
  bool QueueCleanup(std::string directory);
  bool QueueStartupSweep();
  bool IsOperational() const;
  std::size_t PendingCleanupCount() const;
  bool WaitForCleanupIdle(std::chrono::milliseconds timeout) const;
  bool Stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});

private:
  struct WorkerState;

  static bool SameBinding(const JumpgateSubtitleBinding& left,
                          const JumpgateSubtitleBinding& right);
  static void Run(const std::shared_ptr<WorkerState>& state);

  mutable std::mutex m_ownerMutex;
  std::shared_ptr<WorkerState> m_state;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  CJumpgateThreadRegistry::Reservation m_registryReservation;
  std::thread m_worker;
};

} // namespace KODI::JUMPGATE
