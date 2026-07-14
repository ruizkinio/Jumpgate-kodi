/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateSubtitleClient.h"

#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/Variant.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <regex>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_JSON_RESPONSE_BYTES = 256 * 1024;
constexpr std::size_t MAX_REQUEST_BODY_BYTES = 2 * 1024;
constexpr std::size_t MAX_DISCOVERED_SUBTITLES = 128;
constexpr std::size_t MAX_LANGUAGE_PREFERENCES = 16;
constexpr std::uint64_t MAX_PART_BYTES = 64ULL * 1024ULL * 1024ULL;
constexpr std::int64_t MAX_DATE_MILLISECONDS = 8640000000000000LL;

void SecureClear(std::string& value)
{
  volatile char* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = '\0';
  value.clear();
}

void SecureClear(std::vector<std::uint8_t>& value)
{
  volatile std::uint8_t* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = 0;
  value.clear();
}

bool IsOpaqueToken(std::string_view value)
{
  if (value.size() < 32 || value.size() > 128)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-';
                     });
}

bool IsLowerHex(std::string_view value, std::size_t length)
{
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](char item)
                     { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); });
}

bool IsBoundedPrintable(std::string_view value, std::size_t maximum)
{
  if (value.empty() || value.size() > maximum)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char item) { return item >= 0x20 && item != 0x7f; });
}

bool IsLanguage(std::string_view value)
{
  if (value.empty() || value.size() > 35)
    return false;
  static const std::regex pattern("^(?:[a-z]{2,3}(?:-[a-z0-9]{2,8}){0,3}|und)$");
  return std::regex_match(value.begin(), value.end(), pattern);
}

bool IsFormat(std::string_view value)
{
  if (value.empty() || value.size() > 16)
    return false;
  return std::all_of(value.begin(), value.end(), [](char item)
                     { return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9'); });
}

bool IsContentType(std::string_view value)
{
  if (value.empty() || value.size() > 128)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char item) { return item >= 0x21 && item <= 0x7e; });
}

bool IsJsonContentType(std::string_view value)
{
  return value == "application/json" || value == "application/json; charset=utf-8";
}

bool HasOnlyMembers(const CVariant& object, std::initializer_list<std::string_view> allowed)
{
  if (!object.isObject())
    return false;
  for (auto item = object.begin_map(); item != object.end_map(); ++item)
  {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](std::string_view name) { return item->first == name; }))
    {
      return false;
    }
  }
  return true;
}

bool HasNoDuplicateObjectMembers(std::string_view json)
{
  struct Frame
  {
    bool object{false};
    bool expectingKey{false};
    std::set<std::string> members;
  };

  std::vector<Frame> frames;
  for (std::size_t index = 0; index < json.size(); ++index)
  {
    if (json[index] == '"')
    {
      const std::size_t start = index++;
      while (index < json.size() && json[index] != '"')
      {
        if (json[index] == '\\')
        {
          ++index;
          if (index == json.size())
            return false;
        }
        ++index;
      }
      if (index == json.size())
        return false;

      if (!frames.empty() && frames.back().object && frames.back().expectingKey)
      {
        CVariant member;
        const std::string encoded{json.substr(start, index - start + 1)};
        if (!CJSONVariantParser::Parse(encoded, member) || !member.isString() ||
            !frames.back().members.insert(member.asString()).second)
        {
          return false;
        }
        frames.back().expectingKey = false;
      }
      continue;
    }

    switch (json[index])
    {
      case '{':
        frames.push_back(Frame{true, true, {}});
        break;
      case '[':
        frames.push_back(Frame{});
        break;
      case '}':
      case ']':
        if (frames.empty())
          return false;
        frames.pop_back();
        break;
      case ',':
        if (!frames.empty() && frames.back().object)
          frames.back().expectingKey = true;
        break;
      default:
        break;
    }
  }
  return frames.empty();
}

