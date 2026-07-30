/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateHistoryEventClient.h"

#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <regex>
#include <set>
#include <string_view>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_REQUEST_BODY_BYTES = 12 * 1024;
constexpr std::size_t MAX_RESPONSE_BODY_BYTES = 2 * 1024 * 1024;
constexpr std::uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;

const char* ToString(JumpgateHistoryEvent event)
{
  switch (event)
  {
    case JumpgateHistoryEvent::Start:
      return "start";
    case JumpgateHistoryEvent::Progress:
      return "progress";
    case JumpgateHistoryEvent::Pause:
      return "pause";
    case JumpgateHistoryEvent::Background:
      return "background";
    case JumpgateHistoryEvent::Resume:
      return "resume";
    case JumpgateHistoryEvent::Stop:
      return "stop";
    case JumpgateHistoryEvent::Completion:
      return "completion";
  }
  return "";
}

bool IsOpaqueAscii(std::string_view value, std::size_t minimum, std::size_t maximum)
{
  return value.size() >= minimum && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-';
                     });
}

bool IsCanonicalUuid(std::string_view value)
{
  static const std::regex pattern(
      "^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  return std::regex_match(value.begin(), value.end(), pattern);
}

bool IsHistoryGrant(std::string_view value)
{
  return value.size() >= 26 && value.size() <= 132 && value.substr(0, 4) == "hg1_" &&
         IsOpaqueAscii(value.substr(4), 22, 128);
}

bool IsHistoryGrantKind(std::string_view value)
{
  return value == "canonical" || value == "local" || value == "negative";
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
        if (json[index] == '\\' && ++index == json.size())
          return false;
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

bool ReadPositiveSafeInteger(const CVariant& value, std::uint64_t& result)
{
  if (value.isUnsignedInteger())
    result = value.asUnsignedInteger();
  else if (value.isSignedInteger() && value.asInteger() > 0)
    result = static_cast<std::uint64_t>(value.asInteger());
  else
    return false;
  return result > 0 && result <= MAX_SAFE_INTEGER;
}

bool IsValidPreferences(const CVariant& value)
{
  if (!value.isObject() ||
      !HasOnlyMembers(value, {"subtitleTrackId", "audioTrackId", "videoTrackId",
                              "subtitleLanguages", "audioLanguages", "subtitlesEnabled",
                              "hearingImpaired", "forced", "playbackSpeed"}))
  {
    return false;
  }
  for (const char* field : {"subtitleTrackId", "audioTrackId", "videoTrackId"})
  {
    if (value.isMember(field) && (!value[field].isString() || value[field].asString().size() > 256))
    {
      return false;
    }
  }
  for (const char* field : {"subtitleLanguages", "audioLanguages"})
  {
    if (!value.isMember(field))
      continue;
    if (!value[field].isArray() || value[field].size() > 32)
      return false;
    for (auto item = value[field].begin_array(); item != value[field].end_array(); ++item)
    {
      if (!item->isString() || item->asString().empty() || item->asString().size() > 32 ||
          !std::all_of(item->asString().begin(), item->asString().end(),
                       [](char character)
                       {
                         return std::isalnum(static_cast<unsigned char>(character)) ||
                                character == '_' || character == '-';
                       }))
      {
        return false;
      }
    }
  }
  for (const char* field : {"subtitlesEnabled", "hearingImpaired", "forced"})
  {
    if (value.isMember(field) && !value[field].isBoolean())
      return false;
  }
  if (value.isMember("playbackSpeed"))
  {
    if (!value["playbackSpeed"].isDouble() && !value["playbackSpeed"].isInteger())
      return false;
    const double speed = value["playbackSpeed"].asDouble();
    if (!std::isfinite(speed) || speed < 0.25 || speed > 4.0)
      return false;
  }
  return true;
}

bool IsValidRequest(const JumpgateHistoryEventRequest& request)
{
  if (!IsCanonicalOrigin(request.bridgeOrigin) || !IsOpaqueAscii(request.deviceToken, 32, 128) ||
      !IsHistoryGrant(request.historyGrant) || !IsHistoryGrantKind(request.historyGrantKind) ||
      !IsCanonicalUuid(request.idempotencyKey) || !IsOpaqueAscii(request.sessionId, 8, 128) ||
      request.sessionRevision == 0 || request.sessionRevision > MAX_SAFE_INTEGER ||
      request.snapshot.positionMs < 0 || request.snapshot.durationMs < 0 ||
      request.snapshot.watchedMs < 0)
  {
    return false;
  }
  if ((request.snapshot.durationMs == 0 &&
       (request.snapshot.positionMs != 0 || request.snapshot.watchedMs != 0)) ||
      (request.snapshot.durationMs > 0 &&
       (request.snapshot.positionMs > request.snapshot.durationMs ||
        request.snapshot.watchedMs > request.snapshot.durationMs)))
  {
    return false;
  }
  return !request.snapshot.playbackPreferences ||
         IsValidPreferences(*request.snapshot.playbackPreferences);
}

bool SerializeRequest(JumpgateHistoryEvent event,
                      const JumpgateHistoryEventRequest& request,
                      std::string& body)
{
  CVariant root{CVariant::VariantTypeObject};
  root["event"] = ToString(event);
  root["sessionRevision"] = request.sessionRevision;
  root["positionMs"] = request.snapshot.positionMs;
  root["durationMs"] = request.snapshot.durationMs;
  root["watchedMs"] = request.snapshot.watchedMs;
  if (request.snapshot.playbackPreferences)
    root["playbackPreferences"] = *request.snapshot.playbackPreferences;
  return CJSONVariantWriter::Write(root, body, true) && body.size() <= MAX_REQUEST_BODY_BYTES;
}

bool IsValidSessionState(std::string_view value)
{
  return value == "playing" || value == "paused" || value == "backgrounded" || value == "released";
}

bool ParseAcceptedResponse(const std::string& body,
                           JumpgateHistoryEvent event,
                           const JumpgateHistoryEventRequest& request,
                           JumpgateHistoryEventResult& result)
{
  if (body.empty() || body.size() > MAX_RESPONSE_BODY_BYTES || !HasUniqueTopLevelMembers(body))
    return false;

  CVariant root;
  std::uint64_t sessionRevision = 0;
  if (!CJSONVariantParser::Parse(body, root) || root.size() != 9 ||
      !HasOnlyMembers(root, {"ok", "status", "grantKind", "event", "sessionId", "sessionState",
                             "sessionRevision", "history", "dispatchIntent"}) ||
      !root.isMember("ok") || !root["ok"].isBoolean() || !root["ok"].asBoolean() ||
      !root.isMember("status") || !root["status"].isString() || !root.isMember("grantKind") ||
      !root["grantKind"].isString() || root["grantKind"].asString() != request.historyGrantKind ||
      !root.isMember("event") || !root["event"].isString() ||
      root["event"].asString() != ToString(event) || !root.isMember("sessionId") ||
      !root["sessionId"].isString() || root["sessionId"].asString() != request.sessionId ||
      !root.isMember("sessionState") || !root["sessionState"].isString() ||
      !IsValidSessionState(root["sessionState"].asString()) || !root.isMember("sessionRevision") ||
      !ReadPositiveSafeInteger(root["sessionRevision"], sessionRevision) ||
      !root.isMember("history") || (!root["history"].isNull() && !root["history"].isObject()) ||
      !root.isMember("dispatchIntent") ||
      (!root["dispatchIntent"].isNull() && !root["dispatchIntent"].isObject()))
  {
    return false;
  }

  const std::string status = root["status"].asString();
  if (status == "applied")
    result.status = JumpgateHistoryEventStatus::Applied;
  else if (status == "local_only")
    result.status = JumpgateHistoryEventStatus::LocalOnly;
  else if (status == "suppressed")
    result.status = JumpgateHistoryEventStatus::Suppressed;
  else
    return false;

  const std::string state = root["sessionState"].asString();
  if ((event == JumpgateHistoryEvent::Stop || event == JumpgateHistoryEvent::Completion) &&
      state != "released")
  {
    return false;
  }
  if (event == JumpgateHistoryEvent::Pause && state != "paused")
    return false;
  if (event == JumpgateHistoryEvent::Background && state != "backgrounded")
    return false;
  if (result.status == JumpgateHistoryEventStatus::Suppressed &&
      event != JumpgateHistoryEvent::Start && event != JumpgateHistoryEvent::Progress)
  {
    return false;
  }

  result.sessionState = state;
  result.sessionRevision = sessionRevision;
  return true;
}

std::optional<std::string> ParseErrorCode(const std::string& body)
{
  if (body.empty() || body.size() > MAX_RESPONSE_BODY_BYTES || !HasUniqueTopLevelMembers(body))
    return std::nullopt;
  CVariant root;
  if (!CJSONVariantParser::Parse(body, root) || root.size() != 2 ||
      !HasOnlyMembers(root, {"ok", "error"}) || !root.isMember("ok") || !root["ok"].isBoolean() ||
      root["ok"].asBoolean() || !root.isMember("error") || !root["error"].isString())
  {
    return std::nullopt;
  }
  return root["error"].asString();
}

JumpgateHistoryEventStatus MapFailure(int statusCode, const std::string& body)
{
  const std::optional<std::string> error = ParseErrorCode(body);
  if (statusCode == 401 || statusCode == 403)
    return JumpgateHistoryEventStatus::AuthenticationFailure;
  if (statusCode == 400)
    return JumpgateHistoryEventStatus::InvalidRequest;
  if (statusCode == 409 && error == "history_event_idempotency_conflict")
    return JumpgateHistoryEventStatus::IdempotencyConflict;
  if (statusCode == 409)
    return JumpgateHistoryEventStatus::StaleGrant;
  if (statusCode == 408 || statusCode == 425 || statusCode == 429 || statusCode >= 500)
    return JumpgateHistoryEventStatus::Unavailable;
  return JumpgateHistoryEventStatus::HttpFailure;
}
} // namespace

CJumpgateHistoryEventClient::CJumpgateHistoryEventClient(IJumpgatePlaybackClaimTransport& transport)
  : m_transport(transport)
{
}

JumpgateHistoryEventResult CJumpgateHistoryEventClient::Send(
    JumpgateHistoryEvent event, const JumpgateHistoryEventRequest& request) const
{
  JumpgateHistoryEventResult result;
  JumpgatePlaybackHttpRequest prepared;
  if (!Prepare(event, request, prepared))
  {
    result.status = JumpgateHistoryEventStatus::InvalidRequest;
    return result;
  }

  return SendPrepared(event, request, prepared);
}

bool CJumpgateHistoryEventClient::Prepare(JumpgateHistoryEvent event,
                                          const JumpgateHistoryEventRequest& request,
                                          JumpgatePlaybackHttpRequest& prepared) const
{
  prepared.ClearSensitive();
  if (!IsValidRequest(request))
    return false;

  std::string body;
  if (!SerializeRequest(event, request, body))
  {
    std::fill(body.begin(), body.end(), '\0');
    return false;
  }

  prepared.url = request.bridgeOrigin + "/v1/history/events";
  prepared.contentType = "application/json";
  prepared.authorization = "Bearer " + request.deviceToken;
  prepared.headers = {{"x-jumpgate-history-grant", request.historyGrant},
                      {"Idempotency-Key", request.idempotencyKey}};
  prepared.body = std::move(body);
  prepared.followRedirects = false;
  return true;
}

JumpgateHistoryEventResult CJumpgateHistoryEventClient::SendPrepared(
    JumpgateHistoryEvent event,
    const JumpgateHistoryEventRequest& request,
    const JumpgatePlaybackHttpRequest& prepared) const
{
  JumpgateHistoryEventResult result;
  if (!IsValidRequest(request) || prepared.url != request.bridgeOrigin + "/v1/history/events" ||
      prepared.contentType != "application/json" || prepared.followRedirects ||
      prepared.authorization != "Bearer " + request.deviceToken || prepared.body.empty() ||
      prepared.body.size() > MAX_REQUEST_BODY_BYTES || prepared.headers.size() != 2 ||
      prepared.headers[0].name != "x-jumpgate-history-grant" ||
      prepared.headers[0].value != request.historyGrant ||
      prepared.headers[1].name != "Idempotency-Key" ||
      prepared.headers[1].value != request.idempotencyKey)
  {
    result.status = JumpgateHistoryEventStatus::InvalidRequest;
    return result;
  }

  JumpgatePlaybackHttpResponse response;
  if (!m_transport.Post(prepared, response))
  {
    response.ClearSensitive();
    result.status = JumpgateHistoryEventStatus::TransportFailure;
    return result;
  }

  result.httpStatus = response.statusCode;
  if (response.statusCode < 100 || response.statusCode > 599)
  {
    response.ClearSensitive();
    result.status = JumpgateHistoryEventStatus::InvalidResponse;
    return result;
  }
  if (response.statusCode != 200)
  {
    result.status = MapFailure(response.statusCode, response.body);
    response.ClearSensitive();
    return result;
  }

  JumpgateHistoryEventResult parsed;
  parsed.httpStatus = response.statusCode;
  if (!ParseAcceptedResponse(response.body, event, request, parsed))
  {
    response.ClearSensitive();
    result.status = JumpgateHistoryEventStatus::InvalidResponse;
    return result;
  }
  response.ClearSensitive();
  return parsed;
}

} // namespace KODI::JUMPGATE
