/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AndroidJumpgateSubtitleFileStore.h"
#include "utils/JumpgateSubtitleCoordinator.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

enum class AndroidJumpgateSubtitleProvider
{
  Standalone,
  OpenSubtitles,
  BridgePending,
  Bridge,
  Disabled,
};

constexpr AndroidJumpgateSubtitleProvider SelectAndroidJumpgateSubtitleProvider(
    bool externalPlayerMode,
    bool sourceBacked,
    bool credentialsValid,
    bool subtitlesEnabled,
    bool authenticatedClaimReady)
{
  if (!externalPlayerMode)
    return AndroidJumpgateSubtitleProvider::Standalone;
  if (!sourceBacked)
    return AndroidJumpgateSubtitleProvider::OpenSubtitles;
  if (!subtitlesEnabled)
    return AndroidJumpgateSubtitleProvider::Disabled;
  if (!credentialsValid || !authenticatedClaimReady)
    return AndroidJumpgateSubtitleProvider::BridgePending;
  return AndroidJumpgateSubtitleProvider::Bridge;
}

enum class AndroidJumpgateSubtitleActionType
{
  None,
  Stage,
  NotifyRePair,
  LogFailure,
};

struct AndroidJumpgateSubtitleAction
{
  AndroidJumpgateSubtitleActionType type{AndroidJumpgateSubtitleActionType::None};
  std::optional<JumpgateSubtitleBinding> binding;
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  int httpStatus{0};
};

// Pure generation state used by the Android controller and host-side lifecycle tests.
class CAndroidJumpgateSubtitleLifecycle final
{
public:
  bool PrepareGeneration(std::uint64_t generation)
  {
    if (generation == 0 || generation <= m_generation)
      return false;

    m_status.reset();
    m_stagedArtifact.reset();
    m_generation = generation;
    m_binding.reset();
    m_playbackReady = false;
    m_rePairNotified = false;
    return true;
  }

  bool Bind(const JumpgateSubtitleBinding& binding)
  {
    if (binding.generation == 0 || binding.generation != m_generation)
      return false;
    if (m_binding)
      return SameBinding(*m_binding, binding);
    m_binding = binding;
    return true;
  }

  bool MarkPlaybackReady(std::uint64_t generation)
  {
    if (generation == 0 || generation != m_generation)
      return false;
    m_playbackReady = true;
    return true;
  }

  bool AcceptStatus(const JumpgateSubtitleBinding& binding,
                    JumpgateSubtitleResultStatus status,
                    int httpStatus)
  {
    if (!m_binding || m_status || !SameBinding(*m_binding, binding) ||
        status == JumpgateSubtitleResultStatus::Success)
      return false;
    m_status = Status{binding, status, httpStatus};
    return true;
  }

  AndroidJumpgateSubtitleAction TakeAction(const JumpgateSubtitleBinding& current)
  {
    if (!m_binding || !SameBinding(*m_binding, current) || !m_status ||
        !SameBinding(m_status->binding, current))
    {
      return {};
    }

    switch (m_status->status)
    {
      case JumpgateSubtitleResultStatus::Success:
        m_status.reset();
        return {};
      case JumpgateSubtitleResultStatus::RePairRequired:
        if (m_rePairNotified)
        {
          m_status.reset();
          return {};
        }
        m_rePairNotified = true;
        return TakeStatus(AndroidJumpgateSubtitleActionType::NotifyRePair);
      case JumpgateSubtitleResultStatus::NoMatch:
      case JumpgateSubtitleResultStatus::Stale:
      case JumpgateSubtitleResultStatus::Cancelled:
        m_status.reset();
        return {};
      default:
        return TakeStatus(AndroidJumpgateSubtitleActionType::LogFailure);
    }
  }

  bool IsCurrent(const JumpgateSubtitleBinding& binding) const
  {
    return m_binding && SameBinding(*m_binding, binding);
  }

  bool IsPlaybackReady(const JumpgateSubtitleBinding& binding) const
  {
    return m_playbackReady && IsCurrent(binding);
  }