bool ReadPositiveInteger(const CVariant& value, std::uint64_t maximum, std::uint64_t& result)
{
  if (!value.isInteger())
    return false;
  if (value.isSignedInteger())
  {
    const std::int64_t parsed = value.asInteger();
    if (parsed <= 0 || static_cast<std::uint64_t>(parsed) > maximum)
      return false;
    result = static_cast<std::uint64_t>(parsed);
    return true;
  }
  const std::uint64_t parsed = value.asUnsignedInteger();
  if (parsed == 0 || parsed > maximum)
    return false;
  result = parsed;
  return true;
}

std::optional<std::string_view> TextContentTypeForExtension(std::string_view extension)
{
  if (extension == ".srt")
    return "application/x-subrip";
  if (extension == ".vtt")
    return "text/vtt";
  if (extension == ".ass" || extension == ".ssa")
    return "text/x-ssa";
  if (extension == ".smi")
    return "application/x-sami";
  if (extension == ".sub")
    return "text/x-microdvd";
  if (extension == ".txt")
    return "text/plain";
  return std::nullopt;
}

std::optional<std::string_view> FileExtension(std::string_view fileName)
{
  const std::size_t dot = fileName.rfind('.');
  if (dot == std::string_view::npos)
    return std::nullopt;
  return fileName.substr(dot);
}

bool IsCanonicalFileName(std::string_view value)
{
  const std::optional<std::string_view> extension = FileExtension(value);
  if (!extension || value.size() != 64 + extension->size() || !IsLowerHex(value.substr(0, 64), 64))
    return false;
  return TextContentTypeForExtension(*extension).has_value() || *extension == ".idx";
}

std::string BuildDeliveryPath(const std::string& sessionId,
                              const std::string& artifactId,
                              std::uint32_t partNumber,
                              const std::string& fileName)
{
  return "/v1/subtitles/" + sessionId + "/" + artifactId + "/" + std::to_string(partNumber) + "/" +
         fileName;
}

bool IsCanonicalDeliveryPath(const JumpgateSubtitlePartDescriptor& descriptor)
{
  static const std::regex pattern("^/v1/subtitles/[A-Za-z0-9_-]{8,256}/[A-Za-z0-9_-]{8,256}/([12])/"
                                  "[a-f0-9]{64}\\.(?:srt|vtt|ass|ssa|smi|sub|txt|idx)$");
  std::smatch match;
  if (!std::regex_match(descriptor.path, match, pattern))
    return false;
  return static_cast<std::uint32_t>(match[1].str()[0] - '0') == descriptor.partNumber &&
         descriptor.path.size() > descriptor.fileName.size() &&
         descriptor.path.compare(descriptor.path.size() - descriptor.fileName.size(),
                                 descriptor.fileName.size(), descriptor.fileName) == 0;
}

bool IsCanonicalPartDescriptor(const JumpgateSubtitlePartDescriptor& descriptor)
{
  const std::optional<std::string_view> extension = FileExtension(descriptor.fileName);
  if (!extension || !IsCanonicalDeliveryPath(descriptor))
    return false;
  if (descriptor.role == "subtitle")
  {
    const std::optional<std::string_view> expected = TextContentTypeForExtension(*extension);
    return expected && descriptor.contentType == *expected;
  }
  if (descriptor.role == "index")
  {
    return descriptor.partNumber == 1 && *extension == ".idx" &&
           descriptor.contentType == "application/x-vobsub";
  }
  if (descriptor.role == "sub")
  {
    return descriptor.partNumber == 2 && *extension == ".sub" &&
           descriptor.contentType == "application/octet-stream";
  }
  return false;
}

bool ParseRetryAfter(std::string_view value, std::uint32_t& seconds)
{
  if (value.empty() || value.size() > 2 || (value.size() > 1 && value.front() == '0') ||
      !std::all_of(value.begin(), value.end(),
                   [](char item) { return item >= '0' && item <= '9'; }))
  {
    return false;
  }
  unsigned int parsed = 0;
  for (char item : value)
    parsed = parsed * 10 + static_cast<unsigned int>(item - '0');
  if (parsed < 1 || parsed > 60)
    return false;
  seconds = parsed;
  return true;
}

