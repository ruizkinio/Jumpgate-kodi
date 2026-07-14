/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgateSubtitleClient.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <regex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
constexpr const char* SESSION = "session_00000001";
const std::string DEVICE_TOKEN(43, 'A');
const std::string SELECTOR_A(64, 'a');
const std::string SELECTOR_B(64, 'b');
const std::string ARTIFACT = "artifact_00000001";
const std::string BASE_NAME(64, 'c');
const std::string TEXT_SHA256 = "56db31420cc3d2ec7c1fb467c036051bf463c64fe95f07a5536317476a1afc72";

std::vector<std::uint8_t> Bytes(const std::string& value)
{
  return {value.begin(), value.end()};
}

struct Reply
{
  bool succeeds{true};
  int status{200};
  std::string effectiveUrl;
  std::string redirectUrl;
  std::string contentType{"application/json; charset=utf-8"};
  std::optional<std::uint64_t> contentLength;
  std::string retryAfter;
  std::string contentEncoding{"identity"};
  std::string acceptRanges{"none"};
  std::vector<std::uint8_t> body;
};

Reply JsonReply(std::string body)
{
  Reply reply;
  reply.body = Bytes(body);
  return reply;
}

class FakeSubtitleTransport final : public IJumpgateSubtitleTransport
{
public:
  bool Perform(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    ++calls;
    methods.push_back(request.method);
    urls.push_back(request.url);
    contentTypes.push_back(request.contentType);
    authorizations.push_back(request.authorization);
    bodies.push_back(request.body);
    responseCaps.push_back(request.maximumResponseBytes);
    summaries.push_back(request.RedactedSummary());
    followRedirects.push_back(request.followRedirects);
    cancellations.push_back(cancellation.IsCancelled());
    if (replies.empty())
      return false;
    Reply reply = std::move(replies.front());
    replies.pop_front();
    response.statusCode = reply.status;
    response.effectiveUrl =
        reply.effectiveUrl.empty() ? request.url : std::move(reply.effectiveUrl);
    response.redirectUrl = std::move(reply.redirectUrl);
    response.contentType = std::move(reply.contentType);
    response.contentLength = reply.contentLength;
    response.retryAfter = std::move(reply.retryAfter);
    response.contentEncoding = std::move(reply.contentEncoding);
    response.acceptRanges = std::move(reply.acceptRanges);
    response.body = std::move(reply.body);
    return reply.succeeds;
  }

  std::deque<Reply> replies;
  int calls{0};
  std::vector<JumpgateSubtitleHttpMethod> methods;
  std::vector<std::string> urls;
  std::vector<std::string> contentTypes;
  std::vector<std::string> authorizations;
  std::vector<std::string> bodies;
  std::vector<std::size_t> responseCaps;
  std::vector<std::string> summaries;
  std::vector<bool> followRedirects;
  std::vector<bool> cancellations;
};

std::string DiscoverResponse()
{
  return "{\"schemaVersion\":1,\"subtitles\":["
         "{\"selector\":\"" +
         SELECTOR_A +
         "\",\"language\":\"en\",\"format\":\"vtt\",\"label\":\"English - VTT\",\"rank\":1},"
         "{\"selector\":\"" +
         SELECTOR_B +
         "\",\"language\":\"es\",\"format\":\"srt\",\"label\":\"Spanish - SRT\",\"rank\":2}]}";
}

std::string TextResolveResponse(std::uint64_t length = 12)
{
  const std::string fileName = BASE_NAME + ".vtt";
  return "{\"schemaVersion\":2,\"status\":\"ready\",\"artifactId\":\"" + ARTIFACT +
         "\",\"expiresAt\":1783900900000,\"expiresAtUnit\":\"unix_ms\",\"parts\":[{"
         "\"partNumber\":1,\"role\":\"subtitle\",\"contentLength\":" +
         std::to_string(length) + ",\"contentType\":\"text/vtt\",\"fileName\":\"" + fileName +
         "\",\"path\":\"/v1/subtitles/" + SESSION + "/" + ARTIFACT + "/1/" + fileName +
         "\",\"sha256\":\"" + TEXT_SHA256 + "\"}]}";
}