  bool AcceptStaged(AndroidJumpgateStagedArtifact&& artifact)
  {
    if (artifact.directory.empty() || artifact.injectionFileName.empty() ||
        artifact.injectionPath.empty() || !artifact.anchor || !IsCurrent(artifact.binding) ||
        m_stagedArtifact || m_stagedDirectories.size() >= MAX_RETAINED_GENERATIONS ||
        m_stagedDirectories.contains(artifact.binding.generation))
    {
      return false;
    }
    m_stagedDirectories.emplace(artifact.binding.generation,
                                StagedDirectory{artifact.directory, artifact.anchor});
    m_stagedArtifact = std::move(artifact);
    return true;
  }

  std::optional<AndroidJumpgateStagedArtifact> TakeInjection(const JumpgateSubtitleBinding& current)
  {
    if (!m_playbackReady || !IsCurrent(current) || !m_stagedArtifact ||
        !SameBinding(m_stagedArtifact->binding, current))
    {
      return std::nullopt;
    }
    std::optional<AndroidJumpgateStagedArtifact> artifact{std::move(*m_stagedArtifact)};
    m_stagedArtifact.reset();
    return artifact;
  }

  std::optional<JumpgateSubtitleBinding> CurrentBinding() const { return m_binding; }

  std::vector<std::string> CommitTerminal(std::uint64_t generation)
  {
    std::vector<std::string> cleanup;
    const auto staged = m_stagedDirectories.find(generation);
    if (staged != m_stagedDirectories.end())
    {
      cleanup.emplace_back(std::move(staged->second.directory));
      m_stagedDirectories.erase(staged);
    }
    if (generation == m_generation)
    {
      m_status.reset();
      m_stagedArtifact.reset();
      m_binding.reset();
      m_playbackReady = false;
    }
    return cleanup;
  }

  std::vector<std::string> Shutdown(bool playerMayRead)
  {
    m_status.reset();
    m_stagedArtifact.reset();
    m_binding.reset();
    m_playbackReady = false;
    m_rePairNotified = false;
    if (playerMayRead)
      return {};

    std::vector<std::string> cleanup;
    cleanup.reserve(m_stagedDirectories.size());
    for (auto& [generation, staged] : m_stagedDirectories)
    {
      static_cast<void>(generation);
      cleanup.emplace_back(std::move(staged.directory));
    }
    m_stagedDirectories.clear();
    return cleanup;
  }

private:
  static constexpr std::size_t MAX_RETAINED_GENERATIONS = 8;

  struct Status
  {
    JumpgateSubtitleBinding binding;
    JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
    int httpStatus{0};
  };

  struct StagedDirectory
  {
    std::string directory;
    std::shared_ptr<IAndroidJumpgateSubtitleArtifactAnchor> anchor;
  };

  static bool SameBinding(const JumpgateSubtitleBinding& left, const JumpgateSubtitleBinding& right)
  {
    return left.generation == right.generation && left.profileId == right.profileId &&
           left.deviceId == right.deviceId && left.bridgeOrigin == right.bridgeOrigin &&
           left.sessionId == right.sessionId;
  }

  AndroidJumpgateSubtitleAction TakeStatus(AndroidJumpgateSubtitleActionType type)
  {
    AndroidJumpgateSubtitleAction action;
    action.type = type;
    action.binding = std::move(m_status->binding);
    action.status = m_status->status;
    action.httpStatus = m_status->httpStatus;
    m_status.reset();
    return action;
  }

  std::uint64_t m_generation{0};
  std::optional<JumpgateSubtitleBinding> m_binding;
  std::optional<Status> m_status;
  std::optional<AndroidJumpgateStagedArtifact> m_stagedArtifact;
  std::map<std::uint64_t, StagedDirectory> m_stagedDirectories;
  bool m_playbackReady{false};
  bool m_rePairNotified{false};
};

// Executors may opt in only when RequestSafeCancellation performs a synchronized, cross-thread
// cancellation operation and never destroys or otherwise mutates the active transport object.
class IAndroidJumpgateSubtitleHttpExecutor
{
public:
  virtual ~IAndroidJumpgateSubtitleHttpExecutor() = default;

