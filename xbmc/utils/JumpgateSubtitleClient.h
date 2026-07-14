/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

class CJumpgateSubtitleCancellationToken final
{
public:
  CJumpgateSubtitleCancellationToken() = default;

  bool IsCancelled() const;

private:
  explicit CJumpgateSubtitleCancellationToken(std::shared_ptr<std::atomic<bool>> state);

  std::shared_ptr<std::atomic<bool>> m_state;

  friend class CJumpgateSubtitleCancellationSource;
};

class CJumpgateSubtitleCancellationSource final
{
public:
  CJumpgateSubtitleCancellationSource();

  CJumpgateSubtitleCancellationToken Token() const;
  void Cancel();

private:
  std::shared_ptr<std::atomic<bool>> m_state;
};

class CJumpgateSubtitleBearerAuthority final
{
public:
  explicit CJumpgateSubtitleBearerAuthority(std::string token);
  ~CJumpgateSubtitleBearerAuthority();

  CJumpgateSubtitleBearerAuthority(const CJumpgateSubtitleBearerAuthority&) = delete;
  CJumpgateSubtitleBearerAuthority& operator=(const CJumpgateSubtitleBearerAuthority&) = delete;
  CJumpgateSubtitleBearerAuthority(CJumpgateSubtitleBearerAuthority&& other) noexcept;
  CJumpgateSubtitleBearerAuthority& operator=(CJumpgateSubtitleBearerAuthority&& other) noexcept;

  bool IsPresent() const;
  bool IsValid() const;
  std::string RedactedSummary() const;

private:
  std::string m_token;

  friend class CJumpgateSubtitleClient;
};

enum class JumpgateSubtitleHttpMethod : uint8_t
{
  Post,
  Get,
};

struct JumpgateSubtitleHttpRequest final
{
  JumpgateSubtitleHttpRequest() = default;
  JumpgateSubtitleHttpRequest(const JumpgateSubtitleHttpRequest&) = delete;
  JumpgateSubtitleHttpRequest& operator=(const JumpgateSubtitleHttpRequest&) = delete;
  ~JumpgateSubtitleHttpRequest();

  void ClearSensitive();
  std::string RedactedSummary() const;

  JumpgateSubtitleHttpMethod method{JumpgateSubtitleHttpMethod::Post};
  std::string url;
  std::string contentType;
  std::string authorization;
  std::string body;
  std::size_t maximumResponseBytes{0};
  bool followRedirects{false};
};

struct JumpgateSubtitleHttpResponse final
{
  JumpgateSubtitleHttpResponse() = default;
  JumpgateSubtitleHttpResponse(const JumpgateSubtitleHttpResponse&) = delete;
  JumpgateSubtitleHttpResponse& operator=(const JumpgateSubtitleHttpResponse&) = delete;
  ~JumpgateSubtitleHttpResponse();

  void ClearSensitive();

  int statusCode{0};
  std::string effectiveUrl;
  std::string redirectUrl;
  std::string contentType;
  std::optional<std::uint64_t> contentLength;
  std::string retryAfter;
  std::vector<std::uint8_t> body;
};

class IJumpgateSubtitleTransport
{
public:
  virtual ~IJumpgateSubtitleTransport() = default;

  // Implementations must not follow redirects, retain bearer authority, or exceed the response cap.
  virtual bool Perform(const JumpgateSubtitleHttpRequest& request,
                       JumpgateSubtitleHttpResponse& response,
                       const CJumpgateSubtitleCancellationToken& cancellation) = 0;
};

enum class JumpgateSubtitleResultStatus : uint8_t
{
  Success,
  NoMatch,
  InvalidRequest,
  RePairRequired,
  Stale,
  RetryableBusy,
  ProtocolFailure,
  SoftFailure,
  HttpFailure,
  Cancelled,
};

struct JumpgateSubtitleCandidate
{
  std::string selector;
  std::string label;
  std::string language;
  std::string format;
  std::uint32_t rank{0};
};

struct JumpgateSubtitlePartDescriptor
{
  std::uint32_t partNumber{0};
  std::string role;
  std::uint64_t contentLength{0};
  std::string contentType;
  std::string fileName;
  std::string path;
};

struct JumpgateSubtitleArtifactDescriptor
{
  std::string artifactId;
  std::int64_t expiresAt{0};
  std::vector<JumpgateSubtitlePartDescriptor> parts;
};

struct JumpgateSubtitleStagedPart
{
  std::uint32_t partNumber{0};
  std::string role;
  std::string contentType;
  std::string fileName;
  std::string sha256;
  std::vector<std::uint8_t> bytes;
};

struct JumpgateSubtitleDiscoverResult
{
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  int httpStatus{0};
  std::uint32_t retryAfterSeconds{0};
  std::vector<JumpgateSubtitleCandidate> candidates;
};

struct JumpgateSubtitleResolveResult
{
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  int httpStatus{0};
  std::uint32_t retryAfterSeconds{0};
  JumpgateSubtitleArtifactDescriptor artifact;
};

struct JumpgateSubtitlePartResult
{
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  int httpStatus{0};
  std::uint32_t retryAfterSeconds{0};
  JumpgateSubtitleStagedPart part;
};

class CJumpgateSubtitleClient final
{
public:
  explicit CJumpgateSubtitleClient(IJumpgateSubtitleTransport& transport);

  JumpgateSubtitleDiscoverResult Discover(
      const std::string& bridgeOrigin,
      const CJumpgateSubtitleBearerAuthority& authority,
      const std::string& sessionId,
      const CJumpgateSubtitleCancellationToken& cancellation = {}) const;
  JumpgateSubtitleResolveResult Resolve(
      const std::string& bridgeOrigin,
      const CJumpgateSubtitleBearerAuthority& authority,
      const std::string& sessionId,
      const std::string& selector,
      const CJumpgateSubtitleCancellationToken& cancellation = {}) const;
  JumpgateSubtitlePartResult Download(
      const std::string& bridgeOrigin,
      const CJumpgateSubtitleBearerAuthority& authority,
      const JumpgateSubtitlePartDescriptor& descriptor,
      const CJumpgateSubtitleCancellationToken& cancellation = {}) const;

  static bool IsCanonicalOrigin(const std::string& origin);
  static bool IsCanonicalRouteIdentifier(const std::string& value);
  static bool IsCanonicalLanguage(const std::string& value);

private:
  IJumpgateSubtitleTransport& m_transport;
};

std::optional<JumpgateSubtitleCandidate> SelectJumpgateSubtitleCandidate(
    const std::vector<JumpgateSubtitleCandidate>& candidates,
    const std::vector<std::string>& languagePreferences);

} // namespace KODI::JUMPGATE
