/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackAttemptState.h"

#include <algorithm>
#include <iterator>

namespace KODI::JUMPGATE
{

std::optional<JumpgateRemovedPlaybackTerminal> DecodeJumpgateRemovedPlaybackTerminal(
    int messageId, int stoppedMessageId, int endedMessageId, int64_t rawToken)
{
  if ((messageId != stoppedMessageId && messageId != endedMessageId) || rawToken <= 0)
    return std::nullopt;

  return JumpgateRemovedPlaybackTerminal{static_cast<uint64_t>(rawToken),
                                         messageId == endedMessageId};
}

bool CJumpgatePlaybackAttemptState::Bind(uint64_t token, JumpgatePlaybackOpenMode mode)
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto existing =
      std::find_if(m_pendingAttempts.begin(), m_pendingAttempts.end(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  if (existing != m_pendingAttempts.end())
    return false;

  m_pendingAttempts.emplace_back(
      Attempt{token, false, mode == JumpgatePlaybackOpenMode::Immediate});
  return true;
}

bool CJumpgatePlaybackAttemptState::BeginOpenNext(uint64_t token)
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto attempt =
      std::find_if(m_pendingAttempts.rbegin(), m_pendingAttempts.rend(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  if (attempt == m_pendingAttempts.rend() || attempt->openNextReady)
    return false;

  attempt->openNextReady = true;
  return true;
}

bool CJumpgatePlaybackAttemptState::MarkStarted(uint64_t token)
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto attempt =
      std::find_if(m_pendingAttempts.rbegin(), m_pendingAttempts.rend(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  if (attempt == m_pendingAttempts.rend() || attempt != m_pendingAttempts.rbegin() ||
      !attempt->openNextReady || attempt->started)
    return false;

  attempt->started = true;
  return true;
}

bool CJumpgatePlaybackAttemptState::IsStarted(uint64_t token) const
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto attempt =
      std::find_if(m_pendingAttempts.rbegin(), m_pendingAttempts.rend(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  return attempt != m_pendingAttempts.rend() && attempt->started;
}

bool CJumpgatePlaybackAttemptState::IsSuperseded(uint64_t token) const
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto isNewer = [token](const auto& candidate) { return candidate.token > token; };
  return std::any_of(m_pendingAttempts.begin(), m_pendingAttempts.end(), isNewer) ||
         std::any_of(m_unacknowledgedTerminals.begin(), m_unacknowledgedTerminals.end(), isNewer);
}

bool CJumpgatePlaybackAttemptState::CancelOpen(uint64_t token)
{
  if (token == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto attempt =
      std::find_if(m_pendingAttempts.rbegin(), m_pendingAttempts.rend(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  if (attempt == m_pendingAttempts.rend() || attempt->started)
    return false;

  m_pendingAttempts.erase(std::next(attempt).base());
  return true;
}

std::optional<JumpgatePlaybackTerminal> CJumpgatePlaybackAttemptState::EmitTerminal(uint64_t token,
                                                                                    bool completed)
{
  if (token == 0)
    return std::nullopt;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto attempt =
      std::find_if(m_pendingAttempts.rbegin(), m_pendingAttempts.rend(),
                   [token](const Attempt& candidate) { return candidate.token == token; });
  if (attempt == m_pendingAttempts.rend())
    return std::nullopt;

  const bool superseded = attempt != m_pendingAttempts.rbegin();
  const JumpgatePlaybackTerminal terminal{token, completed, attempt->started, superseded};
  // A newer exact terminal proves older attempts on this player were superseded.
  // A delayed old terminal erases only itself and leaves newer attempts intact.
  m_pendingAttempts.erase(m_pendingAttempts.begin(), attempt.base());
  m_unacknowledgedTerminals.emplace_back(terminal);
  return terminal;
}

std::optional<JumpgatePlaybackTerminal> CJumpgatePlaybackAttemptState::AcknowledgeTerminal(
    uint64_t token, bool completed)
{
  if (token == 0)
    return std::nullopt;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto emitted =
      std::find_if(m_unacknowledgedTerminals.begin(), m_unacknowledgedTerminals.end(),
                   [token, completed](const JumpgatePlaybackTerminal& candidate)
                   { return candidate.token == token && candidate.completed == completed; });
  if (emitted == m_unacknowledgedTerminals.end())
    return std::nullopt;

  JumpgatePlaybackTerminal terminal = *emitted;
  const auto isNewer = [token](const auto& candidate) { return candidate.token > token; };
  terminal.superseded =
      terminal.superseded ||
      std::any_of(m_pendingAttempts.begin(), m_pendingAttempts.end(), isNewer) ||
      std::any_of(m_unacknowledgedTerminals.begin(), m_unacknowledgedTerminals.end(), isNewer);
  m_unacknowledgedTerminals.erase(emitted);
  return terminal;
}

std::size_t CJumpgatePlaybackAttemptState::PendingAttemptCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_pendingAttempts.size();
}

} // namespace KODI::JUMPGATE
