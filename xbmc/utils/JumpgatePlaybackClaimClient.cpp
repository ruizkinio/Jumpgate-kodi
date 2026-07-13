/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackClaimClient.h"

#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_FINGERPRINTS = 32;
constexpr std::size_t MAX_FINGERPRINT_LENGTH = 512;
constexpr std::size_t MAX_REQUEST_BODY_BYTES = 8 * 1024;
constexpr std::size_t MAX_RESPONSE_BODY_BYTES = 2 * 1024 * 1024;
constexpr std::int64_t MAX_DATE_MILLISECONDS = 8640000000000000LL;

void SecureClear(std::string& value)
{
  volatile char* data = value.empty() ? nullptr : value.data();
  for (std::size_t index = 0; index < value.size(); ++index)
    data[index] = '\0';
  value.clear();
}

bool IsOpaqueAscii(std::string_view value, std::size_t minimum, std::size_t maximum)
{
  if (value.size() < minimum || value.size() > maximum)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-';
                     });
}

bool IsClientValue(std::string_view value, std::size_t maximum)
{
  if (value.empty() || value.size() > maximum)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-' ||
                              item == '.' || item == '+';
                     });
}

bool IsExactFingerprint(std::string_view value)
{
  if (value.empty() || value.size() > MAX_FINGERPRINT_LENGTH)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char item) { return item >= 0x21 && item <= 0x7e; });
}

bool IsLowerHexDigest(std::string_view value)
{
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char item)
                     { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); });
}

bool IsCanonicalOrigin(const std::string& origin)
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

bool HasUniqueTopLevelMembers(std::string_view json)
{
  int objectDepth = 0;
  int arrayDepth = 0;
  bool expectingKey = false;
  std::set<std::string> members;

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

      if (objectDepth == 1 && arrayDepth == 0 && expectingKey)
      {
        CVariant member;
        const std::string encoded{json.substr(start, index - start + 1)};
        if (!CJSONVariantParser::Parse(encoded, member) || !member.isString() ||
            !members.insert(member.asString()).second)
        {
          return false;
        }
        expectingKey = false;
      }
      continue;
    }

    switch (json[index])
    {
      case '{':
        ++objectDepth;
        if (objectDepth == 1 && arrayDepth == 0)
          expectingKey = true;
        break;
      case '}':
        --objectDepth;
        break;
      case '[':
        ++arrayDepth;
        break;
      case ']':
        --arrayDepth;
        break;
      case ',':
        if (objectDepth == 1 && arrayDepth == 0)
          expectingKey = true;
        break;
      default:
        break;
    }
  }
  return true;
}

bool ReadDigits(std::string_view value, std::size_t offset, std::size_t count, int& result)
{
  result = 0;
  for (std::size_t index = offset; index < offset + count; ++index)
  {
    if (value[index] < '0' || value[index] > '9')
      return false;
    result = result * 10 + value[index] - '0';
  }
  return true;
}