struct HttpClassification
{
  JumpgateSubtitleResultStatus status{JumpgateSubtitleResultStatus::ProtocolFailure};
  std::uint32_t retryAfterSeconds{0};
};

HttpClassification ClassifyHttpResponse(bool transportSucceeded,
                                        const JumpgateSubtitleHttpResponse& response,
                                        const std::string& expectedUrl,
                                        const CJumpgateSubtitleCancellationToken& cancellation)
{
  if (cancellation.IsCancelled())
    return {JumpgateSubtitleResultStatus::Cancelled, 0};
  if (!transportSucceeded)
    return {JumpgateSubtitleResultStatus::SoftFailure, 0};
  if (response.statusCode < 100 || response.statusCode > 599 ||
      response.effectiveUrl != expectedUrl || !response.redirectUrl.empty())
  {
    return {JumpgateSubtitleResultStatus::ProtocolFailure, 0};
  }
  if (response.statusCode == 200)
    return {JumpgateSubtitleResultStatus::Success, 0};
  if (response.statusCode == 401)
    return {JumpgateSubtitleResultStatus::RePairRequired, 0};
  if (response.statusCode == 404)
    return {JumpgateSubtitleResultStatus::Stale, 0};
  if (response.statusCode == 409 || response.statusCode == 429)
  {
    std::uint32_t retryAfterSeconds = 0;
    if (!ParseRetryAfter(response.retryAfter, retryAfterSeconds))
      return {JumpgateSubtitleResultStatus::ProtocolFailure, 0};
    return {JumpgateSubtitleResultStatus::RetryableBusy, retryAfterSeconds};
  }
  if (response.statusCode == 400 || response.statusCode == 422 ||
      (response.statusCode >= 300 && response.statusCode < 400))
  {
    return {JumpgateSubtitleResultStatus::ProtocolFailure, 0};
  }
  if (response.statusCode >= 500 && response.statusCode <= 599)
    return {JumpgateSubtitleResultStatus::SoftFailure, 0};
  return {JumpgateSubtitleResultStatus::HttpFailure, 0};
}

void PopulateAuthenticatedRequest(JumpgateSubtitleHttpMethod method,
                                  std::string url,
                                  const std::string& token,
                                  std::string body,
                                  std::size_t maximumResponseBytes,
                                  JumpgateSubtitleHttpRequest& request)
{
  request.method = method;
  request.url = std::move(url);
  if (method == JumpgateSubtitleHttpMethod::Post)
    request.contentType = "application/json";
  request.authorization.reserve(7 + token.size());
  request.authorization.append("Bearer ");
  request.authorization.append(token);
  request.body = std::move(body);
  request.maximumResponseBytes = maximumResponseBytes;
  request.followRedirects = false;
}

bool SerializeSessionRequest(const std::string& sessionId, std::string& body)
{
  CVariant root{CVariant::VariantTypeObject};
  root["sessionId"] = sessionId;
  return CJSONVariantWriter::Write(root, body, true) && body.size() <= MAX_REQUEST_BODY_BYTES;
}

bool SerializeResolveRequest(const std::string& sessionId,
                             const std::string& selector,
                             std::string& body)
{
  CVariant root{CVariant::VariantTypeObject};
  root["sessionId"] = sessionId;
  root["selector"] = selector;
  return CJSONVariantWriter::Write(root, body, true) && body.size() <= MAX_REQUEST_BODY_BYTES;
}

