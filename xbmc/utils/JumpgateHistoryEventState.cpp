/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateHistoryEventState.h"

#include "utils/StringUtils.h"

#include <algorithm>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
void SecureClear(std::string& value)
{
  volatile char* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = '\0';
  value.clear();
}
} // namespace

struct CJumpgateHistoryEventState::BindingRecord final
{
  enum class RemoteState
  {
    Idle,
    Playing,
    Paused,
    Backgrounded,
    Terminal,
  };

  explicit BindingRecord(JumpgateHistoryEventBinding value)
    : binding(std::move(value)),
      acknowledgedRevision(binding.sessionRevision)
  {
  }

  ~BindingRecord() { binding.ClearSensitive(); }

  JumpgateHistoryEventBinding binding;
  std::uint64_t acknowledgedRevision{0};
  bool revisionCertain{true};
  bool usable{true};
  RemoteState remoteState{RemoteState::Idle};
  bool startQueued{false};
  std::size_t pendingPeriodic{0};
  std::int64_t lastProgressQueuedAtMs{0};
  bool terminalQueued{false};
  bool terminalComplete{false};
  JumpgateHistoryEvent terminalEvent{JumpgateHistoryEvent::Stop};
  JumpgateHistoryEventStatus terminalStatus{JumpgateHistoryEventStatus::InvalidResponse};
  std::size_t terminalAttempts{0};
  bool terminalAccepted{false};
  std::string terminalReceiptId;
  bool abandoned{false};
};

void JumpgateHistoryEventBinding::ClearSensitive()
{
  SecureClear(deviceToken);
  SecureClear(historyGrant);
  SecureClear(sessionId);
  profileId.clear();
  deviceId.clear();
  bridgeOrigin.clear();
  historyGrantKind.clear();
  generation = 0;
  sessionRevision = 0;
}

CJumpgateHistoryEventState::Work::~Work()
{
  ClearSensitive();
}

void CJumpgateHistoryEventState::Work::ClearSensitive()
{
  SecureClear(request.deviceToken);
  SecureClear(request.historyGrant);
  SecureClear(request.idempotencyKey);
  SecureClear(request.sessionId);
  request.bridgeOrigin.clear();
  request.historyGrantKind.clear();
  request.sessionRevision = 0;
  request.snapshot.playbackPreferences.reset();
}

JumpgateHistorySnapshot CJumpgateHistoryEventState::NormalizeSnapshot(
    JumpgateHistorySnapshot snapshot)
{
  snapshot.positionMs = std::max<std::int64_t>(0, snapshot.positionMs);
  snapshot.durationMs = std::max<std::int64_t>(0, snapshot.durationMs);
  snapshot.watchedMs = std::max<std::int64_t>(0, snapshot.watchedMs);
  if (snapshot.durationMs == 0)
  {
    snapshot.positionMs = 0;
    snapshot.watchedMs = 0;
  }
  else
  {
    snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
    snapshot.watchedMs = std::min(snapshot.watchedMs, snapshot.durationMs);
  }
  return snapshot;
}

bool CJumpgateHistoryEventState::AdvanceGeneration(std::uint64_t generation)
{
  if (generation == 0 || generation < m_generation)
    return false;
  if (generation == m_generation)
    return true;
  if (m_current && !m_current->terminalQueued)
    return false;

  m_current.reset();
  m_generation = generation;
  m_playbackState = PlaybackState::Stopped;
  m_desiredPaused = m_backgrounded;
  PruneRecords();
  return true;
}

void CJumpgateHistoryEventState::CancelGeneration(std::uint64_t generation)
{
  if (generation == 0 || generation != m_generation)
    return;
  if (m_current)
  {
    m_current->abandoned = true;
    if (!m_current->terminalQueued)
      m_current->binding.ClearSensitive();
  }
  m_current.reset();
  m_playbackState = PlaybackState::Stopped;
  m_desiredPaused = m_backgrounded;
  PruneRecords();
}