bool IsLeapYear(int year)
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool IsCanonicalTimestamp(std::string_view value)
{
  if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z')
  {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;
  if (!ReadDigits(value, 0, 4, year) || !ReadDigits(value, 5, 2, month) ||
      !ReadDigits(value, 8, 2, day) || !ReadDigits(value, 11, 2, hour) ||
      !ReadDigits(value, 14, 2, minute) || !ReadDigits(value, 17, 2, second) ||
      !ReadDigits(value, 20, 3, millisecond) || year < 1970 || month < 1 || month > 12 ||
      hour > 23 || minute > 59 || second > 59 || millisecond > 999)
  {
    return false;
  }

  constexpr std::array<int, 12> DAYS_IN_MONTH{{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
  int maximumDay = DAYS_IN_MONTH[month - 1];
  if (month == 2 && IsLeapYear(year))
    maximumDay = 29;
  return day >= 1 && day <= maximumDay;
}

bool IsValidClaimRequest(const PlaybackClaimRequest& request)
{
  if (!IsCanonicalOrigin(request.bridgeOrigin) || !IsOpaqueAscii(request.deviceToken, 32, 128) ||
      request.fingerprints.empty() || request.fingerprints.size() > MAX_FINGERPRINTS ||
      !std::all_of(request.fingerprints.begin(), request.fingerprints.end(), IsExactFingerprint) ||
      !IsLowerHexDigest(request.intentUrlHash) || request.launchedAt <= 0 ||
      request.launchedAt > MAX_DATE_MILLISECONDS)
  {
    return false;
  }

  return !request.client || (IsClientValue(request.client->platform, 64) &&
                             IsClientValue(request.client->version, 128));
}

bool IsValidReleaseRequest(const PlaybackReleaseRequest& request)
{
  return IsCanonicalOrigin(request.bridgeOrigin) && IsOpaqueAscii(request.deviceToken, 32, 128) &&
         IsOpaqueAscii(request.sessionId, 8, 128);
}

bool SerializeClaimRequest(const PlaybackClaimRequest& request, std::string& body)
{
  CVariant root{CVariant::VariantTypeObject};
  root["fingerprints"] = request.fingerprints;
  root["intentUrlHash"] = request.intentUrlHash;
  root["launchedAt"] = request.launchedAt;
  if (request.client)
  {
    CVariant client{CVariant::VariantTypeObject};
    client["platform"] = request.client->platform;
    client["version"] = request.client->version;
    root["client"] = std::move(client);
  }
  return CJSONVariantWriter::Write(root, body, true) && body.size() <= MAX_REQUEST_BODY_BYTES;
}

bool SerializeReleaseRequest(const PlaybackReleaseRequest& request, std::string& body)
{
  CVariant root{CVariant::VariantTypeObject};
  root["sessionId"] = request.sessionId;
  return CJSONVariantWriter::Write(root, body, true) && body.size() <= MAX_REQUEST_BODY_BYTES;
}

void PopulateHttpRequest(std::string url,
                         const std::string& deviceToken,
                         std::string body,
                         JumpgatePlaybackHttpRequest& request)
{
  request.url = std::move(url);
  request.contentType = "application/json";
  request.authorization.reserve(7 + deviceToken.size());
  request.authorization.append("Bearer ");
  request.authorization.append(deviceToken);
  request.body = std::move(body);
  request.followRedirects = false;
}

bool ParseClaimResponse(const std::string& body, PlaybackClaimResult& result)
{
  if (body.empty() || body.size() > MAX_RESPONSE_BODY_BYTES)
    return false;

  CVariant root;
  if (!CJSONVariantParser::Parse(body, root) || !root.isObject() || !root.isMember("status") ||
      !root["status"].isString() || !HasUniqueTopLevelMembers(body))
  {
    return false;
  }

  const std::string status = root["status"].asString();
  if (status == "ambiguous" || status == "expired" || status == "not_found")
  {
    if (root.size() != 1 || !HasOnlyMembers(root, {"status"}))
      return false;
    if (status == "ambiguous")
      result.status = PlaybackClaimStatus::Ambiguous;
    else if (status == "expired")
      result.status = PlaybackClaimStatus::Expired;
    else
      result.status = PlaybackClaimStatus::NotFound;
    return true;
  }

  if (status != "claimed" || root.size() != 5 ||
      !HasOnlyMembers(root, {"status", "sessionId", "context", "claimedAt", "expiresAt"}) ||
      !root.isMember("sessionId") || !root["sessionId"].isString() ||
      !IsOpaqueAscii(root["sessionId"].asString(), 8, 128) || !root.isMember("context") ||
      !root["context"].isObject() || root["context"].empty() || !root.isMember("claimedAt") ||
      !root["claimedAt"].isString() || !IsCanonicalTimestamp(root["claimedAt"].asString()) ||
      !root.isMember("expiresAt") || !root["expiresAt"].isString() ||
      !IsCanonicalTimestamp(root["expiresAt"].asString()) ||
      root["expiresAt"].asString() <= root["claimedAt"].asString())
  {
    return false;
  }

  result.status = PlaybackClaimStatus::Claimed;
  result.claim.sessionId = root["sessionId"].asString();
  result.claim.context = root["context"];
  result.claim.claimedAt = root["claimedAt"].asString();
  result.claim.expiresAt = root["expiresAt"].asString();
  return true;
}

bool ParseReleaseResponse(const std::string& body, PlaybackReleaseResult& result)
{
  if (body.empty() || body.size() > MAX_RESPONSE_BODY_BYTES)
    return false;

  CVariant root;
  if (!CJSONVariantParser::Parse(body, root) || root.size() != 1 ||
      !HasOnlyMembers(root, {"status"}) || !root.isMember("status") || !root["status"].isString() ||
      !HasUniqueTopLevelMembers(body))
  {
    return false;
  }

  const std::string status = root["status"].asString();
  if (status == "released")
    result.status = PlaybackReleaseStatus::Released;
  else if (status == "not_found")
    result.status = PlaybackReleaseStatus::NotFound;
  else
    return false;
  return true;
}

} // namespace

JumpgatePlaybackHttpRequest::~JumpgatePlaybackHttpRequest()
{
  ClearSensitive();
}

void JumpgatePlaybackHttpRequest::ClearSensitive()
{
  SecureClear(authorization);
  SecureClear(body);
}

JumpgatePlaybackHttpResponse::~JumpgatePlaybackHttpResponse()
{
  ClearSensitive();
}

void JumpgatePlaybackHttpResponse::ClearSensitive()
{
  SecureClear(body);
}

CJumpgatePlaybackClaimClient::CJumpgatePlaybackClaimClient(
    IJumpgatePlaybackClaimTransport& transport)
  : m_transport(transport)
{
}

PlaybackClaimResult CJumpgatePlaybackClaimClient::Claim(const PlaybackClaimRequest& request) const
{
  PlaybackClaimResult result;
  if (!IsValidClaimRequest(request))
  {
    result.status = PlaybackClaimStatus::InvalidRequest;
    return result;
  }

  std::string body;
  if (!SerializeClaimRequest(request, body))
  {
    SecureClear(body);
    result.status = PlaybackClaimStatus::InvalidRequest;
    return result;
  }

  JumpgatePlaybackHttpRequest httpRequest;
  PopulateHttpRequest(request.bridgeOrigin + "/v1/playback/claim", request.deviceToken,
                      std::move(body), httpRequest);
  JumpgatePlaybackHttpResponse response;
  if (!m_transport.Post(httpRequest, response))
  {
    httpRequest.ClearSensitive();
    result.status = PlaybackClaimStatus::TransportFailure;
    return result;
  }
  httpRequest.ClearSensitive();

  result.httpStatus = response.statusCode;
  if (response.statusCode < 100 || response.statusCode > 599)
  {
    result.status = PlaybackClaimStatus::InvalidResponse;
    return result;
  }
  if (response.statusCode == 401 || response.statusCode == 403)
  {
    result.status = PlaybackClaimStatus::AuthenticationFailure;
    return result;
  }
  if (response.statusCode != 200)
  {
    result.status = PlaybackClaimStatus::HttpFailure;
    return result;
  }

  PlaybackClaimResult parsed;
  parsed.httpStatus = response.statusCode;
  if (!ParseClaimResponse(response.body, parsed))
  {
    result.status = PlaybackClaimStatus::InvalidResponse;
    return result;
  }
  response.ClearSensitive();
  return parsed;
}

PlaybackReleaseResult CJumpgatePlaybackClaimClient::Release(
    const PlaybackReleaseRequest& request) const
{
  PlaybackReleaseResult result;
  if (!IsValidReleaseRequest(request))
  {
    result.status = PlaybackReleaseStatus::InvalidRequest;
    return result;
  }

  std::string body;
  if (!SerializeReleaseRequest(request, body))
  {
    SecureClear(body);
    result.status = PlaybackReleaseStatus::InvalidRequest;
    return result;
  }

  JumpgatePlaybackHttpRequest httpRequest;
  PopulateHttpRequest(request.bridgeOrigin + "/v1/playback/release", request.deviceToken,
                      std::move(body), httpRequest);
  JumpgatePlaybackHttpResponse response;
  if (!m_transport.Post(httpRequest, response))
  {
    httpRequest.ClearSensitive();
    result.status = PlaybackReleaseStatus::TransportFailure;
    return result;
  }
  httpRequest.ClearSensitive();

  result.httpStatus = response.statusCode;
  if (response.statusCode < 100 || response.statusCode > 599)
  {
    result.status = PlaybackReleaseStatus::InvalidResponse;
    return result;
  }
  if (response.statusCode == 401 || response.statusCode == 403)
  {
    result.status = PlaybackReleaseStatus::AuthenticationFailure;
    return result;
  }
  if (response.statusCode != 200)
  {
    result.status = PlaybackReleaseStatus::HttpFailure;
    return result;
  }

  PlaybackReleaseResult parsed;
  parsed.httpStatus = response.statusCode;
  if (!ParseReleaseResponse(response.body, parsed))
  {
    result.status = PlaybackReleaseStatus::InvalidResponse;
    return result;
  }
  response.ClearSensitive();
  return parsed;
}

} // namespace KODI::JUMPGATE