bool ParseDiscoverResponse(const std::vector<std::uint8_t>& bytes,
                           std::vector<JumpgateSubtitleCandidate>& candidates)
{
  if (bytes.empty() || bytes.size() > MAX_JSON_RESPONSE_BYTES ||
      std::find(bytes.begin(), bytes.end(), 0) != bytes.end())
    return false;
  const std::string body(bytes.begin(), bytes.end());
  if (!HasNoDuplicateObjectMembers(body))
    return false;

  CVariant root;
  if (!CJSONVariantParser::Parse(body, root) || !root.isObject() || root.size() != 2 ||
      !HasOnlyMembers(root, {"schemaVersion", "subtitles"}) || !root.isMember("schemaVersion") ||
      !root["schemaVersion"].isInteger() || root["schemaVersion"].asUnsignedInteger() != 1 ||
      !root.isMember("subtitles") || !root["subtitles"].isArray() ||
      root["subtitles"].size() > MAX_DISCOVERED_SUBTITLES)
  {
    return false;
  }

  std::set<std::string> selectors;
  candidates.clear();
  candidates.reserve(root["subtitles"].size());
  for (std::size_t index = 0; index < root["subtitles"].size(); ++index)
  {
    const CVariant& input = root["subtitles"][static_cast<unsigned int>(index)];
    std::uint64_t rank = 0;
    if (!input.isObject() || input.size() != 5 ||
        !HasOnlyMembers(input, {"selector", "label", "language", "format", "rank"}) ||
        !input.isMember("selector") || !input["selector"].isString() ||
        !IsLowerHex(input["selector"].asString(), 64) ||
        !selectors.insert(input["selector"].asString()).second || !input.isMember("label") ||
        !input["label"].isString() || !IsBoundedPrintable(input["label"].asString(), 64) ||
        !input.isMember("language") || !input["language"].isString() ||
        !IsLanguage(input["language"].asString()) || !input.isMember("format") ||
        !input["format"].isString() || !IsFormat(input["format"].asString()) ||
        !input.isMember("rank") ||
        !ReadPositiveInteger(input["rank"], MAX_DISCOVERED_SUBTITLES, rank) || rank != index + 1)
    {
      candidates.clear();
      return false;
    }
    candidates.push_back({input["selector"].asString(), input["label"].asString(),
                          input["language"].asString(), input["format"].asString(),
                          static_cast<std::uint32_t>(rank)});
  }
  return true;
}