  virtual bool Execute(const JumpgateSubtitleHttpRequest& request,
                       JumpgateSubtitleHttpResponse& response,
                       const CJumpgateSubtitleCancellationToken& cancellation) = 0;
  virtual bool SupportsSafeConcurrentCancellation() const noexcept { return false; }
  virtual void RequestSafeCancellation() {}
};

class CAndroidJumpgateSubtitleTransport final : public IJumpgateSubtitleTransport
{
public:
  explicit CAndroidJumpgateSubtitleTransport(
      std::string bridgeOrigin,
      std::shared_ptr<IAndroidJumpgateSubtitleHttpExecutor> executor = {});

  bool Perform(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken& cancellation) override;
  bool RequestSafeCancellation();

private:
  std::string m_bridgeOrigin;
  std::shared_ptr<IAndroidJumpgateSubtitleHttpExecutor> m_executor;
  CAndroidJumpgateArtifactBudget m_artifactBudget;
};

struct AndroidJumpgateSubtitleControllerDependencies
{
  using TransportFactory =
      std::function<std::shared_ptr<CAndroidJumpgateSubtitleTransport>(const std::string&)>;

  std::shared_ptr<IAndroidJumpgateSubtitleFileStore> fileStore;
  std::shared_ptr<CJumpgateThreadRegistry> registry;
  TransportFactory transportFactory;
  std::function<void(const std::string&)> subtitleInjector;
  std::function<void()> rePairNotifier;
  std::function<void(JumpgateSubtitleResultStatus, int)> failureLogger;
  // Optional deterministic transition barrier used by production-linked lifecycle tests.
  std::function<void(std::uint64_t)> restartTransitionBarrier;
};

class CAndroidJumpgateSubtitleController final
{
public:
  CAndroidJumpgateSubtitleController();
  explicit CAndroidJumpgateSubtitleController(
      AndroidJumpgateSubtitleControllerDependencies dependencies);
  ~CAndroidJumpgateSubtitleController();

  CAndroidJumpgateSubtitleController(const CAndroidJumpgateSubtitleController&) = delete;
  CAndroidJumpgateSubtitleController& operator=(const CAndroidJumpgateSubtitleController&) = delete;

  void SweepStartupOrphans();
  bool PrepareGeneration(std::uint64_t generation);
  bool Queue(JumpgateSubtitleRequest request);
  void MarkPlaybackReady(std::uint64_t generation);
  void Process(const JumpgateSubtitleBinding& current);
  void OnPlaybackTerminal(std::uint64_t generation);
  std::size_t PendingCleanupCount() const;
  bool WaitForCleanupIdle(std::chrono::milliseconds timeout) const;
  bool Restart(std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});
  void Stop(bool playerMayRead,
            bool waitForCompletion = true,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3500});

private:
  void QueueCleanup(std::vector<std::string> directories);
  void QueueCleanup(std::vector<std::string> directories,
                    const std::shared_ptr<CAndroidJumpgateSubtitleStageWorker>& worker);

  mutable std::mutex m_mutex;
  CAndroidJumpgateSubtitleLifecycle m_lifecycle;
  std::unique_ptr<CJumpgateSubtitleCoordinator> m_coordinator;
  std::shared_ptr<IAndroidJumpgateSubtitleFileStore> m_fileStore;
  std::shared_ptr<CJumpgateThreadRegistry> m_registry;
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> m_stageWorker;
  AndroidJumpgateSubtitleControllerDependencies::TransportFactory m_transportFactory;
  std::function<void(const std::string&)> m_subtitleInjector;
  std::function<void()> m_rePairNotifier;
  std::function<void(JumpgateSubtitleResultStatus, int)> m_failureLogger;
  std::function<void(std::uint64_t)> m_restartTransitionBarrier;
  std::string m_coordinatorOrigin;
  std::uint64_t m_transitionSerial{0};
  bool m_startupSweepComplete{false};
  bool m_stopped{false};
};

} // namespace KODI::JUMPGATE