bool CJumpgateHistoryEventState::BindClaim(JumpgateHistoryEventBinding binding,
                                           JumpgateHistorySnapshot snapshot,
                                           std::int64_t nowMs)
{
  if (binding.generation == 0 || binding.generation != m_generation ||
      binding.sessionRevision == 0 || binding.profileId.empty() || binding.deviceId.empty() ||
      binding.bridgeOrigin.empty() || binding.deviceToken.empty() || binding.historyGrant.empty() ||
      binding.historyGrantKind.empty() || binding.sessionId.empty() || m_current)
  {
    binding.ClearSensitive();
    return false;
  }

  m_current = std::make_shared<BindingRecord>(std::move(binding));
  m_records[m_current->binding.generation] = m_current;
  m_recordOrder.emplace_back(m_current->binding.generation);
  if (m_playbackState != PlaybackState::Stopped)
  {
    QueueStart(m_current, snapshot, nowMs);
    if (m_backgrounded)
      QueueEvent(JumpgateHistoryEvent::Background, m_current, snapshot, false);
    else if (m_desiredPaused || m_playbackState == PlaybackState::Paused)
      QueueEvent(JumpgateHistoryEvent::Pause, m_current, snapshot, false);
  }
  PruneRecords();
  return true;
}

void CJumpgateHistoryEventState::PlaybackStarted(bool resumed,
                                                 JumpgateHistorySnapshot snapshot,
                                                 std::int64_t nowMs)
{
  const bool wasStopped = m_playbackState == PlaybackState::Stopped;
  const bool shouldResume = !m_backgrounded && m_desiredPaused && (resumed || !wasStopped);
  m_playbackState = PlaybackState::Playing;
  if (shouldResume)
    m_desiredPaused = false;

  if (!m_current || m_current->terminalQueued || m_current->abandoned)
    return;
  if (!m_current->startQueued)
  {
    QueueStart(m_current, snapshot, nowMs);
    if (m_backgrounded)
      QueueEvent(JumpgateHistoryEvent::Background, m_current, snapshot, false);
    else if (m_desiredPaused)
      QueueEvent(JumpgateHistoryEvent::Pause, m_current, snapshot, false);
    return;
  }
  if (shouldResume)
    QueueEvent(JumpgateHistoryEvent::Resume, m_current, snapshot, false);
}

void CJumpgateHistoryEventState::PlaybackPaused(JumpgateHistorySnapshot snapshot)
{
  m_playbackState = PlaybackState::Paused;
  if (m_desiredPaused)
    return;
  m_desiredPaused = true;
  if (m_current && m_current->startQueued && !m_current->terminalQueued && !m_backgrounded)
    QueueEvent(JumpgateHistoryEvent::Pause, m_current, snapshot, false);
}

void CJumpgateHistoryEventState::SetBackgrounded(bool backgrounded,
                                                 JumpgateHistorySnapshot snapshot)
{
  if (m_backgrounded == backgrounded)
    return;
  m_backgrounded = backgrounded;
  if (!backgrounded)
  {
    if (m_playbackState == PlaybackState::Stopped)
      m_desiredPaused = false;
    else if (m_playbackState == PlaybackState::Playing && m_desiredPaused)
    {
      m_desiredPaused = false;
      if (m_current && m_current->startQueued && !m_current->terminalQueued)
        QueueEvent(JumpgateHistoryEvent::Resume, m_current, std::move(snapshot), false);
    }
    return;
  }

  m_desiredPaused = true;
  if (m_current && m_current->startQueued && !m_current->terminalQueued)
    QueueEvent(JumpgateHistoryEvent::Background, m_current, snapshot, false);
}

void CJumpgateHistoryEventState::ProcessSlow(JumpgateHistorySnapshot snapshot,
                                             std::int64_t nowMs,
                                             std::int64_t intervalMs)
{
  if (!m_current || !m_current->usable || m_current->terminalQueued ||
      m_playbackState != PlaybackState::Playing || m_backgrounded || m_desiredPaused ||
      !m_current->startQueued || m_current->pendingPeriodic != 0 || !m_current->revisionCertain ||
      m_current->remoteState != BindingRecord::RemoteState::Playing || intervalMs <= 0 ||
      nowMs - m_current->lastProgressQueuedAtMs < intervalMs)
  {
    return;
  }
  QueueProgress(m_current, snapshot, nowMs);
}