bool ParseResolveResponse(const std::vector<std::uint8_t>& bytes,
                          const std::string& sessionId,
                          JumpgateSubtitleArtifactDescriptor& artifact)
{
  if (bytes.empty() || bytes.size() > MAX_JSON_RESPONSE_BYTES ||
      std::find(bytes.begin(), bytes.end(), 0) != bytes.end())
    return false;
  const std::string body(bytes.begin(), bytes.end());
  if (!HasNoDuplicateObjectMembers(body))
    return false;

  CVariant root;
  std::uint64_t expiresAt = 0;
  if (!CJSONVariantParser::Parse(body, root) || !root.isObject() || root.size() != 6 ||
      !HasOnlyMembers(
          root, {"schemaVersion", "status", "artifactId", "expiresAt", "expiresAtUnit", "parts"}) ||
      !root.isMember("schemaVersion") || !root["schemaVersion"].isInteger() ||
      root["schemaVersion"].asUnsignedInteger() != 1 || !root.isMember("status") ||
      !root["status"].isString() || root["status"].asString() != "ready" ||
      !root.isMember("artifactId") || !root["artifactId"].isString() ||
      !CJumpgateSubtitleClient::IsCanonicalRouteIdentifier(root["artifactId"].asString()) ||
      !root.isMember("expiresAt") ||
      !ReadPositiveInteger(root["expiresAt"], MAX_DATE_MILLISECONDS, expiresAt) ||
      !root.isMember("expiresAtUnit") || !root["expiresAtUnit"].isString() ||
      root["expiresAtUnit"].asString() != "unix_ms" || !root.isMember("parts") ||
      !root["parts"].isArray() || root["parts"].empty() || root["parts"].size() > 2)
  {
    return false;
  }

  JumpgateSubtitleArtifactDescriptor parsed;
  parsed.artifactId = root["artifactId"].asString();
  parsed.expiresAt = static_cast<std::int64_t>(expiresAt);
  parsed.parts.reserve(root["parts"].size());
  for (std::size_t index = 0; index < root["parts"].size(); ++index)
  {
    const CVariant& input = root["parts"][static_cast<unsigned int>(index)];
    std::uint64_t partNumber = 0;
    std::uint64_t contentLength = 0;
    if (!input.isObject() || input.size() != 6 ||
        !HasOnlyMembers(
            input, {"partNumber", "role", "contentLength", "contentType", "fileName", "path"}) ||
        !input.isMember("partNumber") || !ReadPositiveInteger(input["partNumber"], 2, partNumber) ||
        partNumber != index + 1 || !input.isMember("role") || !input["role"].isString() ||
        !IsFormat(input["role"].asString()) || !input.isMember("contentLength") ||
        !ReadPositiveInteger(input["contentLength"], MAX_PART_BYTES, contentLength) ||
        !input.isMember("contentType") || !input["contentType"].isString() ||
        !IsContentType(input["contentType"].asString()) || !input.isMember("fileName") ||
        !input["fileName"].isString() || !IsCanonicalFileName(input["fileName"].asString()) ||
        !input.isMember("path") || !input["path"].isString())
    {
      return false;
    }

    JumpgateSubtitlePartDescriptor part;
    part.partNumber = static_cast<std::uint32_t>(partNumber);
    part.role = input["role"].asString();
    part.contentLength = contentLength;
    part.contentType = input["contentType"].asString();
    part.fileName = input["fileName"].asString();
    part.path = input["path"].asString();
    if (part.path !=
            BuildDeliveryPath(sessionId, parsed.artifactId, part.partNumber, part.fileName) ||
        !IsCanonicalDeliveryPath(part))
    {
      return false;
    }
    parsed.parts.emplace_back(std::move(part));
  }

  if (parsed.parts.size() == 1)
  {
    const std::optional<std::string_view> extension = FileExtension(parsed.parts[0].fileName);
    const std::optional<std::string_view> expected =
        extension ? TextContentTypeForExtension(*extension) : std::nullopt;
    if (parsed.parts[0].role != "subtitle" || !expected || parsed.parts[0].contentType != *expected)
    {
      return false;
    }
  }
  else
  {
    const JumpgateSubtitlePartDescriptor& index = parsed.parts[0];
    const JumpgateSubtitlePartDescriptor& sub = parsed.parts[1];
    if (index.role != "index" || index.contentType != "application/x-vobsub" ||
        FileExtension(index.fileName) != std::optional<std::string_view>{".idx"} ||
        sub.role != "sub" || sub.contentType != "application/octet-stream" ||
        FileExtension(sub.fileName) != std::optional<std::string_view>{".sub"} ||
        index.fileName.substr(0, 64) != sub.fileName.substr(0, 64))
    {
      return false;
    }
  }

  artifact = std::move(parsed);
  return true;
}

std::string_view PrimaryLanguage(std::string_view language)
{
  const std::size_t separator = language.find('-');
  return language.substr(0, separator);
}

} // namespace

CJumpgateSubtitleCancellationToken::CJumpgateSubtitleCancellationToken(
    std::shared_ptr<std::atomic<bool>> state)
  : m_state(std::move(state))
{
}

bool CJumpgateSubtitleCancellationToken::IsCancelled() const
{
  return m_state && m_state->load(std::memory_order_acquire);
}

CJumpgateSubtitleCancellationSource::CJumpgateSubtitleCancellationSource()
  : m_state(std::make_shared<std::atomic<bool>>(false))
{
}

CJumpgateSubtitleCancellationToken CJumpgateSubtitleCancellationSource::Token() const
{
  return CJumpgateSubtitleCancellationToken{m_state};
}

void CJumpgateSubtitleCancellationSource::Cancel()
{
  m_state->store(true, std::memory_order_release);
}

CJumpgateSubtitleBearerAuthority::CJumpgateSubtitleBearerAuthority(std::string token)
  : m_token(std::move(token))
{
}

CJumpgateSubtitleBearerAuthority::~CJumpgateSubtitleBearerAuthority()
{
  SecureClear(m_token);
}

