/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgateHistoryEventClient.h"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
constexpr const char* IDEMPOTENCY_KEY = "018f47a2-"
                                        "5b6c-7d8e-"
                                        "9f01-23456789abcd";
const std::string DEVICE_TOKEN(43, 'A');
const std::string HISTORY_GRANT = "hg1_" + std::string(32, 'H');

class FakeHistoryTransport final : public IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const JumpgatePlaybackHttpRequest& request,
            JumpgatePlaybackHttpResponse& response) override
  {
    ++calls;
    urls.emplace_back(request.url);
    bodies.emplace_back(request.body);
    authorization = request.authorization;
    headers = request.headers;
    followRedirects = request.followRedirects;
    if (!succeeds)
      return false;
    response.statusCode = responseStatus;
    response.body = responseBody;
    return true;
  }

  bool succeeds{true};
  int responseStatus{200};
  std::string responseBody;
  int calls{0};
  std::vector<std::string> urls;
  std::vector<std::string> bodies;
  std::string authorization;
  std::vector<JumpgatePlaybackHttpHeader> headers;
  bool followRedirects{true};
};

JumpgateHistoryEventRequest ValidRequest()
{
  JumpgateHistoryEventRequest request;
  request.bridgeOrigin = ORIGIN;
  request.deviceToken = DEVICE_TOKEN;
  request.historyGrant = HISTORY_GRANT;
  request.historyGrantKind = "canonical";
  request.idempotencyKey = IDEMPOTENCY_KEY;
  request.sessionId = "session_00000001";
  request.sessionRevision = 1;
  request.snapshot.positionMs = 12500;
  request.snapshot.durationMs = 100000;
  request.snapshot.watchedMs = 11000;
  return request;
}

const char* EventName(JumpgateHistoryEvent event)
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

std::string Accepted(JumpgateHistoryEvent event,
                     std::uint64_t revision,
                     const std::string& status = "applied",
                     const std::string& grantKind = "canonical")
{
  std::string state = "playing";
  if (event == JumpgateHistoryEvent::Pause)
    state = "paused";
  else if (event == JumpgateHistoryEvent::Background)
    state = "backgrounded";
  else if (event == JumpgateHistoryEvent::Stop || event == JumpgateHistoryEvent::Completion)
    state = "released";
  return "{\"dispatchIntent\":null,\"event\":\"" + std::string(EventName(event)) +
         "\",\"grantKind\":\"" + grantKind +
         "\",\"history\":null,\"ok\":true,\"sessionId\":\"session_00000001\"," +
         "\"sessionRevision\":" + std::to_string(revision) + ",\"sessionState\":\"" + state +
         "\",\"status\":\"" + status + "\"}";
}
} // namespace

TEST(TestJumpgateHistoryEventClient, SerializesExactAuthenticatedHistoryIngress)
{
  FakeHistoryTransport transport;
  transport.responseBody = Accepted(JumpgateHistoryEvent::Start, 2);
  CJumpgateHistoryEventClient client{transport};

  const JumpgateHistoryEventResult result =
      client.Send(JumpgateHistoryEvent::Start, ValidRequest());

  ASSERT_TRUE(result.IsAccepted());
  EXPECT_EQ(result.status, JumpgateHistoryEventStatus::Applied);
  EXPECT_EQ(result.sessionRevision, 2u);
  ASSERT_EQ(transport.urls.size(), 1u);
  EXPECT_EQ(transport.urls[0], "https://bridge.example/v1/history/events");
  EXPECT_EQ(transport.authorization, "Bearer " + DEVICE_TOKEN);
  EXPECT_FALSE(transport.followRedirects);
  ASSERT_EQ(transport.headers.size(), 2u);
  EXPECT_EQ(transport.headers[0].name, "x-jumpgate-history-grant");
  EXPECT_EQ(transport.headers[0].value, HISTORY_GRANT);
  EXPECT_EQ(transport.headers[1].name, "Idempotency-Key");
  EXPECT_EQ(transport.headers[1].value, IDEMPOTENCY_KEY);

  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.bodies[0], body));
  EXPECT_EQ(body.size(), 5u);
  EXPECT_EQ(body["event"].asString(), "start");
  EXPECT_EQ(body["sessionRevision"].asUnsignedInteger(), 1u);
  EXPECT_EQ(body["positionMs"].asInteger(), 12500);
  EXPECT_EQ(body["durationMs"].asInteger(), 100000);
  EXPECT_EQ(body["watchedMs"].asInteger(), 11000);
  EXPECT_FALSE(body.isMember("sessionId"));
  EXPECT_FALSE(body.isMember("profileId"));
  EXPECT_FALSE(body.isMember("sourceUrl"));
}