std::string VobSubResolveResponse(bool sameBase = true)
{
  const std::string secondBase = sameBase ? BASE_NAME : std::string(64, 'd');
  const std::string indexName = BASE_NAME + ".idx";
  const std::string subName = secondBase + ".sub";
  return "{\"schemaVersion\":2,\"status\":\"ready\",\"artifactId\":\"" + ARTIFACT +
         "\",\"expiresAt\":1783900900000,\"expiresAtUnit\":\"unix_ms\",\"parts\":[{"
         "\"partNumber\":1,\"role\":\"index\",\"contentLength\":11,"
         "\"contentType\":\"application/x-vobsub\",\"fileName\":\"" +
         indexName + "\",\"path\":\"/v1/subtitles/" + SESSION + "/" + ARTIFACT + "/1/" + indexName +
         "\",\"sha256\":\"" + std::string(64, 'd') +
         "\"},{\"partNumber\":2,\"role\":\"sub\",\"contentLength\":13,"
         "\"contentType\":\"application/octet-stream\",\"fileName\":\"" +
         subName + "\",\"path\":\"/v1/subtitles/" + SESSION + "/" + ARTIFACT + "/2/" + subName +
         "\",\"sha256\":\"" + std::string(64, 'e') + "\"}]}";
}

JumpgateSubtitlePartDescriptor TextDescriptor(std::uint64_t length)
{
  const std::string fileName = BASE_NAME + ".vtt";
  return {1,          "subtitle",
          length,     "text/vtt",
          fileName,   "/v1/subtitles/" + std::string(SESSION) + "/" + ARTIFACT + "/1/" + fileName,
          TEXT_SHA256};
}
} // namespace

static_assert(!std::is_copy_constructible_v<CJumpgateSubtitleBearerAuthority>);
static_assert(!std::is_copy_assignable_v<CJumpgateSubtitleBearerAuthority>);
static_assert(std::is_move_constructible_v<CJumpgateSubtitleBearerAuthority>);
static_assert(!std::is_copy_constructible_v<JumpgateSubtitleHttpRequest>);