CJumpgateSubtitleBearerAuthority::CJumpgateSubtitleBearerAuthority(
    CJumpgateSubtitleBearerAuthority&& other) noexcept
  : m_token(std::move(other.m_token))
{
  SecureClear(other.m_token);
}

CJumpgateSubtitleBearerAuthority& CJumpgateSubtitleBearerAuthority::operator=(
    CJumpgateSubtitleBearerAuthority&& other) noexcept
{
  if (this != &other)
  {
    SecureClear(m_token);
    m_token = std::move(other.m_token);
    SecureClear(other.m_token);
  }
  return *this;
}

bool CJumpgateSubtitleBearerAuthority::IsPresent() const
{
  return !m_token.empty();
}

bool CJumpgateSubtitleBearerAuthority::IsValid() const
{
  return IsOpaqueToken(m_token);
}

std::string CJumpgateSubtitleBearerAuthority::RedactedSummary() const
{
  return m_token.empty() ? "bearer=<absent>" : "bearer=<redacted>";
}

JumpgateSubtitleHttpRequest::~JumpgateSubtitleHttpRequest()
{
  ClearSensitive();
}

void JumpgateSubtitleHttpRequest::ClearSensitive()
{
  SecureClear(url);
  SecureClear(authorization);
  SecureClear(body);
}

std::string JumpgateSubtitleHttpRequest::RedactedSummary() const
{
  const char* methodName = method == JumpgateSubtitleHttpMethod::Get ? "GET" : "POST";
  return std::string{"subtitle_http method="} + methodName + " url=<redacted> bearer=<redacted>";
}

JumpgateSubtitleHttpResponse::~JumpgateSubtitleHttpResponse()
{
  ClearSensitive();
}

void JumpgateSubtitleHttpResponse::ClearSensitive()
{
  SecureClear(body);
  SecureClear(effectiveUrl);
  SecureClear(redirectUrl);
  retryAfter.clear();
}

CJumpgateSubtitleClient::CJumpgateSubtitleClient(IJumpgateSubtitleTransport& transport)
  : m_transport(transport)
{
}

JumpgateSubtitleDiscoverResult CJumpgateSubtitleClient::Discover(
    const std::string& bridgeOrigin,
    const CJumpgateSubtitleBearerAuthority& authority,
    const std::string& sessionId,
    const CJumpgateSubtitleCancellationToken& cancellation) const
{
  JumpgateSubtitleDiscoverResult result;
  if (!IsCanonicalOrigin(bridgeOrigin) || !IsOpaqueToken(authority.m_token) ||
      !IsCanonicalRouteIdentifier(sessionId))
  {
    result.status = JumpgateSubtitleResultStatus::InvalidRequest;
    return result;
  }
  if (cancellation.IsCancelled())
  {
    result.status = JumpgateSubtitleResultStatus::Cancelled;
    return result;
  }

  std::string body;
  if (!SerializeSessionRequest(sessionId, body))
  {
    result.status = JumpgateSubtitleResultStatus::InvalidRequest;
    return result;
  }
  const std::string url = bridgeOrigin + "/v1/subtitles/discover";
  JumpgateSubtitleHttpRequest request;
  PopulateAuthenticatedRequest(JumpgateSubtitleHttpMethod::Post, url, authority.m_token,
                               std::move(body), MAX_JSON_RESPONSE_BYTES, request);
  JumpgateSubtitleHttpResponse response;
  const bool succeeded = m_transport.Perform(request, response, cancellation);
  request.ClearSensitive();
  const HttpClassification classification =
      ClassifyHttpResponse(succeeded, response, url, cancellation);
  result.status = classification.status;
  result.httpStatus = response.statusCode;
  result.retryAfterSeconds = classification.retryAfterSeconds;
  if (result.status != JumpgateSubtitleResultStatus::Success)
    return result;
  if (response.body.size() > request.maximumResponseBytes ||
      !IsJsonContentType(response.contentType) ||
      !ParseDiscoverResponse(response.body, result.candidates))
  {
    result.status = JumpgateSubtitleResultStatus::ProtocolFailure;
  }
  return result;
}

