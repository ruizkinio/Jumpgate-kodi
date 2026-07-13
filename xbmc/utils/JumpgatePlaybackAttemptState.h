/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace KODI::JUMPGATE
{

struct JumpgatePlaybackTerminal
{
  uint64_t token{0};
  bool completed{false};
  bool started{false};
  bool superseded{false};
};

struct JumpgateRemovedPlaybackTerminal
{
  uint64_t token{0};
  bool completed{false};
};

std::optional<JumpgateRemovedPlaybackTerminal> DecodeJumpgateRemovedPlaybackTerminal(
    int messageId, int stoppedMessageId, int endedMessageId, int64_t rawToken);

enum class JumpgatePlaybackOpenMode : uint8_t
{
  Immediate,
  Deferred,
};

class CJumpgatePlaybackAttemptState final
{
public:
  bool Bind(uint64_t token, JumpgatePlaybackOpenMode mode = JumpgatePlaybackOpenMode::Immediate);
  bool BeginOpenNext(uint64_t token);
  bool MarkStarted(uint64_t token);
  bool IsStarted(uint64_t token) const;
  bool IsSuperseded(uint64_t token) const;
  bool CancelOpen(uint64_t token);
  std::optional<JumpgatePlaybackTerminal> EmitTerminal(uint64_t token, bool completed);
  std::optional<JumpgatePlaybackTerminal> AcknowledgeTerminal(uint64_t token, bool completed);
  std::size_t PendingAttemptCount() const;

private:
  struct Attempt
  {
    uint64_t token{0};
    bool started{false};
    bool openNextReady{true};
  };

  mutable std::mutex m_mutex;
  std::deque<Attempt> m_pendingAttempts;
  std::deque<JumpgatePlaybackTerminal> m_unacknowledgedTerminals;
};

} // namespace KODI::JUMPGATE
