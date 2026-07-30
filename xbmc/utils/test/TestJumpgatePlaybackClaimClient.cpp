/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgatePlaybackClaimClient.h"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
constexpr const char* ATTEMPT_ID = "018f47a2-5b6c-7d8e-9f01-23456789abcd";
constexpr const char* RECEIPT_ID = "018f47a2-5b6c-7d8e-af01-23456789abcd";
const std::string DEVICE_TOKEN(43, 'A');
const std::string HISTORY_GRANT = "hg1_" + std::string(32, 'G');
const std::string INTENT_HASH(64, 'b');

class FakePlaybackTransport final : public IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const JumpgatePlaybackHttpRequest& request,
            JumpgatePlaybackHttpResponse& response) override
  {
    ++calls;
    url = request.url;
    contentType = request.contentType;
    authorization = request.authorization;
    body = request.body;
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
  std::string url;
  std::string contentType;
  std::string authorization;
  std::string body;
  bool followRedirects{true};
};

PlaybackClaimRequest ValidClaimRequest()
{
  PlaybackClaimRequest request;
  request.bridgeOrigin = ORIGIN;
  request.deviceToken = DEVICE_TOKEN;
  request.attemptId = ATTEMPT_ID;
  request.fingerprints = {"v1:url:sha256:" + std::string(64, '1'),
                          "v1:opaque:sha256:" + std::string(64, '2')};
  request.intentUrlHash = INTENT_HASH;
  request.launchedAt = 1783900800123LL;
  request.client = PlaybackClaimClientInfo{"android", "0.2.12"};
  return request;
}

PlaybackReleaseRequest ValidReleaseRequest()
{
  return {ORIGIN, DEVICE_TOKEN, "session_00000001", RECEIPT_ID};
}

std::string ClaimedResponse()
{
  return std::string{R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{"contentKey":")"} +
         std::string(64, 'c') +
         R"(","traktEligible":true},"expiresAt":"2026-07-13T10:05:00.123Z","historyGrant":")" +
         HISTORY_GRANT +
         R"(","historyGrantKind":"canonical","sessionId":"session_00000001","sessionRevision":1,"status":"claimed"})";
}

std::string NegativeResponse(const std::string& status)
{
  return "{\"historyGrant\":\"" + HISTORY_GRANT +
         "\",\"historyGrantKind\":\"negative\",\"sessionId\":\"session_negative_01\"," +
         "\"sessionRevision\":1,\"status\":\"" + status + "\"}";
}
} // namespace

TEST(TestJumpgatePlaybackClaimClient, SerializesAttemptBoundClaimAndParsesHistoryAuthority)
{
  FakePlaybackTransport transport;
  transport.responseBody = ClaimedResponse();
  CJumpgatePlaybackClaimClient client{transport};

  const PlaybackClaimResult result = client.Claim(ValidClaimRequest());

  ASSERT_TRUE(result.IsClaimed());
  EXPECT_EQ(result.claim.sessionId, "session_00000001");
  EXPECT_EQ(result.claim.sessionRevision, 1u);
  EXPECT_EQ(result.claim.historyGrant, HISTORY_GRANT);
  EXPECT_EQ(result.claim.historyGrantKind, "canonical");
  EXPECT_EQ(transport.url, "https://bridge.example/v1/playback/claim");
  EXPECT_EQ(transport.authorization, "Bearer " + DEVICE_TOKEN);
  EXPECT_FALSE(transport.followRedirects);

  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.body, body));
  ASSERT_TRUE(body.isObject());
  EXPECT_EQ(body.size(), 5u);
  EXPECT_EQ(body["attemptId"].asString(), ATTEMPT_ID);
  EXPECT_EQ(body["intentUrlHash"].asString(), INTENT_HASH);
  EXPECT_EQ(body["launchedAt"].asInteger(), 1783900800123LL);
  ASSERT_EQ(body["fingerprints"].size(), 2u);
  EXPECT_EQ(body["client"]["platform"].asString(), "android");
  EXPECT_FALSE(body.isMember("ip"));
  EXPECT_FALSE(body.isMember("title"));
}