TEST(TestJumpgateSubtitleClient, ParsesExactDiscoverAndSelectsByConfiguredLanguageThenRank)
{
  FakeSubtitleTransport transport;
  transport.replies.push_back(JsonReply(DiscoverResponse()));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  const JumpgateSubtitleDiscoverResult result = client.Discover(ORIGIN, authority, SESSION);

  ASSERT_EQ(result.status, JumpgateSubtitleResultStatus::Success);
  ASSERT_EQ(result.candidates.size(), 2u);
  EXPECT_EQ(result.candidates[0].rank, 1u);
  EXPECT_EQ(result.candidates[1].language, "es");
  const auto selected = SelectJumpgateSubtitleCandidate(result.candidates, {"es", "en"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->selector, SELECTOR_B);

  ASSERT_EQ(transport.calls, 1);
  EXPECT_EQ(transport.methods[0], JumpgateSubtitleHttpMethod::Post);
  EXPECT_EQ(transport.urls[0], "https://bridge.example/v1/subtitles/discover");
  EXPECT_EQ(transport.contentTypes[0], "application/json");
  EXPECT_EQ(transport.authorizations[0], "Bearer " + DEVICE_TOKEN);
  EXPECT_FALSE(transport.followRedirects[0]);
  CVariant body;
  ASSERT_TRUE(CJSONVariantParser::Parse(transport.bodies[0], body));
  EXPECT_EQ(body.size(), 1u);
  EXPECT_EQ(body["sessionId"].asString(), SESSION);
}

TEST(TestJumpgateSubtitleClient, SelectionIsProviderIndependentAndUsesStableSelectorTieBreak)
{
  std::vector<JumpgateSubtitleCandidate> candidates = {
      {SELECTOR_B, "English US", "en-us", "srt", 2},
      {SELECTOR_A, "English GB", "en-gb", "srt", 2},
      {std::string(64, 'c'), "French", "fr", "srt", 1},
  };
  auto selected = SelectJumpgateSubtitleCandidate(candidates, {"en"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->selector, SELECTOR_A);

  std::reverse(candidates.begin(), candidates.end());
  selected = SelectJumpgateSubtitleCandidate(candidates, {"en"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->selector, SELECTOR_A);
  EXPECT_FALSE(SelectJumpgateSubtitleCandidate(candidates, {"EN"}));
}

TEST(TestJumpgateSubtitleClient, RejectsNonCanonicalOriginsAndCredentialsWithoutTransport)
{
  const std::vector<std::string> invalidOrigins = {
      "https://bridge.example/",     "https://bridge.example/path",
      "https://bridge.example?q=1",  "https://bridge.example#fragment",
      "https://user@bridge.example", "HTTPS://BRIDGE.EXAMPLE",
      "http://bridge.example",       " https://bridge.example",
  };
  for (const std::string& origin : invalidOrigins)
  {
    FakeSubtitleTransport transport;
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Discover(origin, authority, SESSION).status,
              JumpgateSubtitleResultStatus::InvalidRequest)
        << origin;
    EXPECT_EQ(transport.calls, 0) << origin;
  }

  FakeSubtitleTransport transport;
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority shortAuthority{"short"};
  EXPECT_EQ(client.Discover(ORIGIN, shortAuthority, SESSION).status,
            JumpgateSubtitleResultStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);
}

TEST(TestJumpgateSubtitleClient, RejectsUnknownDuplicateAndOutOfBoundsDiscoverFields)
{
  const std::vector<std::string> invalid = {
      R"({"schemaVersion":2,"subtitles":[]})",
      R"({"schemaVersion":1,"subtitles":[],"extra":true})",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_A +
          "\",\"language\":\"en\",\"format\":\"srt\",\"label\":\"English\",\"rank\":1,\"rank\":2}]"
          "}",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + std::string(64, 'A') +
          "\",\"language\":\"en\",\"format\":\"srt\",\"label\":\"English\",\"rank\":1}]}",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_A +
          "\",\"language\":\"EN\",\"format\":\"srt\",\"label\":\"English\",\"rank\":1}]}",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_A +
          "\",\"language\":\"en\",\"format\":\"SRT\",\"label\":\"English\",\"rank\":1}]}",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_A +
          "\",\"language\":\"en\",\"format\":\"srt\",\"label\":\"" + std::string(65, 'x') +
          "\",\"rank\":1}]}",
      "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_A +
          "\",\"language\":\"en\",\"format\":\"srt\",\"label\":\"English\",\"rank\":2}]}",
  };

  for (const std::string& body : invalid)
  {
    FakeSubtitleTransport transport;
    transport.replies.push_back(JsonReply(body));
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
              JumpgateSubtitleResultStatus::ProtocolFailure)
        << body;
  }
}

TEST(TestJumpgateSubtitleClient, RejectsEmbeddedNulAfterValidDiscoverAndResolvePrefixes)
{
  std::string discover = DiscoverResponse();
  discover.push_back('\0');
  discover += R"({"schemaVersion":1,"subtitles":[]})";
  std::string resolve = TextResolveResponse();
  resolve.push_back('\0');
  resolve += TextResolveResponse();

  FakeSubtitleTransport transport;
  transport.replies.push_back(JsonReply(std::move(discover)));
  transport.replies.push_back(JsonReply(std::move(resolve)));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
}

TEST(TestJumpgateSubtitleClient, ClassifiesAuthStaleBusyProtocolAndSoftFailuresStably)
{
  FakeSubtitleTransport transport;
  Reply failed;
  failed.succeeds = false;
  transport.replies.push_back(std::move(failed));
  for (int status : {401, 404, 409, 409, 400, 302, 403, 503})
  {
    Reply reply;
    reply.status = status;
    if (status == 409 && transport.replies.size() == 3)
      reply.retryAfter = "7";
    else if (status == 409)
      reply.retryAfter = "61";
    transport.replies.push_back(std::move(reply));
  }
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::SoftFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::RePairRequired);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::Stale);
  const auto busy = client.Discover(ORIGIN, authority, SESSION);
  EXPECT_EQ(busy.status, JumpgateSubtitleResultStatus::RetryableBusy);
  EXPECT_EQ(busy.retryAfterSeconds, 7u);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::HttpFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::SoftFailure);
}

