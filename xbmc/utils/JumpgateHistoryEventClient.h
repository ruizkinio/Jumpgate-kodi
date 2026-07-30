/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgatePlaybackClaimClient.h"

#include <cstdint>
#include <optional>
#include <string>

namespace KODI::JUMPGATE
{

enum class JumpgateHistoryEvent
{
  Start,
  Progress,
  Pause,
  Background,
  Resume,
  Stop,
  Completion,
};

struct JumpgateHistorySnapshot final
{
  std::int64_t positionMs{0};
  std::int64_t durationMs{0};
  std::int64_t watchedMs{0};
  std::optional<CVariant> playbackPreferences;
};

struct JumpgateHistoryEventRequest final
{
  std::string bridgeOrigin;
  std::string deviceToken;
  std::string historyGrant;
  std::string historyGrantKind;
  std::string idempotencyKey;
  std::string sessionId;
  std::uint64_t sessionRevision{0};
  JumpgateHistorySnapshot snapshot;
};

enum class JumpgateHistoryEventStatus
{
  Applied,
  LocalOnly,
  Suppressed,
  InvalidRequest,
  TransportFailure,
  AuthenticationFailure,
  StaleGrant,
  IdempotencyConflict,
  Unavailable,
  HttpFailure,
  InvalidResponse,
};

struct JumpgateHistoryEventResult final
{
  bool IsAccepted() const
  {
    return status == JumpgateHistoryEventStatus::Applied ||
           status == JumpgateHistoryEventStatus::LocalOnly ||
           status == JumpgateHistoryEventStatus::Suppressed;
  }

  JumpgateHistoryEventStatus status{JumpgateHistoryEventStatus::InvalidResponse};
  int httpStatus{0};
  std::string sessionState;
  std::uint64_t sessionRevision{0};
};

class CJumpgateHistoryEventClient final
{
public:
  explicit CJumpgateHistoryEventClient(IJumpgatePlaybackClaimTransport& transport);

  JumpgateHistoryEventResult Send(JumpgateHistoryEvent event,
                                  const JumpgateHistoryEventRequest& request) const;

  bool Prepare(JumpgateHistoryEvent event,
               const JumpgateHistoryEventRequest& request,
               JumpgatePlaybackHttpRequest& prepared) const;
  JumpgateHistoryEventResult SendPrepared(JumpgateHistoryEvent event,
                                          const JumpgateHistoryEventRequest& request,
                                          const JumpgatePlaybackHttpRequest& prepared) const;

private:
  IJumpgatePlaybackClaimTransport& m_transport;
};

} // namespace KODI::JUMPGATE
