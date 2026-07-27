/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgateHistoryEventClient.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace KODI::JUMPGATE
{

struct JumpgateHistoryEventBinding final
{
  std::uint64_t generation{0};
  std::string profileId;
  std::string deviceId;
  std::string bridgeOrigin;
  std::string deviceToken;
  std::string historyGrant;
  std::string historyGrantKind;
  std::string sessionId;
  std::uint64_t sessionRevision{0};

  void ClearSensitive();
};

enum class JumpgateHistoryTerminalStatus
{
  NotRequired,
  Pending,
  Finished,
  Rejected,
};

struct JumpgateHistoryTerminalResult final
{
  JumpgateHistoryTerminalResult() = default;
  JumpgateHistoryTerminalResult(JumpgateHistoryTerminalStatus value, std::uint64_t valueGeneration)
    : status(value),
      generation(valueGeneration)
  {
  }

  bool IsFinished() const
  {
    return status == JumpgateHistoryTerminalStatus::NotRequired ||
           status == JumpgateHistoryTerminalStatus::Finished;
  }

  JumpgateHistoryTerminalStatus status{JumpgateHistoryTerminalStatus::NotRequired};
  std::uint64_t generation{0};
  JumpgateHistoryEvent event{JumpgateHistoryEvent::Stop};
  JumpgateHistoryEventStatus resultStatus{JumpgateHistoryEventStatus::InvalidResponse};
  std::size_t attempts{0};
  bool accepted{false};
  std::string terminalReceiptId;
};

class CJumpgateHistoryEventState final
{
  struct BindingRecord;

public:
  struct Work final
  {
    Work() = default;
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
    Work(Work&&) noexcept = default;
    Work& operator=(Work&&) noexcept = default;
    ~Work();

    void ClearSensitive();

    JumpgateHistoryEvent event{JumpgateHistoryEvent::Start};
    JumpgateHistoryEventRequest request;
    std::uint64_t generation{0};
    bool terminal{false};

  private:
    friend class CJumpgateHistoryEventState;
    std::shared_ptr<BindingRecord> record;
  };

  bool AdvanceGeneration(std::uint64_t generation);
  void CancelGeneration(std::uint64_t generation);
  bool BindClaim(JumpgateHistoryEventBinding binding,
                 JumpgateHistorySnapshot snapshot,
                 std::int64_t nowMs);

  void PlaybackStarted(bool resumed, JumpgateHistorySnapshot snapshot, std::int64_t nowMs);
  void PlaybackPaused(JumpgateHistorySnapshot snapshot);
  void SetBackgrounded(bool backgrounded, JumpgateHistorySnapshot snapshot);
  void ProcessSlow(JumpgateHistorySnapshot snapshot, std::int64_t nowMs, std::int64_t intervalMs);

  JumpgateHistoryTerminalResult BeginTerminal(std::uint64_t generation,
                                              bool completed,
                                              JumpgateHistorySnapshot snapshot);
  JumpgateHistoryTerminalResult GetTerminalResult(std::uint64_t generation) const;

  bool HasPendingWork() const { return !m_pending.empty(); }
  std::optional<Work> TakeNext();
  void Complete(Work& work,
                const JumpgateHistoryEventResult& result,
                std::size_t attempts,
                bool ambiguousOutcome);
  void Clear();

private:
  enum class PlaybackState
  {
    Stopped,
    Playing,
    Paused,
  };

  struct Intent final
  {
    JumpgateHistoryEvent event{JumpgateHistoryEvent::Start};
    std::shared_ptr<BindingRecord> record;
    std::string idempotencyKey;
    JumpgateHistorySnapshot snapshot;
    bool terminal{false};
  };

  static JumpgateHistorySnapshot NormalizeSnapshot(JumpgateHistorySnapshot snapshot);
  void QueueStart(const std::shared_ptr<BindingRecord>& record,
                  JumpgateHistorySnapshot snapshot,
                  std::int64_t nowMs);
  void QueueProgress(const std::shared_ptr<BindingRecord>& record,
                     JumpgateHistorySnapshot snapshot,
                     std::int64_t nowMs);
  void QueueEvent(JumpgateHistoryEvent event,
                  const std::shared_ptr<BindingRecord>& record,
                  JumpgateHistorySnapshot snapshot,
                  bool terminal);
  void FinishSkipped(Intent& intent);
  void PruneRecords();

  std::uint64_t m_generation{0};
  PlaybackState m_playbackState{PlaybackState::Stopped};
  bool m_backgrounded{false};
  bool m_desiredPaused{false};
  std::shared_ptr<BindingRecord> m_current;
  std::deque<Intent> m_pending;
  std::unordered_map<std::uint64_t, std::shared_ptr<BindingRecord>> m_records;
  std::deque<std::uint64_t> m_recordOrder;
};

} // namespace KODI::JUMPGATE