TEST(TestJumpgateSubtitleClient, Classifies429AsStrictBoundedRetryable)
{
  FakeSubtitleTransport transport;
  Reply retryable;
  retryable.status = 429;
  retryable.retryAfter = "9";
  transport.replies.push_back(std::move(retryable));
  constexpr std::array<const char*, 8> malformedHeaders{"",   "0",  "01",  "61",
                                                        " 1", "1 ", "1.0", "seconds"};
  for (const char* malformed : malformedHeaders)
  {
    Reply reply;
    reply.status = 429;
    reply.retryAfter = malformed;
    transport.replies.push_back(std::move(reply));
  }
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  const auto retry = client.Discover(ORIGIN, authority, SESSION);
  EXPECT_EQ(retry.status, JumpgateSubtitleResultStatus::RetryableBusy);
  EXPECT_EQ(retry.httpStatus, 429);
  EXPECT_EQ(retry.retryAfterSeconds, 9u);
  for (int index = 0; index < 8; ++index)
  {
    EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
              JumpgateSubtitleResultStatus::ProtocolFailure);
  }
}

TEST(TestJumpgateSubtitleClient, RejectsFollowedAndAdvertisedRedirects)
{
  FakeSubtitleTransport transport;
  Reply followed = JsonReply(DiscoverResponse());
  followed.effectiveUrl = "https://attacker.example/v1/subtitles/discover";
  transport.replies.push_back(std::move(followed));
  Reply advertised = JsonReply(DiscoverResponse());
  advertised.redirectUrl = "https://attacker.example/private";
  transport.replies.push_back(std::move(advertised));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_FALSE(transport.followRedirects[0]);
  EXPECT_FALSE(transport.followRedirects[1]);
}

TEST(TestJumpgateSubtitleClient, ParsesExactTextResolutionAndCanonicalDeliveryPath)
{
  FakeSubtitleTransport transport;
  transport.replies.push_back(JsonReply(TextResolveResponse()));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  const JumpgateSubtitleResolveResult result =
      client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A);

  ASSERT_EQ(result.status, JumpgateSubtitleResultStatus::Success);
  EXPECT_EQ(result.artifact.artifactId, ARTIFACT);
  EXPECT_EQ(result.artifact.expiresAt, 1783900900000LL);
  ASSERT_EQ(result.artifact.parts.size(), 1u);
  EXPECT_EQ(result.artifact.parts[0].role, "subtitle");
  EXPECT_EQ(result.artifact.parts[0].contentType, "text/vtt");
  EXPECT_EQ(result.artifact.parts[0].sha256, TEXT_SHA256);
  EXPECT_EQ(transport.urls[0], "https://bridge.example/v1/subtitles/resolve");
  EXPECT_NE(transport.bodies[0].find("\"responseSchemaVersion\":2"), std::string::npos);
}

TEST(TestJumpgateSubtitleClient, RejectsMalformedResolveMetadataAndPaths)
{
  const std::string valid = TextResolveResponse();
  std::vector<std::string> invalid = {
      valid.substr(0, valid.size() - 1) + ",\"extra\":true}",
      std::regex_replace(valid, std::regex("unix_ms"), "seconds"),
      std::regex_replace(valid, std::regex("1783900900000"), "0"),
      std::regex_replace(valid, std::regex("text/vtt"), "text/plain"),
      std::regex_replace(valid, std::regex("schemaVersion\\\":2"), "schemaVersion\":1"),
      std::regex_replace(valid, std::regex(R"(contentLength":12)"), R"(contentLength":0)"),
      std::regex_replace(valid, std::regex(TEXT_SHA256), std::string(64, 'A')),
      std::regex_replace(valid, std::regex(R"(,"sha256":"[a-f0-9]{64}")"), ""),
      std::regex_replace(valid, std::regex("/1/"), "/2/"),
      std::regex_replace(valid, std::regex("\\.vtt"), ".idx"),
      std::regex_replace(valid, std::regex(R"("role":"subtitle")"),
                         R"("role":"subtitle","role":"index")"),
  };
  for (const std::string& body : invalid)
  {
    FakeSubtitleTransport transport;
    transport.replies.push_back(JsonReply(body));
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A).status,
              JumpgateSubtitleResultStatus::ProtocolFailure)
        << body;
  }
}