JumpgateSubtitleResolveResult CJumpgateSubtitleClient::Resolve(
    const std::string& bridgeOrigin,
    const CJumpgateSubtitleBearerAuthority& authority,
    const std::string& sessionId,
    const std::string& selector,
    const CJumpgateSubtitleCancellationToken& cancellation) const
{
  JumpgateSubtitleResolveResult result;
  if (!IsCanonicalOrigin(bridgeOrigin) || !IsOpaqueToken(authority.m_token) ||
      !IsCanonicalRouteIdentifier(sessionId) || !IsLowerHex(selector, 64))
  {
    result.status = JumpgateSubtitleResultStatus::InvalidRequest;
    return result;
  }
  if (cancellation.IsCancelled())
  {
    result.status = JumpgateSubtitleResultStatus::Cancelled;
    return result;
  }

  std::string body;
  if (!SerializeResolveRequest(sessionId, selector, body))
  {
    result.status = JumpgateSubtitleResultStatus::InvalidRequest;
    return result;
  }
  const std::string url = bridgeOrigin + "/v1/subtitles/resolve";
  JumpgateSubtitleHttpRequest request;
  PopulateAuthenticatedRequest(JumpgateSubtitleHttpMethod::Post, url, authority.m_token,
                               std::move(body), MAX_JSON_RESPONSE_BYTES, request);
  JumpgateSubtitleHttpResponse response;
  const bool succeeded = m_transport.Perform(request, response, cancellation);
  request.ClearSensitive();
  const HttpClassification classification =
      ClassifyHttpResponse(succeeded, response, url, cancellation);
  result.status = classification.status;
  result.httpStatus = response.statusCode;
  result.retryAfterSeconds = classification.retryAfterSeconds;
  if (result.status != JumpgateSubtitleResultStatus::Success)
    return result;
  if (response.body.size() > request.maximumResponseBytes ||
      !IsJsonContentType(response.contentType) ||
      !ParseResolveResponse(response.body, sessionId, result.artifact))
  {
    result.status = JumpgateSubtitleResultStatus::ProtocolFailure;
  }
  return result;
}

JumpgateSubtitlePartResult CJumpgateSubtitleClient::Download(
    const std::string& bridgeOrigin,
    const CJumpgateSubtitleBearerAuthority& authority,
    const JumpgateSubtitlePartDescriptor& descriptor,
    const CJumpgateSubtitleCancellationToken& cancellation) const
{
  JumpgateSubtitlePartResult result;
  if (!IsCanonicalOrigin(bridgeOrigin) || !IsOpaqueToken(authority.m_token) ||
      descriptor.partNumber < 1 || descriptor.partNumber > 2 || descriptor.contentLength < 1 ||
      descriptor.contentLength > MAX_PART_BYTES || !IsContentType(descriptor.contentType) ||
      !IsCanonicalFileName(descriptor.fileName) || !IsCanonicalPartDescriptor(descriptor))
  {
    result.status = JumpgateSubtitleResultStatus::InvalidRequest;
    return result;
  }
  if (cancellation.IsCancelled())
  {
    result.status = JumpgateSubtitleResultStatus::Cancelled;
    return result;
  }

  const std::string url = bridgeOrigin + descriptor.path;
  JumpgateSubtitleHttpRequest request;
  PopulateAuthenticatedRequest(JumpgateSubtitleHttpMethod::Get, url, authority.m_token, {},
                               static_cast<std::size_t>(descriptor.contentLength), request);
  JumpgateSubtitleHttpResponse response;
  const bool succeeded = m_transport.Perform(request, response, cancellation);
  request.ClearSensitive();
  const HttpClassification classification =
      ClassifyHttpResponse(succeeded, response, url, cancellation);
  result.status = classification.status;
  result.httpStatus = response.statusCode;
  result.retryAfterSeconds = classification.retryAfterSeconds;
  if (result.status != JumpgateSubtitleResultStatus::Success)
    return result;
  if (!response.contentLength || *response.contentLength != descriptor.contentLength ||
      response.contentType != descriptor.contentType ||
      response.body.size() != descriptor.contentLength ||
      response.body.size() > request.maximumResponseBytes)
  {
    result.status = JumpgateSubtitleResultStatus::ProtocolFailure;
    return result;
  }

  result.part.partNumber = descriptor.partNumber;
  result.part.role = descriptor.role;
  result.part.contentType = descriptor.contentType;
  result.part.fileName = descriptor.fileName;
  result.part.sha256 = KODI::UTILITY::CDigest::Calculate(
      KODI::UTILITY::CDigest::Type::SHA256, response.body.data(), response.body.size());
  if (!IsLowerHex(result.part.sha256, 64))
  {
    result.status = JumpgateSubtitleResultStatus::ProtocolFailure;
    return result;
  }
  result.part.bytes = std::move(response.body);
  return result;
}

