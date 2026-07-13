/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgatePlaybackClaimClient.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
const std::string DEVICE_TOKEN(43, 'A');
const std::string INTENT_HASH(64, 'b');

class FakePlaybackTransport : public IJumpgatePlaybackClaimTransport
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
  request.fingerprints = {"v1:url:sha256:" + std::string(64, '1'),
                          "v1:opaque:sha256:" + std::string(64, '2')};
  request.intentUrlHash = INTENT_HASH;
  request.launchedAt = 1783900800123LL;
  request.client = PlaybackClaimClientInfo{"android", "0.2.12"};
  return request;
}

PlaybackReleaseRequest ValidReleaseRequest()
{
  return {ORIGIN, DEVICE_TOKEN, "session_00000001"};
}

std::string ClaimedResponse()
{
  return R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{"canonicalIdentity":{"id":"tt0133093","type":"movie"},"traktEligible":true},"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"session_00000001","status":"claimed"})";
}

} // namespace

TEST(TestJumpgatePlaybackClaimClient, SerializesAuthenticatedClaimAtExactOrigin)
{
  FakePlaybackTransport transport;
  transport.responseBody = ClaimedResponse();
  CJumpgatePlaybackClaimClient client{transport};

  const PlaybackClaimResult result = client.Claim(ValidClaimRequest());

  ASSERT_TRUE(result.IsClaimed());
  EXPECT_EQ(result.httpStatus, 200);
  EXPECT_EQ(result.claim.sessionId, "session_00000001");
  EXPECT_EQ(result.claim.claimedAt, "2026-07-13T10:00:00.123Z");
  EXPECT_EQ(result.claim.expiresAt, "2026-07-13T10:05:00.123Z");
  ASSERT_TRUE(result.claim.context.isObject());
  EXPECT_TRUE(result.claim.context["traktEligible"].asBoolean());

  EXPECT_EQ(transport.calls, 1);
  EXPECT_EQ(transport.url, "https://bridge.example/v1/playback/claim");
  EXPECT_EQ(transport.contentType, "application/json");
  EXPECT_EQ(transport.authorization, "Bearer " + DEVICE_TOKEN);
  EXPECT_FALSE(transport.followRedirects);

  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.body, body));
  ASSERT_TRUE(body.isObject());
  EXPECT_EQ(body.size(), 4u);
  ASSERT_TRUE(body["fingerprints"].isArray());
  ASSERT_EQ(body["fingerprints"].size(), 2u);
  EXPECT_EQ(body["fingerprints"][0].asString(), "v1:url:sha256:" + std::string(64, '1'));
  EXPECT_EQ(body["fingerprints"][1].asString(), "v1:opaque:sha256:" + std::string(64, '2'));
  EXPECT_EQ(body["intentUrlHash"].asString(), INTENT_HASH);
  EXPECT_TRUE(body["launchedAt"].isInteger());
  EXPECT_EQ(body["launchedAt"].asInteger(), 1783900800123LL);
  ASSERT_TRUE(body["client"].isObject());
  EXPECT_EQ(body["client"].size(), 2u);
  EXPECT_EQ(body["client"]["platform"].asString(), "android");
  EXPECT_EQ(body["client"]["version"].asString(), "0.2.12");
  EXPECT_FALSE(body.isMember("ip"));
  EXPECT_FALSE(body.isMember("title"));
  EXPECT_FALSE(body.isMember("recent"));
}

TEST(TestJumpgatePlaybackClaimClient, OmitsOptionalClientAndPreservesExactFingerprints)
{
  FakePlaybackTransport transport;
  transport.responseBody = R"({"status":"not_found"})";
  CJumpgatePlaybackClaimClient client{transport};
  PlaybackClaimRequest request = ValidClaimRequest();
  request.client.reset();
  request.fingerprints = {"exact-A", "exact-A", "exact-B"};

  const PlaybackClaimResult result = client.Claim(request);

  EXPECT_EQ(result.status, PlaybackClaimStatus::NotFound);
  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.body, body));
  EXPECT_FALSE(body.isMember("client"));
  ASSERT_EQ(body["fingerprints"].size(), 3u);
  EXPECT_EQ(body["fingerprints"][0].asString(), "exact-A");
  EXPECT_EQ(body["fingerprints"][1].asString(), "exact-A");
  EXPECT_EQ(body["fingerprints"][2].asString(), "exact-B");
}