TEST(TestJumpgateHistoryEventClient, AcceptsAllSevenEventsAndExactGrantStatuses)
{
  const std::vector<JumpgateHistoryEvent> events = {
      JumpgateHistoryEvent::Start,      JumpgateHistoryEvent::Progress, JumpgateHistoryEvent::Pause,
      JumpgateHistoryEvent::Background, JumpgateHistoryEvent::Resume,   JumpgateHistoryEvent::Stop,
      JumpgateHistoryEvent::Completion,
  };
  for (const JumpgateHistoryEvent event : events)
  {
    FakeHistoryTransport transport;
    transport.responseBody = Accepted(event, 7);
    CJumpgateHistoryEventClient client{transport};
    const auto result = client.Send(event, ValidRequest());
    EXPECT_EQ(result.status, JumpgateHistoryEventStatus::Applied) << EventName(event);
    EXPECT_EQ(result.sessionRevision, 7u);
  }

  FakeHistoryTransport local;
  auto localRequest = ValidRequest();
  localRequest.historyGrantKind = "local";
  local.responseBody = Accepted(JumpgateHistoryEvent::Progress, 2, "local_only", "local");
  CJumpgateHistoryEventClient localClient{local};
  EXPECT_EQ(localClient.Send(JumpgateHistoryEvent::Progress, localRequest).status,
            JumpgateHistoryEventStatus::LocalOnly);

  FakeHistoryTransport suppressed;
  suppressed.responseBody = Accepted(JumpgateHistoryEvent::Progress, 1, "suppressed");
  CJumpgateHistoryEventClient suppressedClient{suppressed};
  EXPECT_EQ(suppressedClient.Send(JumpgateHistoryEvent::Progress, ValidRequest()).status,
            JumpgateHistoryEventStatus::Suppressed);
}

TEST(TestJumpgateHistoryEventClient, PreparedRetriesReuseExactBytesAndHeaders)
{
  FakeHistoryTransport transport;
  transport.responseBody = Accepted(JumpgateHistoryEvent::Progress, 2);
  CJumpgateHistoryEventClient client{transport};
  const auto request = ValidRequest();
  JumpgatePlaybackHttpRequest prepared;
  ASSERT_TRUE(client.Prepare(JumpgateHistoryEvent::Progress, request, prepared));

  ASSERT_TRUE(client.SendPrepared(JumpgateHistoryEvent::Progress, request, prepared).IsAccepted());
  ASSERT_TRUE(client.SendPrepared(JumpgateHistoryEvent::Progress, request, prepared).IsAccepted());
  ASSERT_EQ(transport.bodies.size(), 2u);
  EXPECT_EQ(transport.bodies[0], transport.bodies[1]);
  EXPECT_EQ(transport.headers[1].value, IDEMPOTENCY_KEY);
}

TEST(TestJumpgateHistoryEventClient, RejectsInvalidSnapshotsGrantsAndPreferencesBeforeTransport)
{
  std::vector<JumpgateHistoryEventRequest> invalid;
  invalid.emplace_back(ValidRequest()).historyGrant = "short";
  invalid.emplace_back(ValidRequest()).historyGrantKind = "negative-ish";
  invalid.emplace_back(ValidRequest()).idempotencyKey = "not-a-uuid";
  invalid.emplace_back(ValidRequest()).sessionRevision = 0;
  invalid.emplace_back(ValidRequest()).snapshot.positionMs = 100001;
  invalid.emplace_back(ValidRequest()).snapshot.watchedMs = -1;
  auto preferences = ValidRequest();
  preferences.snapshot.playbackPreferences = CVariant{CVariant::VariantTypeObject};
  (*preferences.snapshot.playbackPreferences)["sourceUrl"] = "https://secret.example/video";
  invalid.emplace_back(std::move(preferences));

  for (const auto& request : invalid)
  {
    FakeHistoryTransport transport;
    CJumpgateHistoryEventClient client{transport};
    EXPECT_EQ(client.Send(JumpgateHistoryEvent::Progress, request).status,
              JumpgateHistoryEventStatus::InvalidRequest);
    EXPECT_EQ(transport.calls, 0);
  }
}

TEST(TestJumpgateHistoryEventClient, MapsBridgeFailuresAndRejectsNonExactSuccess)
{
  const struct
  {
    int httpStatus;
    const char* code;
    JumpgateHistoryEventStatus expected;
  } cases[] = {
      {400, "invalid_history_event", JumpgateHistoryEventStatus::InvalidRequest},
      {409, "history_session_stale", JumpgateHistoryEventStatus::StaleGrant},
      {409, "history_event_idempotency_conflict", JumpgateHistoryEventStatus::IdempotencyConflict},
      {503, "history_unavailable", JumpgateHistoryEventStatus::Unavailable},
  };
  for (const auto& item : cases)
  {
    FakeHistoryTransport transport;
    transport.responseStatus = item.httpStatus;
    transport.responseBody = std::string{"{\"error\":\""} + item.code + "\",\"ok\":false}";
    CJumpgateHistoryEventClient client{transport};
    EXPECT_EQ(client.Send(JumpgateHistoryEvent::Start, ValidRequest()).status, item.expected);
  }

  FakeHistoryTransport malformed;
  malformed.responseBody = Accepted(JumpgateHistoryEvent::Start, 2);
  malformed.responseBody.insert(malformed.responseBody.size() - 1, ",\"extra\":true");
  CJumpgateHistoryEventClient client{malformed};
  EXPECT_EQ(client.Send(JumpgateHistoryEvent::Start, ValidRequest()).status,
            JumpgateHistoryEventStatus::InvalidResponse);

  malformed.responseBody = Accepted(JumpgateHistoryEvent::Pause, 2);
  EXPECT_EQ(client.Send(JumpgateHistoryEvent::Start, ValidRequest()).status,
            JumpgateHistoryEventStatus::InvalidResponse);
}