TEST(TestJumpgateSubtitleClient, RejectsPartAndAggregateMetadataAboveStagingCaps)
{
  const std::string oversizedPart = std::regex_replace(
      TextResolveResponse(), std::regex(R"(contentLength":12)"), R"(contentLength":8388609)");
  const std::string oversizedAggregate = std::regex_replace(
      std::regex_replace(VobSubResolveResponse(), std::regex(R"(contentLength":11)"),
                         R"(contentLength":7340032)"),
      std::regex(R"(contentLength":13)"), R"(contentLength":6291456)");
  for (const std::string& body : {oversizedPart, oversizedAggregate})
  {
    FakeSubtitleTransport transport;
    transport.replies.push_back(JsonReply(body));
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A).status,
              JumpgateSubtitleResultStatus::ProtocolFailure);
  }
}

TEST(TestJumpgateSubtitleClient, PreservesOnlyCanonicalTwoPartVobSub)
{
  FakeSubtitleTransport transport;
  transport.replies.push_back(JsonReply(VobSubResolveResponse()));
  transport.replies.push_back(JsonReply(VobSubResolveResponse(false)));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  const auto valid = client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A);
  ASSERT_EQ(valid.status, JumpgateSubtitleResultStatus::Success);
  ASSERT_EQ(valid.artifact.parts.size(), 2u);
  EXPECT_EQ(valid.artifact.parts[0].fileName.substr(0, 64),
            valid.artifact.parts[1].fileName.substr(0, 64));
  EXPECT_EQ(valid.artifact.parts[0].role, "index");
  EXPECT_EQ(valid.artifact.parts[1].role, "sub");
  EXPECT_EQ(valid.artifact.parts[0].sha256, std::string(64, 'd'));
  EXPECT_EQ(valid.artifact.parts[1].sha256, std::string(64, 'e'));

  EXPECT_EQ(client.Resolve(ORIGIN, authority, SESSION, SELECTOR_A).status,
            JumpgateSubtitleResultStatus::ProtocolFailure);
}

TEST(TestJumpgateSubtitleClient, ValidatesDeliveryMimeLengthAndComputesChecksum)
{
  const std::string payload = "WEBVTT\n\nA\n";
  FakeSubtitleTransport transport;
  Reply valid;
  valid.contentType = "text/vtt";
  valid.contentLength = payload.size();
  valid.body = Bytes(payload);
  transport.replies.push_back(std::move(valid));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  const JumpgateSubtitlePartResult result =
      client.Download(ORIGIN, authority, TextDescriptor(payload.size()));

  ASSERT_EQ(result.status, JumpgateSubtitleResultStatus::Success);
  EXPECT_EQ(result.part.bytes, Bytes(payload));
  EXPECT_EQ(result.part.sha256, TEXT_SHA256);
  EXPECT_EQ(transport.methods[0], JumpgateSubtitleHttpMethod::Get);
  EXPECT_TRUE(transport.contentTypes[0].empty());
  EXPECT_TRUE(transport.bodies[0].empty());
  EXPECT_EQ(transport.responseCaps[0], payload.size());
}