JumpgateHistoryTerminalResult CJumpgateHistoryEventState::BeginTerminal(
    std::uint64_t generation, bool completed, JumpgateHistorySnapshot snapshot)
{
  if (generation == 0 || generation != m_generation || !m_current)
    return {JumpgateHistoryTerminalStatus::NotRequired, generation};
  if (!m_current->terminalQueued)
  {
    m_current->terminalQueued = true;
    m_current->terminalEvent =
        completed ? JumpgateHistoryEvent::Completion : JumpgateHistoryEvent::Stop;
    QueueEvent(m_current->terminalEvent, m_current, snapshot, true);
  }
  m_playbackState = PlaybackState::Stopped;
  m_desiredPaused = m_backgrounded;
  return GetTerminalResult(generation);
}

JumpgateHistoryTerminalResult CJumpgateHistoryEventState::GetTerminalResult(
    std::uint64_t generation) const
{
  const auto found = m_records.find(generation);
  if (generation == 0 || found == m_records.end() || !found->second)
    return {JumpgateHistoryTerminalStatus::NotRequired, generation};
  const std::shared_ptr<BindingRecord>& record = found->second;
  if (!record->terminalQueued)
    return {JumpgateHistoryTerminalStatus::Rejected, generation};

  JumpgateHistoryTerminalResult result;
  result.status = record->terminalComplete ? JumpgateHistoryTerminalStatus::Finished
                                           : JumpgateHistoryTerminalStatus::Pending;
  result.generation = generation;
  result.event = record->terminalEvent;
  result.resultStatus = record->terminalStatus;
  result.attempts = record->terminalAttempts;
  result.accepted = record->terminalAccepted;
  result.terminalReceiptId = record->terminalReceiptId;
  return result;
}

std::optional<CJumpgateHistoryEventState::Work> CJumpgateHistoryEventState::TakeNext()
{
  while (!m_pending.empty())
  {
    Intent intent = std::move(m_pending.front());
    m_pending.pop_front();
    const std::shared_ptr<BindingRecord>& record = intent.record;
    if (!record || record->binding.deviceToken.empty() || record->binding.historyGrant.empty() ||
        record->binding.sessionId.empty() || !record->revisionCertain ||
        record->acknowledgedRevision == 0 || (record->abandoned && !intent.terminal) ||
        !record->usable)
    {
      FinishSkipped(intent);
      continue;
    }

    Work work;
    work.event = intent.event;
    work.generation = record->binding.generation;
    work.terminal = intent.terminal;
    work.record = record;
    work.request.bridgeOrigin = record->binding.bridgeOrigin;
    work.request.deviceToken = record->binding.deviceToken;
    work.request.historyGrant = record->binding.historyGrant;
    work.request.historyGrantKind = record->binding.historyGrantKind;
    work.request.idempotencyKey = std::move(intent.idempotencyKey);
    work.request.sessionId = record->binding.sessionId;
    work.request.sessionRevision = record->acknowledgedRevision;
    work.request.snapshot = NormalizeSnapshot(std::move(intent.snapshot));
    return work;
  }
  return std::nullopt;
}

void CJumpgateHistoryEventState::Complete(Work& work,
                                          const JumpgateHistoryEventResult& result,
                                          std::size_t attempts,
                                          bool ambiguousOutcome)
{
  const std::shared_ptr<BindingRecord> record = work.record;
  if (!record)
    return;
  if ((work.event == JumpgateHistoryEvent::Start || work.event == JumpgateHistoryEvent::Progress) &&
      record->pendingPeriodic != 0)
  {
    --record->pendingPeriodic;
  }

  if (result.IsAccepted())
  {
    record->acknowledgedRevision = result.sessionRevision;
    record->revisionCertain = true;
    if (result.sessionState == "playing")
      record->remoteState = BindingRecord::RemoteState::Playing;
    else if (result.sessionState == "paused")
      record->remoteState = BindingRecord::RemoteState::Paused;
    else if (result.sessionState == "backgrounded")
      record->remoteState = BindingRecord::RemoteState::Backgrounded;
    else if (result.sessionState == "released")
      record->remoteState = BindingRecord::RemoteState::Terminal;
  }
  else
  {
    record->usable = false;
    if (ambiguousOutcome)
      record->revisionCertain = false;
  }

  if (work.terminal)
  {
    record->terminalComplete = true;
    record->terminalStatus = result.status;
    record->terminalAttempts = attempts;
    record->terminalAccepted = result.IsAccepted();
    if (record->terminalAccepted)
      record->terminalReceiptId = work.request.idempotencyKey;
    else
      SecureClear(record->terminalReceiptId);
    record->binding.ClearSensitive();
  }
  PruneRecords();
}

