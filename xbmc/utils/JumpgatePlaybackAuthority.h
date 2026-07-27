/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace KODI::JUMPGATE
{

class CJumpgatePlaybackAuthority final
{
public:
  using Token = uint64_t;

  struct Event
  {
    Token token{0};
    uint64_t generation{0};
  };

  class Transaction final
  {
  public:
    Transaction(Transaction&&) noexcept = default;
    Transaction& operator=(Transaction&&) noexcept = default;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool CanMutateProfile() const;
    bool CanAdmitPlayback() const;
    std::optional<Token> BeginProfileMutation();
    bool CommitProfileMutation(Token token);
    bool RollbackProfileMutation(Token token);
    std::optional<Event> CommitAdmission(uint64_t generation);
    std::optional<Event> CommitPlaybackStarted(uint64_t fallbackGeneration = 0, Token token = 0);
    std::optional<Event> CommitPlaybackResumed() const;
    std::optional<Event> CommitPlaybackStopped(Token token = 0);
    std::optional<Event> CommitPlaybackTerminal(Token token, bool startedObserved);
    std::optional<Event> CancelPendingAdmission(uint64_t generation = 0);
    std::optional<Event> CancelPendingAdmissionByToken(Token token);
    bool IsLatestPendingAdmission(Token token) const;
    bool HasNewerPlayback(Token token) const;
    Token GetActiveToken() const;

  private:
    friend class CJumpgatePlaybackAuthority;
    explicit Transaction(CJumpgatePlaybackAuthority& owner);

    CJumpgatePlaybackAuthority* m_owner;
    std::unique_lock<std::mutex> m_lock;
  };

  Transaction BeginTransaction();

private:
  std::mutex m_mutex;
  std::deque<Event> m_pendingAdmissions;
  std::deque<Event> m_startedTokens;
  std::optional<Event> m_lastStoppedEvent;
  Token m_activeToken{0};
  Token m_profileMutationToken{0};
  Token m_nextToken{1};
};

} // namespace KODI::JUMPGATE
