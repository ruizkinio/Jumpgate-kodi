/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackAuthority.h"

#include <algorithm>
#include <iterator>

namespace KODI::JUMPGATE
{

CJumpgatePlaybackAuthority::Transaction::Transaction(CJumpgatePlaybackAuthority& owner)
  : m_owner(&owner),
    m_lock(owner.m_mutex)
{
}

bool CJumpgatePlaybackAuthority::Transaction::CanMutateProfile() const
{
  return m_owner->m_profileMutationToken == 0 && m_owner->m_activeToken == 0 &&
         m_owner->m_pendingAdmissions.empty() && m_owner->m_startedTokens.empty();
}

bool CJumpgatePlaybackAuthority::Transaction::CanAdmitPlayback() const
{
  return m_owner->m_profileMutationToken == 0;
}

std::optional<CJumpgatePlaybackAuthority::Token> CJumpgatePlaybackAuthority::Transaction::
    BeginProfileMutation()
{
  if (!CanMutateProfile())
    return std::nullopt;

  const Token token = m_owner->m_nextToken++;
  m_owner->m_profileMutationToken = token;
  m_owner->m_lastStoppedEvent.reset();
  return token;
}

bool CJumpgatePlaybackAuthority::Transaction::CommitProfileMutation(Token token)
{
  if (token == 0 || token != m_owner->m_profileMutationToken)
    return false;
  m_owner->m_profileMutationToken = 0;
  return true;
}

bool CJumpgatePlaybackAuthority::Transaction::RollbackProfileMutation(Token token)
{
  if (token == 0 || token != m_owner->m_profileMutationToken)
    return false;
  m_owner->m_profileMutationToken = 0;
  return true;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CommitAdmission(uint64_t generation)
{
  if (!CanAdmitPlayback() || generation == 0)
    return std::nullopt;

  const Token token = m_owner->m_nextToken++;
  const Event event{token, generation};
  m_owner->m_lastStoppedEvent.reset();
  m_owner->m_pendingAdmissions.emplace_back(event);
  return event;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CommitPlaybackStarted(uint64_t fallbackGeneration, Token token)
{
  if (!CanAdmitPlayback())
    return std::nullopt;

  Event event;
  const auto pending =
      token == 0
          ? m_owner->m_pendingAdmissions.end()
          : std::find_if(m_owner->m_pendingAdmissions.begin(), m_owner->m_pendingAdmissions.end(),
                         [token](const Event& candidate) { return candidate.token == token; });
  if (token != 0 && pending != m_owner->m_pendingAdmissions.end())
  {
    event = *pending;
    // A newer exact start proves that earlier start callbacks were superseded
    // before ApplicationMessageHandling could commit them.
    m_owner->m_pendingAdmissions.erase(m_owner->m_pendingAdmissions.begin(), std::next(pending));
    m_owner->m_lastStoppedEvent.reset();
  }
  else if (token != 0)
  {
    if (m_owner->m_activeToken != 0 || !m_owner->m_pendingAdmissions.empty() ||
        !m_owner->m_lastStoppedEvent || m_owner->m_lastStoppedEvent->token != token ||
        m_owner->m_lastStoppedEvent->generation != fallbackGeneration)
    {
      return std::nullopt;
    }
    event = *m_owner->m_lastStoppedEvent;
    m_owner->m_lastStoppedEvent.reset();
  }
  else
  {
    // Untokened ordinary Kodi playback may create independent generation-zero
    // authority, but it must never claim a pending Jumpgate admission.
    if (!m_owner->m_pendingAdmissions.empty())
      return std::nullopt;
    event.token = m_owner->m_nextToken++;
    event.generation = fallbackGeneration;
    m_owner->m_lastStoppedEvent.reset();
  }
  m_owner->m_startedTokens.emplace_back(event);
  m_owner->m_activeToken = event.token;
  return event;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CommitPlaybackResumed() const
{
  if (m_owner->m_activeToken == 0)
    return std::nullopt;
  for (auto event = m_owner->m_startedTokens.rbegin(); event != m_owner->m_startedTokens.rend();
       ++event)
  {
    if (event->token == m_owner->m_activeToken)
      return *event;
  }
  return std::nullopt;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CommitPlaybackStopped(Token token)
{
  if (token == 0 || m_owner->m_startedTokens.empty())
    return std::nullopt;

  const auto started =
      std::find_if(m_owner->m_startedTokens.begin(), m_owner->m_startedTokens.end(),
                   [token](const Event& event) { return event.token == token; });
  if (started == m_owner->m_startedTokens.end())
    return std::nullopt;

  const Event event = *started;
  if (m_owner->m_activeToken == event.token)
  {
    // Earlier callbacks have already been emitted if a newer active token is
    // terminating. Their GUI messages may have been removed by PlayFile.
    m_owner->m_startedTokens.erase(m_owner->m_startedTokens.begin(), std::next(started));
    m_owner->m_activeToken = 0;
    m_owner->m_lastStoppedEvent = event;
  }
  else
  {
    m_owner->m_startedTokens.erase(started);
  }
  return event;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CommitPlaybackTerminal(Token token, bool startedObserved)
{
  if (const auto stopped = CommitPlaybackStopped(token); stopped)
    return stopped;

  if (token == 0)
    return std::nullopt;

  const auto pending =
      std::find_if(m_owner->m_pendingAdmissions.begin(), m_owner->m_pendingAdmissions.end(),
                   [token](const Event& event) { return event.token == token; });
  if (pending == m_owner->m_pendingAdmissions.end())
    return std::nullopt;

  if (!startedObserved)
  {
    const Event event = *pending;
    m_owner->m_pendingAdmissions.erase(pending);
    return event;
  }

  const Event event = *pending;
  // Core playback began before the GUI OnPlay announcement was observed. Make
  // that exact admission active, purge only admissions it superseded, and then
  // terminate it in the same authority transaction. This also clears any old
  // started token owned by the player that just stopped.
  m_owner->m_pendingAdmissions.erase(m_owner->m_pendingAdmissions.begin(), std::next(pending));
  m_owner->m_lastStoppedEvent.reset();
  m_owner->m_startedTokens.emplace_back(event);
  m_owner->m_activeToken = event.token;
  const auto stopped = CommitPlaybackStopped(event.token);
  // This attempt already reached a terminal callback. Unlike a real stack or
  // playlist continuation, a delayed GUI start must never revive it.
  m_owner->m_lastStoppedEvent.reset();
  return stopped;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CancelPendingAdmission(uint64_t generation)
{
  const auto pending = generation == 0 ? m_owner->m_pendingAdmissions.begin()
                                       : std::find_if(m_owner->m_pendingAdmissions.begin(),
                                                      m_owner->m_pendingAdmissions.end(),
                                                      [generation](const Event& event)
                                                      { return event.generation == generation; });
  if (pending == m_owner->m_pendingAdmissions.end())
    return std::nullopt;

  const Event event = *pending;
  m_owner->m_pendingAdmissions.erase(pending);
  return event;
}

std::optional<CJumpgatePlaybackAuthority::Event> CJumpgatePlaybackAuthority::Transaction::
    CancelPendingAdmissionByToken(Token token)
{
  if (token == 0)
    return std::nullopt;
  const auto pending =
      std::find_if(m_owner->m_pendingAdmissions.begin(), m_owner->m_pendingAdmissions.end(),
                   [token](const Event& event) { return event.token == token; });
  if (pending == m_owner->m_pendingAdmissions.end())
    return std::nullopt;

  const Event event = *pending;
  m_owner->m_pendingAdmissions.erase(pending);
  return event;
}

bool CJumpgatePlaybackAuthority::Transaction::IsLatestPendingAdmission(Token token) const
{
  return token != 0 && !m_owner->m_pendingAdmissions.empty() &&
         m_owner->m_pendingAdmissions.back().token == token;
}

bool CJumpgatePlaybackAuthority::Transaction::HasNewerPlayback(Token token) const
{
  if (token == 0)
    return false;

  const auto isNewer = [token](const Event& event) { return event.token > token; };
  return std::any_of(m_owner->m_pendingAdmissions.begin(), m_owner->m_pendingAdmissions.end(),
                     isNewer) ||
         std::any_of(m_owner->m_startedTokens.begin(), m_owner->m_startedTokens.end(), isNewer);
}

CJumpgatePlaybackAuthority::Token CJumpgatePlaybackAuthority::Transaction::GetActiveToken() const
{
  return m_owner->m_activeToken;
}

CJumpgatePlaybackAuthority::Transaction CJumpgatePlaybackAuthority::BeginTransaction()
{
  return Transaction(*this);
}

} // namespace KODI::JUMPGATE