void CJumpgateHistoryEventState::Clear()
{
  for (Intent& intent : m_pending)
    SecureClear(intent.idempotencyKey);
  m_pending.clear();
  for (auto& [generation, record] : m_records)
  {
    if (record)
      record->binding.ClearSensitive();
  }
  m_records.clear();
  m_recordOrder.clear();
  m_current.reset();
  m_generation = 0;
  m_playbackState = PlaybackState::Stopped;
  m_backgrounded = false;
  m_desiredPaused = false;
}

void CJumpgateHistoryEventState::QueueStart(const std::shared_ptr<BindingRecord>& record,
                                            JumpgateHistorySnapshot snapshot,
                                            std::int64_t nowMs)
{
  if (!record || record->terminalQueued)
    return;
  record->startQueued = true;
  ++record->pendingPeriodic;
  record->lastProgressQueuedAtMs = nowMs;
  QueueEvent(JumpgateHistoryEvent::Start, record, std::move(snapshot), false);
}

void CJumpgateHistoryEventState::QueueProgress(const std::shared_ptr<BindingRecord>& record,
                                               JumpgateHistorySnapshot snapshot,
                                               std::int64_t nowMs)
{
  if (!record || record->terminalQueued)
    return;
  ++record->pendingPeriodic;
  record->lastProgressQueuedAtMs = nowMs;
  QueueEvent(JumpgateHistoryEvent::Progress, record, std::move(snapshot), false);
}

void CJumpgateHistoryEventState::QueueEvent(JumpgateHistoryEvent event,
                                            const std::shared_ptr<BindingRecord>& record,
                                            JumpgateHistorySnapshot snapshot,
                                            bool terminal)
{
  Intent intent;
  intent.event = event;
  intent.record = record;
  intent.idempotencyKey = StringUtils::CreateUUID();
  intent.snapshot = NormalizeSnapshot(std::move(snapshot));
  intent.terminal = terminal;
  m_pending.emplace_back(std::move(intent));
}

void CJumpgateHistoryEventState::FinishSkipped(Intent& intent)
{
  if (intent.record &&
      (intent.event == JumpgateHistoryEvent::Start ||
       intent.event == JumpgateHistoryEvent::Progress) &&
      intent.record->pendingPeriodic != 0)
  {
    --intent.record->pendingPeriodic;
  }
  if (intent.record && intent.terminal)
  {
    intent.record->terminalComplete = true;
    intent.record->terminalStatus = JumpgateHistoryEventStatus::InvalidRequest;
    intent.record->terminalAttempts = 0;
    intent.record->terminalAccepted = false;
    SecureClear(intent.record->terminalReceiptId);
    intent.record->binding.ClearSensitive();
  }
  SecureClear(intent.idempotencyKey);
}

void CJumpgateHistoryEventState::PruneRecords()
{
  if (m_records.size() <= 16)
    return;
  for (auto iterator = m_recordOrder.begin();
       iterator != m_recordOrder.end() && m_records.size() > 16;)
  {
    const auto found = m_records.find(*iterator);
    if (found == m_records.end())
    {
      iterator = m_recordOrder.erase(iterator);
      continue;
    }
    const std::shared_ptr<BindingRecord>& record = found->second;
    if (record != m_current && record && (record->terminalComplete || record->abandoned))
    {
      m_records.erase(found);
      iterator = m_recordOrder.erase(iterator);
      continue;
    }
    ++iterator;
  }
}

} // namespace KODI::JUMPGATE
