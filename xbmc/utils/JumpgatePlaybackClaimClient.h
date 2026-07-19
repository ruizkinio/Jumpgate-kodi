/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Variant.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

struct JumpgatePlaybackHttpHeader final
{
  std::string name;
  std::string value;
};

struct JumpgatePlaybackHttpRequest final
{
  JumpgatePlaybackHttpRequest() = default;
  JumpgatePlaybackHttpRequest(const JumpgatePlaybackHttpRequest&) = delete;
  JumpgatePlaybackHttpRequest& operator=(const JumpgatePlaybackHttpRequest&) = delete;
  ~JumpgatePlaybackHttpRequest();

  void ClearSensitive();

  std::string url;
  std::string contentType;
  std::string authorization;
  std::vector<JumpgatePlaybackHttpHeader> headers;
  std::string body;
  bool followRedirects{false};
};

struct JumpgatePlaybackHttpResponse final
{
  JumpgatePlaybackHttpResponse() = default;
  JumpgatePlaybackHttpResponse(const JumpgatePlaybackHttpResponse&) = delete;
  JumpgatePlaybackHttpResponse& operator=(const JumpgatePlaybackHttpResponse&) = delete;
  ~JumpgatePlaybackHttpResponse();

  void ClearSensitive();

  int statusCode{0};
  std::string body;
};

class IJumpgatePlaybackClaimTransport
{
public:
  virtual ~IJumpgatePlaybackClaimTransport() = default;

  // Implementations must honor followRedirects, and must not log or retain authorization.
  virtual bool Post(const JumpgatePlaybackHttpRequest& request,
                    JumpgatePlaybackHttpResponse& response) = 0;
};

struct PlaybackClaimClientInfo
{
  std::string platform;
  std::string version;
};

struct PlaybackClaimRequest
{
  std::string bridgeOrigin;
  std::string deviceToken;
  std::string attemptId;
  std::vector<std::string> fingerprints;
  std::string intentUrlHash;
  std::int64_t launchedAt{0};
  std::optional<PlaybackClaimClientInfo> client;
};

struct PlaybackClaim
{
  void ClearSensitive();

  std::string sessionId;
  std::uint64_t sessionRevision{0};
  std::string historyGrant;
  std::string historyGrantKind;
  CVariant context;
  std::string claimedAt;
  std::string expiresAt;
};

enum class PlaybackClaimStatus
{
  Claimed,
  Ambiguous,
  Expired,
  NotFound,
  InvalidRequest,
  TransportFailure,
  AuthenticationFailure,
  HttpFailure,
  InvalidResponse,
};

struct PlaybackClaimResult
{
  bool IsClaimed() const { return status == PlaybackClaimStatus::Claimed; }
  void ClearSensitive();

  PlaybackClaimStatus status{PlaybackClaimStatus::InvalidResponse};
  int httpStatus{0};
  PlaybackClaim claim;
};

struct PlaybackReleaseRequest
{
  void ClearSensitive();

  std::string bridgeOrigin;
  std::string deviceToken;
  std::string sessionId;
  std::string terminalReceiptId;
};

enum class PlaybackReleaseStatus
{
  Released,
  NotFound,
  InvalidRequest,
  TransportFailure,
  AuthenticationFailure,
  HttpFailure,
  InvalidResponse,
};

struct PlaybackReleaseResult
{
  bool IsReleased() const { return status == PlaybackReleaseStatus::Released; }

  PlaybackReleaseStatus status{PlaybackReleaseStatus::InvalidResponse};
  int httpStatus{0};
};

class CJumpgatePlaybackClaimClient final
{
public:
  explicit CJumpgatePlaybackClaimClient(IJumpgatePlaybackClaimTransport& transport);

  PlaybackClaimResult Claim(const PlaybackClaimRequest& request) const;
  PlaybackReleaseResult Release(const PlaybackReleaseRequest& request) const;

private:
  IJumpgatePlaybackClaimTransport& m_transport;
};

} // namespace KODI::JUMPGATE