TEST(TestJumpgatePlaybackClaimClient, ParsesEveryNegativeClaimWithAReleasableGrant)
{
  const std::vector<std::pair<std::string, PlaybackClaimStatus>> cases = {
      {"ambiguous", PlaybackClaimStatus::Ambiguous},
      {"expired", PlaybackClaimStatus::Expired},
      {"not_found", PlaybackClaimStatus::NotFound},
  };
  for (const auto& [name, expected] : cases)
  {
    FakePlaybackTransport transport;
    transport.responseBody = NegativeResponse(name);
    CJumpgatePlaybackClaimClient client{transport};
    const PlaybackClaimResult result = client.Claim(ValidClaimRequest());
    EXPECT_EQ(result.status, expected);
    EXPECT_EQ(result.claim.sessionId, "session_negative_01");
    EXPECT_EQ(result.claim.historyGrant, HISTORY_GRANT);
    EXPECT_EQ(result.claim.historyGrantKind, "negative");
    EXPECT_EQ(result.claim.sessionRevision, 1u);
  }
}

TEST(TestJumpgatePlaybackClaimClient, RejectsMalformedAttemptAndAuthorityWithoutTransportOrTrust)
{
  PlaybackClaimRequest invalid = ValidClaimRequest();
  invalid.attemptId.clear();
  FakePlaybackTransport transport;
  CJumpgatePlaybackClaimClient client{transport};
  EXPECT_EQ(client.Claim(invalid).status, PlaybackClaimStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);

  invalid = ValidClaimRequest();
  invalid.attemptId = "018F47A2-5B6C-7D8E-9F01-23456789ABCD";
  EXPECT_EQ(client.Claim(invalid).status, PlaybackClaimStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);

  transport.responseBody = ClaimedResponse();
  const std::string marker = "\"historyGrantKind\":\"canonical\"";
  transport.responseBody.replace(transport.responseBody.find(marker), marker.size(),
                                 "\"historyGrantKind\":\"negative\"");
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::InvalidResponse);

  transport.responseBody = NegativeResponse("not_found");
  transport.responseBody += " ";
  transport.responseBody.insert(transport.responseBody.size() - 2, ",\"extra\":true");
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::InvalidResponse);
}

TEST(TestJumpgatePlaybackClaimClient, SerializesTerminalReceiptForRelease)
{
  FakePlaybackTransport transport;
  transport.responseBody = R"({"status":"released"})";
  CJumpgatePlaybackClaimClient client{transport};

  const PlaybackReleaseResult result = client.Release(ValidReleaseRequest());
  ASSERT_TRUE(result.IsReleased());
  EXPECT_EQ(transport.url, "https://bridge.example/v1/playback/release");
  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.body, body));
  EXPECT_EQ(body.size(), 2u);
  EXPECT_EQ(body["sessionId"].asString(), "session_00000001");
  EXPECT_EQ(body["terminalReceiptId"].asString(), RECEIPT_ID);

  PlaybackReleaseRequest invalid = ValidReleaseRequest();
  invalid.terminalReceiptId.clear();
  const int priorCalls = transport.calls;
  EXPECT_EQ(client.Release(invalid).status, PlaybackReleaseStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, priorCalls);
}

TEST(TestJumpgatePlaybackClaimClient, FailsClosedOnTransportAuthenticationAndNonExactResponses)
{
  FakePlaybackTransport transport;
  CJumpgatePlaybackClaimClient client{transport};
  transport.succeeds = false;
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::TransportFailure);

  transport.succeeds = true;
  transport.responseStatus = 401;
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::AuthenticationFailure);

  transport.responseStatus = 200;
  transport.responseBody = R"({"status":"not_found"})";
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::InvalidResponse);

  transport.responseBody = R"({"status":"released","extra":true})";
  EXPECT_EQ(client.Release(ValidReleaseRequest()).status, PlaybackReleaseStatus::InvalidResponse);
}