TEST(TestJumpgateSubtitleClient, RejectsChecksumEncodingAndRangeContractMismatch)
{
  const std::string payload = "WEBVTT\n\nA\n";
  for (int scenario = 0; scenario < 3; ++scenario)
  {
    Reply reply;
    reply.contentType = "text/vtt";
    reply.contentLength = payload.size();
    reply.body = Bytes(payload);
    JumpgateSubtitlePartDescriptor descriptor = TextDescriptor(payload.size());
    if (scenario == 0)
      descriptor.sha256 = std::string(64, '0');
    else if (scenario == 1)
      reply.contentEncoding = "gzip";
    else
      reply.acceptRanges = "bytes";

    FakeSubtitleTransport transport;
    transport.replies.push_back(std::move(reply));
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Download(ORIGIN, authority, descriptor).status,
              JumpgateSubtitleResultStatus::ProtocolFailure);
  }
}

TEST(TestJumpgateSubtitleClient, RejectsDeliveryMetadataMismatchAndCrossOriginCompletion)
{
  const std::string payload = "WEBVTT\n\nA\n";
  std::vector<Reply> invalid;
  Reply wrongLength;
  wrongLength.contentType = "text/vtt";
  wrongLength.contentLength = payload.size() + 1;
  wrongLength.body = Bytes(payload);
  invalid.push_back(std::move(wrongLength));
  Reply wrongMime;
  wrongMime.contentType = "text/plain";
  wrongMime.contentLength = payload.size();
  wrongMime.body = Bytes(payload);
  invalid.push_back(std::move(wrongMime));
  Reply shortBody;
  shortBody.contentType = "text/vtt";
  shortBody.contentLength = payload.size();
  shortBody.body = Bytes("short");
  invalid.push_back(std::move(shortBody));
  Reply crossOrigin;
  crossOrigin.contentType = "text/vtt";
  crossOrigin.contentLength = payload.size();
  crossOrigin.body = Bytes(payload);
  crossOrigin.effectiveUrl = "https://attacker.example/subtitle.vtt";
  invalid.push_back(std::move(crossOrigin));

  for (Reply& reply : invalid)
  {
    FakeSubtitleTransport transport;
    transport.replies.push_back(std::move(reply));
    CJumpgateSubtitleClient client{transport};
    CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
    EXPECT_EQ(client.Download(ORIGIN, authority, TextDescriptor(payload.size())).status,
              JumpgateSubtitleResultStatus::ProtocolFailure);
  }

  JumpgateSubtitlePartDescriptor mismatched = TextDescriptor(payload.size());
  mismatched.role = "index";
  FakeSubtitleTransport transport;
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};
  EXPECT_EQ(client.Download(ORIGIN, authority, mismatched).status,
            JumpgateSubtitleResultStatus::InvalidRequest);
  EXPECT_EQ(transport.calls, 0);
}

TEST(TestJumpgateSubtitleClient, RedactedSummariesNeverContainAuthorityBodyOrRoute)
{
  FakeSubtitleTransport transport;
  transport.replies.push_back(JsonReply(DiscoverResponse()));
  CJumpgateSubtitleClient client{transport};
  CJumpgateSubtitleBearerAuthority authority{DEVICE_TOKEN};

  ASSERT_EQ(client.Discover(ORIGIN, authority, SESSION).status,
            JumpgateSubtitleResultStatus::Success);
  ASSERT_EQ(transport.summaries.size(), 1u);
  EXPECT_EQ(transport.summaries[0], "subtitle_http method=POST url=<redacted> bearer=<redacted>");
  EXPECT_EQ(transport.summaries[0].find(DEVICE_TOKEN), std::string::npos);
  EXPECT_EQ(transport.summaries[0].find(SESSION), std::string::npos);
  EXPECT_EQ(transport.summaries[0].find(ORIGIN), std::string::npos);
  EXPECT_EQ(authority.RedactedSummary(), "bearer=<redacted>");

  CJumpgateSubtitleBearerAuthority moved{std::move(authority)};
  EXPECT_FALSE(authority.IsPresent());
  EXPECT_TRUE(moved.IsPresent());
  EXPECT_EQ(moved.RedactedSummary().find(DEVICE_TOKEN), std::string::npos);
}
