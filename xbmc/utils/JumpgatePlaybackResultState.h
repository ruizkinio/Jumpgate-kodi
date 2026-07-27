/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace KODI::JUMPGATE
{

struct JumpgatePlaybackResult
{
  uint64_t generation{0};
  std::string requestId;
  int64_t positionMs{0};
  int64_t durationMs{0};
  bool completed{false};
};

struct JumpgatePlaybackResultOwner
{
  uint64_t generation{0};
  std::string requestId;
};

class CJumpgatePlaybackResultState final
{
public:
  class LifecycleOperation final
  {
  public:
    LifecycleOperation(LifecycleOperation&&) noexcept = default;
    LifecycleOperation& operator=(LifecycleOperation&&) noexcept = default;

    LifecycleOperation(const LifecycleOperation&) = delete;
    LifecycleOperation& operator=(const LifecycleOperation&) = delete;

    uint64_t Generation() const { return m_generation; }
    bool OwnsGeneration(uint64_t generation) const
    {
      return generation != 0 && generation == m_generation;
    }

  private:
    friend class CJumpgatePlaybackResultState;
    explicit LifecycleOperation(CJumpgatePlaybackResultState& owner);
    LifecycleOperation(CJumpgatePlaybackResultState& owner, std::try_to_lock_t tag);

    CJumpgatePlaybackResultState* m_owner{nullptr};
    std::unique_lock<std::mutex> m_lock;
    uint64_t m_generation{0};
  };

  LifecycleOperation BeginLifecycleOperation();
  std::optional<LifecycleOperation> TryBeginLifecycleOperation();
  bool Begin(LifecycleOperation& operation, uint64_t generation);
  bool Begin(LifecycleOperation& operation, uint64_t generation, std::string requestId);
  bool Begin(uint64_t generation);
  void CloseAdmissions();
  bool AdmissionsClosed() const;
  std::optional<JumpgatePlaybackResult> Finish(uint64_t generation,
                                               int64_t positionMs,
                                               int64_t durationMs,
                                               bool completed);
  bool Capture(uint64_t generation, int64_t positionMs, int64_t durationMs);
  std::optional<JumpgatePlaybackResult> Finish(uint64_t generation, bool completed);
  std::optional<JumpgatePlaybackResult> TakeFinished();
  std::optional<JumpgatePlaybackResult> TakeFinished(LifecycleOperation& operation);
  std::optional<JumpgatePlaybackResultOwner> CurrentOwner(
      const LifecycleOperation& operation) const;
  uint64_t CurrentGeneration() const;
  bool IsCurrent(uint64_t generation) const;
  bool Reset(uint64_t generation);
  bool Reset(LifecycleOperation& operation, uint64_t generation);
  void Reset();

private:
  std::mutex m_lifecycleMutex;
  mutable std::mutex m_mutex;
  uint64_t m_generation{0};
  bool m_pending{false};
  int64_t m_positionMs{0};
  int64_t m_durationMs{0};
  std::string m_requestId;
  std::optional<JumpgatePlaybackResult> m_finishedResult;
  bool m_admissionsClosed{false};
};

} // namespace KODI::JUMPGATE