TEST(TestJumpgatePlaybackClaimClient, RejectsNonCanonicalOrConfiguredOriginsWithoutTransport)
{
  const std::vector<std::string> invalidOrigins = {
      "https://bridge.example/",     "https://bridge.example/_c/config",
      "HTTPS://BRIDGE.EXAMPLE",      "https://bridge.example?config=x",
      "https://user@bridge.example", "http://bridge.example",
      "https://bridge.example/path", " https://bridge.example",
  };

  for (const std::string& origin : invalidOrigins)
  {
    FakePlaybackTransport transport;
    CJumpgatePlaybackClaimClient client{transport};
    PlaybackClaimRequest request = ValidClaimRequest();
    request.bridgeOrigin = origin;
    EXPECT_EQ(client.Claim(request).status, PlaybackClaimStatus::InvalidRequest) << origin;
    EXPECT_EQ(transport.calls, 0) << origin;
  }
}

TEST(TestJumpgatePlaybackClaimClient, AllowsExplicitCanonicalLoopbackDevelopmentOrigin)
{
  FakePlaybackTransport transport;
  transport.responseBody = R"({"status":"not_found"})";
  CJumpgatePlaybackClaimClient client{transport};
  PlaybackClaimRequest request = ValidClaimRequest();
  request.bridgeOrigin = "http://127.0.0.1:11470";

  EXPECT_EQ(client.Claim(request).status, PlaybackClaimStatus::NotFound);
  EXPECT_EQ(transport.url, "http://127.0.0.1:11470/v1/playback/claim");
  EXPECT_FALSE(transport.followRedirects);
}

TEST(TestJumpgatePlaybackClaimClient, RejectsMalformedClaimInputsWithoutTransport)
{
  std::vector<PlaybackClaimRequest> requests;
  PlaybackClaimRequest request = ValidClaimRequest();
  request.deviceToken = "short";
  requests.push_back(request);
  request = ValidClaimRequest();
  request.fingerprints.clear();
  requests.push_back(request);
  request = ValidClaimRequest();
  request.fingerprints = {""};
  requests.push_back(request);
  request = ValidClaimRequest();
  request.fingerprints = {"bad\nfingerprint"};
  requests.push_back(request);
  request = ValidClaimRequest();
  request.fingerprints.assign(33, "exact-fingerprint");
  requests.push_back(request);
  request = ValidClaimRequest();
  request.fingerprints.assign(32, std::string(512, 'x'));
  requests.push_back(request);
  request = ValidClaimRequest();
  request.intentUrlHash = std::string(64, 'A');
  requests.push_back(request);
  request = ValidClaimRequest();
  request.launchedAt = 0;
  requests.push_back(request);
  request = ValidClaimRequest();
  request.launchedAt = 8640000000000001LL;
  requests.push_back(request);
  request = ValidClaimRequest();
  request.client->platform.clear();
  requests.push_back(request);
  request = ValidClaimRequest();
  request.client->version = "version with spaces";
  requests.push_back(request);

  for (const PlaybackClaimRequest& invalid : requests)
  {
    FakePlaybackTransport transport;
    CJumpgatePlaybackClaimClient client{transport};
    EXPECT_EQ(client.Claim(invalid).status, PlaybackClaimStatus::InvalidRequest);
    EXPECT_EQ(transport.calls, 0);
  }
}

TEST(TestJumpgatePlaybackClaimClient, MapsNegativeClaimStatusesFailClosed)
{
  const std::vector<std::pair<std::string, PlaybackClaimStatus>> cases = {
      {"ambiguous", PlaybackClaimStatus::Ambiguous},
      {"expired", PlaybackClaimStatus::Expired},
      {"not_found", PlaybackClaimStatus::NotFound},
  };

  for (const auto& item : cases)
  {
    FakePlaybackTransport transport;
    transport.responseBody = "{\"status\":\"" + item.first + "\"}";
    CJumpgatePlaybackClaimClient client{transport};
    const PlaybackClaimResult result = client.Claim(ValidClaimRequest());
    EXPECT_EQ(result.status, item.second);
    EXPECT_FALSE(result.IsClaimed());
    EXPECT_TRUE(result.claim.sessionId.empty());
    EXPECT_TRUE(result.claim.context.isNull());
  }
}

