/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackHistoryState.h"

#include <charconv>
#include <limits>
#include <utility>

namespace KODI::JUMPGATE
{

std::optional<int64_t> ParseJumpgatePositiveInt64(std::string_view value)
{
  if (value.empty() || value.front() == '+' || value.front() == '-')
    return std::nullopt;
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
      parsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
  {
    return std::nullopt;
  }
  return static_cast<int64_t>(parsed);
}

bool IsJumpgateResumeCorrectionWithinWindow(int64_t playbackStartedAtMs,
                                            int64_t observedAtMs,
                                            int64_t windowMs)
{
  if (playbackStartedAtMs < 0 || observedAtMs < 0 || windowMs < 0)
    return false;
  if (playbackStartedAtMs == 0)
    return true;
  return observedAtMs >= playbackStartedAtMs &&
         observedAtMs - playbackStartedAtMs <= windowMs;
}

bool CJumpgatePlaybackHistoryState::AdvanceGeneration(uint64_t generation)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation == 0 || generation <= m_generation)
    return false;
  m_generation = generation;
  ++m_resumeSerial;
  m_active.reset();
  return true;
}

bool CJumpgatePlaybackHistoryState::Activate(JumpgatePlaybackHistoryIdentity identity,
                                             int64_t activatedAtMs)
{
  const JumpgatePlaybackHistoryKey key{identity.historyNamespace, identity.profileId,
                                       identity.contentKey};
  if (!IsValidJumpgatePlaybackHistoryKey(key) || activatedAtMs < 0 ||
      (identity.canonicalIdentity &&
       !IsValidJumpgateHistoryCanonicalIdentity(*identity.canonicalIdentity)) ||
      (identity.historyNamespace == JumpgatePlaybackHistoryNamespace::LocalSource &&
       identity.canonicalIdentity))
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (identity.generation == 0 || identity.generation != m_generation || m_active)
    return false;
  ++m_resumeSerial;
  m_active =
      ActiveState{std::move(identity), 0, 0, activatedAtMs, false, std::nullopt, std::nullopt};
  return true;
}

bool CJumpgatePlaybackHistoryState::ActivateLocalSource(
    uint64_t generation,
    const std::vector<std::string>& canonicalFingerprints,
    std::string_view rawLaunchUri,
    int64_t activatedAtMs)
{
  std::optional<std::string> contentKey =
      DeriveJumpgateLocalSourceHistoryKey(canonicalFingerprints);
  if (!contentKey)
    contentKey = DeriveJumpgateLocalSourceFallbackHistoryKey(rawLaunchUri);

  JumpgatePlaybackHistoryIdentity identity;
  identity.generation = generation;
  identity.historyNamespace = JumpgatePlaybackHistoryNamespace::LocalSource;
  identity.contentKey = std::move(*contentKey);
  return Activate(std::move(identity), activatedAtMs);
}

bool CJumpgatePlaybackHistoryState::Promote(JumpgatePlaybackHistoryIdentity identity)
{
  const JumpgatePlaybackHistoryKey key{identity.historyNamespace, identity.profileId,
                                       identity.contentKey};
  if (identity.historyNamespace != JumpgatePlaybackHistoryNamespace::AuthenticatedProfile ||
      !IsValidJumpgatePlaybackHistoryKey(key) ||
      (identity.canonicalIdentity &&
       !IsValidJumpgateHistoryCanonicalIdentity(*identity.canonicalIdentity)))
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_active || m_active->finalSnapshot || identity.generation == 0 ||
      identity.generation != m_generation || identity.generation != m_active->identity.generation ||
      m_active->identity.historyNamespace != JumpgatePlaybackHistoryNamespace::LocalSource ||
      m_active->identity.contentKey == identity.contentKey)
  {
    return false;
  }

  ++m_resumeSerial;
  m_active->identity = std::move(identity);
  m_active->resumeApplied = false;
  return true;
}

bool CJumpgatePlaybackHistoryState::UpdateProgress(uint64_t generation,
                                                   int64_t positionMs,
                                                   int64_t durationMs,
                                                   int64_t observedAtMs)
{
  if (positionMs < 0 || durationMs < 0 || observedAtMs < 0)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_active || generation != m_generation || generation != m_active->identity.generation ||
      m_active->finalSnapshot || observedAtMs < m_active->updatedAtMs)
  {
    return false;
  }
  if (positionMs == 0 && durationMs == 0 && (m_active->positionMs > 0 || m_active->durationMs > 0))
  {
    return false;
  }
  m_active->positionMs = positionMs;
  m_active->durationMs = durationMs;
  m_active->updatedAtMs = observedAtMs;
  return true;
}

std::optional<JumpgatePlaybackHistoryEntry> CJumpgatePlaybackHistoryState::Finalize(
    uint64_t generation, bool explicitEnd, int64_t observedAtMs)
{
  if (observedAtMs < 0)
    return std::nullopt;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_active || generation != m_generation || generation != m_active->identity.generation)
    return std::nullopt;
  if (m_active->finalSnapshot)
  {
    if (explicitEnd && !m_active->finalSnapshot->completed)
    {
      m_active->finalSnapshot->completed = true;
      if (observedAtMs > m_active->finalSnapshot->updatedAtMs)
      {
        m_active->finalSnapshot->updatedAtMs = observedAtMs;
      }
      else if (m_active->finalSnapshot->updatedAtMs < std::numeric_limits<int64_t>::max())
      {
        ++m_active->finalSnapshot->updatedAtMs;
      }
    }
    return m_active->finalSnapshot;
  }
  if (!explicitEnd && m_active->positionMs == 0 && m_active->durationMs == 0)
    return std::nullopt;

  JumpgatePlaybackHistoryEntry entry;
  entry.historyNamespace = m_active->identity.historyNamespace;
  entry.profileId = m_active->identity.profileId;
  entry.contentKey = m_active->identity.contentKey;
  entry.canonicalIdentity = m_active->identity.canonicalIdentity;
  entry.display = m_active->identity.display;
  entry.positionMs = m_active->positionMs;
  entry.durationMs = m_active->durationMs;
  entry.completed = explicitEnd;
  entry.updatedAtMs = observedAtMs > m_active->updatedAtMs ? observedAtMs : m_active->updatedAtMs;
  m_active->finalSnapshot = entry;
  return entry;
}

std::optional<JumpgatePlaybackResumeToken> CJumpgatePlaybackHistoryState::BeginResume(
    uint64_t generation) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_active || m_active->resumeApplied || m_active->finalSnapshot ||
      generation != m_generation || generation != m_active->identity.generation)
  {
    return std::nullopt;
  }
  return JumpgatePlaybackResumeToken{generation, m_resumeSerial,
                                     m_active->identity.historyNamespace,
                                     m_active->identity.profileId, m_active->identity.contentKey,
                                     m_active->appliedResumePositionMs};
}

bool CJumpgatePlaybackHistoryState::ApplyResume(const JumpgatePlaybackResumeToken& token,
                                                int64_t positionMs,
                                                const std::function<void(int64_t)>& apply)
{
  if (positionMs < 0 || !apply)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_active || m_active->resumeApplied || m_active->finalSnapshot ||
      token.generation != m_generation || token.generation != m_active->identity.generation ||
      token.serial != m_resumeSerial ||
      token.historyNamespace != m_active->identity.historyNamespace ||
      token.profileId != m_active->identity.profileId ||
      token.contentKey != m_active->identity.contentKey)
  {
    return false;
  }
  m_active->resumeApplied = true;
  m_active->appliedResumePositionMs = positionMs;
  apply(positionMs);
  return true;
}

} // namespace KODI::JUMPGATE