bool CJumpgateSubtitleClient::IsCanonicalOrigin(const std::string& origin)
{
  if (origin.empty() || origin.size() > 512)
    return false;
  static const std::regex originPattern(
      "^(https|http)://"
      "(localhost|127\\.0\\.0\\.1|\\[::1\\]|[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}"
      "[A-Za-z0-9])?)(?::([0-9]{1,5}))?$",
      std::regex::icase);
  std::smatch match;
  if (!std::regex_match(origin, match, originPattern))
    return false;

  std::string scheme = match[1].str();
  std::string host = match[2].str();
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
  const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
  if (scheme != "https" && !(scheme == "http" && loopback))
    return false;
  if (match[3].matched)
  {
    const int port = std::atoi(match[3].str().c_str());
    if (port < 1 || port > 65535)
      return false;
  }
  std::string normalized = scheme + "://" + host;
  if (match[3].matched)
    normalized += ":" + match[3].str();
  return normalized == origin;
}

bool CJumpgateSubtitleClient::IsCanonicalRouteIdentifier(const std::string& value)
{
  if (value.size() < 8 || value.size() > 256)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-';
                     });
}

bool CJumpgateSubtitleClient::IsCanonicalLanguage(const std::string& value)
{
  return IsLanguage(value);
}

std::optional<JumpgateSubtitleCandidate> SelectJumpgateSubtitleCandidate(
    const std::vector<JumpgateSubtitleCandidate>& candidates,
    const std::vector<std::string>& languagePreferences)
{
  if (languagePreferences.size() > MAX_LANGUAGE_PREFERENCES ||
      !std::all_of(languagePreferences.begin(), languagePreferences.end(),
                   [](const std::string& language) { return IsLanguage(language); }))
  {
    return std::nullopt;
  }

  using Score = std::tuple<std::size_t, unsigned int, std::uint32_t, std::string>;
  std::optional<Score> bestScore;
  std::optional<JumpgateSubtitleCandidate> best;
  for (const JumpgateSubtitleCandidate& candidate : candidates)
  {
    if (!IsLowerHex(candidate.selector, 64) || !IsBoundedPrintable(candidate.label, 64) ||
        !IsLanguage(candidate.language) || !IsFormat(candidate.format) || candidate.rank < 1 ||
        candidate.rank > MAX_DISCOVERED_SUBTITLES)
    {
      return std::nullopt;
    }

    std::size_t preference = languagePreferences.size();
    unsigned int matchQuality = 2;
    for (std::size_t index = 0; index < languagePreferences.size(); ++index)
    {
      unsigned int quality = 2;
      if (candidate.language == languagePreferences[index])
        quality = 0;
      else if (candidate.language != "und" && languagePreferences[index] != "und" &&
               PrimaryLanguage(candidate.language) == PrimaryLanguage(languagePreferences[index]))
        quality = 1;
      if (quality < 2)
      {
        preference = index;
        matchQuality = quality;
        break;
      }
    }
    const Score score{preference, matchQuality, candidate.rank, candidate.selector};
    if (!bestScore || score < *bestScore)
    {
      bestScore = score;
      best = candidate;
    }
  }
  return best;
}

} // namespace KODI::JUMPGATE