TEST(TestJumpgatePlaybackClaimClient, RejectsMalformedOrNonExactClaimResponses)
{
  const std::vector<std::string> invalidResponses = {
      "",
      "not-json",
      R"([])",
      R"({"status":"unknown"})",
      R"({"status":"ambiguous","context":{}})",
      R"({"status":"not_found","status":"ambiguous"})",
      R"({"status":"claimed"})",
      R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{},"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":[],"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":1783936800123,"context":{"ok":true},"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":"2026-02-30T10:00:00.123Z","context":{"ok":true},"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":"2026-07-13T10:05:00.123Z","context":{"ok":true},"expiresAt":"2026-07-13T10:00:00.123Z","sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{"ok":true},"expiresAt":"2026-07-13T10:05:00.123Z","extra":true,"sessionId":"session_00000001","status":"claimed"})",
      R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{"ok":true},"expiresAt":"2026-07-13T10:05:00.123Z","sessionId":"bad id","status":"claimed"})",
  };

  for (const std::string& response : invalidResponses)
  {
    FakePlaybackTransport transport;
    transport.responseBody = response;
    CJumpgatePlaybackClaimClient client{transport};
    const PlaybackClaimResult result = client.Claim(ValidClaimRequest());
    EXPECT_EQ(result.status, PlaybackClaimStatus::InvalidResponse) << response;
    EXPECT_FALSE(result.IsClaimed());
  }
}

TEST(TestJumpgatePlaybackClaimClient, MapsTransportAuthenticationAndHttpFailures)
{
  FakePlaybackTransport transport;
  transport.succeeds = false;
  CJumpgatePlaybackClaimClient client{transport};
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::TransportFailure);

  transport.succeeds = true;
  transport.responseStatus = 401;
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::AuthenticationFailure);
  transport.responseStatus = 302;
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::HttpFailure);
  EXPECT_FALSE(transport.followRedirects);
  transport.responseStatus = 700;
  EXPECT_EQ(client.Claim(ValidClaimRequest()).status, PlaybackClaimStatus::InvalidResponse);
}

TEST(TestJumpgatePlaybackClaimClient, SerializesReleaseAndParsesReleasedOrNotFound)
{
  FakePlaybackTransport transport;
  transport.responseBody = R"({"status":"released"})";
  CJumpgatePlaybackClaimClient client{transport};

  PlaybackReleaseResult result = client.Release(ValidReleaseRequest());

  EXPECT_TRUE(result.IsReleased());
  EXPECT_EQ(result.httpStatus, 200);
  EXPECT_EQ(transport.url, "https://bridge.example/v1/playback/release");
  EXPECT_EQ(transport.contentType, "application/json");
  EXPECT_EQ(transport.authorization, "Bearer " + DEVICE_TOKEN);
  EXPECT_FALSE(transport.followRedirects);
  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.body, body));
  EXPECT_EQ(body.size(), 1u);
  EXPECT_EQ(body["sessionId"].asString(), "session_00000001");

  transport.responseBody = R"({"status":"not_found"})";
  result = client.Release(ValidReleaseRequest());
  EXPECT_EQ(result.status, PlaybackReleaseStatus::NotFound);
  EXPECT_FALSE(result.IsReleased());
}

TEST(TestJumpgatePlaybackClaimClient, RejectsMalformedReleaseInputsAndResponses)
{
  FakePlaybackTransport transport;
  CJumpgatePlaybackClaimClient client{transport};
  PlaybackReleaseRequest request = ValidReleaseRequest();
  request.sessionId = "short";
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);

  request = ValidReleaseRequest();
  request.bridgeOrigin += "/_c/config";
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);

  request = ValidReleaseRequest();
  transport.responseBody = R"({"status":"released","sessionId":"session_00000001"})";
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::InvalidResponse);
  transport.responseBody = R"({"status":"claimed"})";
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::InvalidResponse);
  transport.responseBody = R"({"status":"not_found","status":"released"})";
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::InvalidResponse);

  transport.succeeds = false;
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::TransportFailure);
  transport.succeeds = true;
  transport.responseStatus = 403;
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::AuthenticationFailure);
  transport.responseStatus = 500;
  EXPECT_EQ(client.Release(request).status, PlaybackReleaseStatus::HttpFailure);
}
