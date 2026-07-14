/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/JumpgateSubtitleCoordinator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
const std::string DEVICE_TOKEN(43, 'T');
const std::string SELECTOR_EN(64, 'a');
const std::string SELECTOR_ES(64, 'b');
const std::string ARTIFACT = "artifact_00000001";
const std::string BASE_NAME(64, 'c');
const std::string TEXT_PAYLOAD = "WEBVTT\n\nA\n";
const std::string INDEX_PAYLOAD = "# VobSub\n";
const std::vector<std::uint8_t> SUB_PAYLOAD{0x00, 0x00, 0x01, 0xba, 0x44, 0x02};
const std::string INDEX_SHA256 = "d4e6270fc336d62c9814128f08c7b85861773789fd959c4b804f975929d940ed";
const std::string SUB_SHA256 = "3724f9de0017a850b18d1c84d3528f127e510f96b1b7aaff6e5cf83dfc9fc61f";

std::vector<std::uint8_t> Bytes(const std::string& value)
{
  return {value.begin(), value.end()};
}

bool EndsWith(const std::string& value, const std::string& suffix)
{
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string DiscoverResponse()
{
  return "{\"schemaVersion\":1,\"subtitles\":[{\"selector\":\"" + SELECTOR_EN +
         "\",\"language\":\"en\",\"format\":\"vtt\",\"label\":\"English - VTT\",\"rank\":1},"
         "{\"selector\":\"" +
         SELECTOR_ES +
         "\",\"language\":\"es\",\"format\":\"vtt\",\"label\":\"Spanish - VTT\",\"rank\":2}]}";
}

std::string ResolveResponse(const std::string& sessionId,
                            bool vobSub,
                            std::int64_t expiresAt,
                            std::size_t textPayloadSize,
                            const std::string& textSha256)
{
  if (!vobSub)
  {
    const std::string fileName = BASE_NAME + ".vtt";
    return "{\"schemaVersion\":2,\"status\":\"ready\",\"artifactId\":\"" + ARTIFACT +
           "\",\"expiresAt\":" + std::to_string(expiresAt) +
           ",\"expiresAtUnit\":\"unix_ms\",\"parts\":[{\"partNumber\":1,"
           "\"role\":\"subtitle\",\"contentLength\":" +
           std::to_string(textPayloadSize) + ",\"contentType\":\"text/vtt\",\"fileName\":\"" +
           fileName + "\",\"path\":\"/v1/subtitles/" + sessionId + "/" + ARTIFACT + "/1/" +
           fileName + "\",\"sha256\":\"" + textSha256 + "\"}]}";
  }

  const std::string indexName = BASE_NAME + ".idx";
  const std::string subName = BASE_NAME + ".sub";
  return "{\"schemaVersion\":2,\"status\":\"ready\",\"artifactId\":\"" + ARTIFACT +
         "\",\"expiresAt\":" + std::to_string(expiresAt) +
         ",\"expiresAtUnit\":\"unix_ms\",\"parts\":[{\"partNumber\":1,"
         "\"role\":\"index\",\"contentLength\":" +
         std::to_string(INDEX_PAYLOAD.size()) +
         ",\"contentType\":\"application/x-vobsub\",\"fileName\":\"" + indexName +
         "\",\"path\":\"/v1/subtitles/" + sessionId + "/" + ARTIFACT + "/1/" + indexName +
         "\",\"sha256\":\"" + INDEX_SHA256 +
         "\"},{\"partNumber\":2,\"role\":\"sub\",\"contentLength\":" +
         std::to_string(SUB_PAYLOAD.size()) +
         ",\"contentType\":\"application/octet-stream\",\"fileName\":\"" + subName +
         "\",\"path\":\"/v1/subtitles/" + sessionId + "/" + ARTIFACT + "/2/" + subName +
         "\",\"sha256\":\"" + SUB_SHA256 + "\"}]}";
}

class WorkflowTransport final : public IJumpgateSubtitleTransport
{
public:
  bool Perform(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_active;
    m_maxActive = std::max(m_maxActive, m_active);
    m_authorizations.push_back(request.authorization);
    response.effectiveUrl = request.url;

    if (EndsWith(request.url, "/v1/subtitles/discover"))
    {
      ++m_discoverCalls;
      m_condition.notify_all();
      if (m_blockFirstDiscover && m_discoverCalls == 1)
      {
        while (!m_releaseFirstDiscover)
        {
          if (cancellation.IsCancelled())
          {
            m_cancellationObserved = true;
            m_condition.notify_all();
            if (!m_ignoreCancellation)
              break;
          }
          m_condition.wait_for(lock, 5ms);
        }
        if (cancellation.IsCancelled() && !m_ignoreCancellation)
        {
          --m_active;
          m_condition.notify_all();
          return false;
        }
      }
      response.statusCode = m_discoverStatus;
      response.contentType = "application/json; charset=utf-8";
      response.body = Bytes(m_discoverStatus == 200 ? DiscoverResponse() : "{}");
      --m_active;
      m_condition.notify_all();
      return true;
    }

    if (EndsWith(request.url, "/v1/subtitles/resolve"))
    {
      ++m_resolveCalls;
      if (m_retryableResolveResponses > 0)
      {
        --m_retryableResolveResponses;
        response.statusCode = m_retryableResolveStatus;
        response.retryAfter = m_retryAfter;
        response.contentType = "application/json; charset=utf-8";
        response.body = Bytes(R"({"ok":false,"error":"subtitle_busy"})");
      }
      else
      {
        CVariant input;
        std::string sessionId;
        if (CJSONVariantParser::Parse(request.body, input) && input.isObject() &&
            input.isMember("sessionId"))
        {
          sessionId = input["sessionId"].asString();
        }
        response.statusCode = 200;
        response.contentType = "application/json; charset=utf-8";
        const std::string textSha256 = KODI::UTILITY::CDigest::Calculate(
            KODI::UTILITY::CDigest::Type::SHA256, m_textPayload.data(), m_textPayload.size());
        response.body = Bytes(
            ResolveResponse(sessionId, m_vobSub, m_expiresAt, m_textPayload.size(), textSha256));
      }
      --m_active;
      m_condition.notify_all();
      return true;
    }

    ++m_downloadCalls;
    if (m_softDownloadResponses > 0)
    {
      --m_softDownloadResponses;
      --m_active;
      m_condition.notify_all();
      return false;
    }
    response.statusCode = 200;
    if (EndsWith(request.url, ".idx"))
    {
      response.contentType = "application/x-vobsub";
      response.body = Bytes(INDEX_PAYLOAD);
    }
    else if (EndsWith(request.url, ".sub"))
    {
      response.contentType = "application/octet-stream";
      response.body = SUB_PAYLOAD;
    }
    else
    {
      response.contentType = "text/vtt";
      response.body = m_textPayload;
    }
    response.contentLength = response.body.size();
    response.contentEncoding = "identity";
    response.acceptRanges = "none";
    --m_active;
    m_condition.notify_all();
    return true;
  }

  void BlockFirstDiscover(bool ignoreCancellation)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockFirstDiscover = true;
    m_ignoreCancellation = ignoreCancellation;
  }

  bool WaitForDiscoverCalls(int count)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [this, count] { return m_discoverCalls >= count; });
  }

  bool WaitForCancellationObserved()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [this] { return m_cancellationObserved; });
  }

  bool CancellationObserved() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancellationObserved;
  }

  void ReleaseFirstDiscover()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_releaseFirstDiscover = true;
    m_condition.notify_all();
  }

  void SetBusyResolveResponses(int count)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_retryableResolveResponses = count;
    m_retryableResolveStatus = 409;
    m_retryAfter = "1";
  }

  void SetLimitedResolveResponses(int count, std::string retryAfter = "1")
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_retryableResolveResponses = count;
    m_retryableResolveStatus = 429;
    m_retryAfter = std::move(retryAfter);
  }

  void SetSoftDownloadResponses(int count)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_softDownloadResponses = count;
  }

  void SetVobSub(bool enabled)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vobSub = enabled;
  }

  void SetExpiresAt(std::int64_t expiresAt)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_expiresAt = expiresAt;
  }

  void SetTextPayloadSize(std::size_t size)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_textPayload.assign(size, 0x41);
  }

  int DiscoverCalls() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_discoverCalls;
  }

  int ResolveCalls() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resolveCalls;
  }

  int DownloadCalls() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_downloadCalls;
  }

  int MaxActive() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxActive;
  }

  int Active() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active;
  }

  bool EveryAuthorizationIsExact() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_authorizations.empty() &&
           std::all_of(m_authorizations.begin(), m_authorizations.end(),
                       [](const std::string& value) { return value == "Bearer " + DEVICE_TOKEN; });
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  int m_active{0};
  int m_maxActive{0};
  int m_discoverCalls{0};
  int m_resolveCalls{0};
  int m_downloadCalls{0};
  int m_retryableResolveResponses{0};
  int m_retryableResolveStatus{409};
  int m_softDownloadResponses{0};
  int m_discoverStatus{200};
  std::int64_t m_expiresAt{1783900900000LL};
  bool m_vobSub{false};
  bool m_blockFirstDiscover{false};
  bool m_ignoreCancellation{false};
  bool m_releaseFirstDiscover{false};
  bool m_cancellationObserved{false};
  std::string m_retryAfter{"1"};
  std::vector<std::uint8_t> m_textPayload{Bytes(TEXT_PAYLOAD)};
  std::vector<std::string> m_authorizations;
};

JumpgateSubtitleBinding Binding(std::uint64_t generation,
                                std::string sessionId = "session_00000001")
{
  return {generation, "profile_00000001", "device_00000001", "https://bridge.example",
          std::move(sessionId)};
}

JumpgateSubtitleRequest Request(JumpgateSubtitleBinding binding,
                                std::vector<std::string> languages = {"es", "en"})
{
  return {std::move(binding), CJumpgateSubtitleBearerAuthority{DEVICE_TOKEN}, std::move(languages)};
}

JumpgateSubtitleCoordinatorOptions Options(std::uint32_t attempts = 3)
{
  JumpgateSubtitleCoordinatorOptions options;
  options.maximumAttempts = attempts;
  options.retryAfterSecond = 1ms;
  options.softFailureDelay = 1ms;
  options.nowMilliseconds = [] { return 1783900800000LL; };
  return options;
}

std::optional<JumpgateSubtitleCompletion> WaitForCompletion(
    CJumpgateSubtitleCoordinator& coordinator, const JumpgateSubtitleBinding& binding)
{
  for (int attempt = 0; attempt < 300; ++attempt)
  {
    if (auto completion = coordinator.TakeCompletion(binding))
      return completion;
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}
} // namespace

TEST(TestJumpgateSubtitleCoordinator, StagesSelectedTextWithoutInjectingIntoPlayer)
{
  auto transport = std::make_shared<WorkflowTransport>();
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding binding = Binding(1);
  ASSERT_TRUE(coordinator.Queue(Request(binding)));

  JumpgateSubtitleBinding wrong = binding;
  wrong.profileId = "profile_00000002";
  EXPECT_FALSE(coordinator.TakeCompletion(wrong));
  wrong = binding;
  wrong.deviceId = "device_00000002";
  EXPECT_FALSE(coordinator.TakeCompletion(wrong));
  wrong = binding;
  wrong.bridgeOrigin = "https://other.example";
  EXPECT_FALSE(coordinator.TakeCompletion(wrong));
  wrong = binding;
  wrong.sessionId = "session_00000002";
  EXPECT_FALSE(coordinator.TakeCompletion(wrong));

  const auto completion = WaitForCompletion(coordinator, binding);
  ASSERT_TRUE(completion);
  ASSERT_EQ(completion->status, JumpgateSubtitleResultStatus::Success);
  EXPECT_EQ(completion->binding.generation, 1u);
  EXPECT_EQ(completion->artifact.selected.language, "es");
  EXPECT_EQ(completion->artifact.artifactId, ARTIFACT);
  ASSERT_EQ(completion->artifact.parts.size(), 1u);
  EXPECT_EQ(completion->artifact.parts[0].bytes, Bytes(TEXT_PAYLOAD));
  EXPECT_EQ(completion->artifact.parts[0].sha256.size(), 64u);
  EXPECT_EQ(transport->DiscoverCalls(), 1);
  EXPECT_EQ(transport->ResolveCalls(), 1);
  EXPECT_EQ(transport->DownloadCalls(), 1);
  EXPECT_EQ(transport->MaxActive(), 1);
  EXPECT_TRUE(transport->EveryAuthorizationIsExact());
  EXPECT_TRUE(coordinator.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, RetriesBusyAndSoftFailuresWithinConfiguredBound)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->SetBusyResolveResponses(2);
  transport->SetSoftDownloadResponses(1);
  CJumpgateSubtitleCoordinator coordinator{transport, Options(3)};
  const JumpgateSubtitleBinding binding = Binding(2);
  ASSERT_TRUE(coordinator.Queue(Request(binding)));

  const auto completion = WaitForCompletion(coordinator, binding);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->status, JumpgateSubtitleResultStatus::Success);
  EXPECT_EQ(transport->ResolveCalls(), 3);
  EXPECT_EQ(transport->DownloadCalls(), 2);
  EXPECT_TRUE(coordinator.Stop());

  auto exhaustedTransport = std::make_shared<WorkflowTransport>();
  exhaustedTransport->SetBusyResolveResponses(10);
  CJumpgateSubtitleCoordinator exhausted{exhaustedTransport, Options(3)};
  const JumpgateSubtitleBinding exhaustedBinding = Binding(3, "session_00000003");
  ASSERT_TRUE(exhausted.Queue(Request(exhaustedBinding)));
  const auto exhaustedCompletion = WaitForCompletion(exhausted, exhaustedBinding);
  ASSERT_TRUE(exhaustedCompletion);
  EXPECT_EQ(exhaustedCompletion->status, JumpgateSubtitleResultStatus::RetryableBusy);
  EXPECT_EQ(exhaustedTransport->ResolveCalls(), 3);
  EXPECT_TRUE(exhausted.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, Retries429ThenSucceedsAndExhaustsAtSameBoundAs409)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->SetLimitedResolveResponses(2);
  CJumpgateSubtitleCoordinator coordinator{transport, Options(3)};
  const JumpgateSubtitleBinding binding = Binding(13, "session_00000013");
  ASSERT_TRUE(coordinator.Queue(Request(binding)));
  const auto completion = WaitForCompletion(coordinator, binding);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->status, JumpgateSubtitleResultStatus::Success);
  EXPECT_EQ(transport->ResolveCalls(), 3);
  EXPECT_TRUE(coordinator.Stop());

  auto exhaustedTransport = std::make_shared<WorkflowTransport>();
  exhaustedTransport->SetLimitedResolveResponses(10);
  CJumpgateSubtitleCoordinator exhausted{exhaustedTransport, Options(3)};
  const JumpgateSubtitleBinding exhaustedBinding = Binding(14, "session_00000014");
  ASSERT_TRUE(exhausted.Queue(Request(exhaustedBinding)));
  const auto exhaustedCompletion = WaitForCompletion(exhausted, exhaustedBinding);
  ASSERT_TRUE(exhaustedCompletion);
  EXPECT_EQ(exhaustedCompletion->status, JumpgateSubtitleResultStatus::RetryableBusy);
  EXPECT_EQ(exhaustedCompletion->httpStatus, 429);
  EXPECT_EQ(exhaustedTransport->ResolveCalls(), 3);
  EXPECT_TRUE(exhausted.Stop());

  auto malformedTransport = std::make_shared<WorkflowTransport>();
  malformedTransport->SetLimitedResolveResponses(10, "61");
  CJumpgateSubtitleCoordinator malformed{malformedTransport, Options(3)};
  const JumpgateSubtitleBinding malformedBinding = Binding(15, "session_00000015");
  ASSERT_TRUE(malformed.Queue(Request(malformedBinding)));
  const auto malformedCompletion = WaitForCompletion(malformed, malformedBinding);
  ASSERT_TRUE(malformedCompletion);
  EXPECT_EQ(malformedCompletion->status, JumpgateSubtitleResultStatus::ProtocolFailure);
  EXPECT_EQ(malformedCompletion->httpStatus, 429);
  EXPECT_EQ(malformedTransport->ResolveCalls(), 1);
  EXPECT_TRUE(malformed.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, StagesCanonicalVobSubPairWithSameBasename)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->SetVobSub(true);
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding binding = Binding(4, "session_00000004");
  ASSERT_TRUE(coordinator.Queue(Request(binding, {"en"})));

  const auto completion = WaitForCompletion(coordinator, binding);
  ASSERT_TRUE(completion);
  ASSERT_EQ(completion->status, JumpgateSubtitleResultStatus::Success);
  ASSERT_EQ(completion->artifact.parts.size(), 2u);
  EXPECT_EQ(completion->artifact.parts[0].role, "index");
  EXPECT_EQ(completion->artifact.parts[1].role, "sub");
  EXPECT_EQ(completion->artifact.parts[0].fileName.substr(0, 64),
            completion->artifact.parts[1].fileName.substr(0, 64));
  EXPECT_EQ(completion->artifact.parts[0].bytes, Bytes(INDEX_PAYLOAD));
  EXPECT_EQ(completion->artifact.parts[1].bytes, SUB_PAYLOAD);
  EXPECT_EQ(transport->DownloadCalls(), 2);
  EXPECT_TRUE(coordinator.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, NewGenerationCancelsAndRejectsStaleCompletion)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->BlockFirstDiscover(true);
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding oldBinding = Binding(5, "session_00000005");
  const JumpgateSubtitleBinding newBinding = Binding(6, "session_00000006");
  ASSERT_TRUE(coordinator.Queue(Request(oldBinding)));
  ASSERT_TRUE(transport->WaitForDiscoverCalls(1));
  ASSERT_TRUE(coordinator.Queue(Request(newBinding)));
  transport->ReleaseFirstDiscover();

  const auto completion = WaitForCompletion(coordinator, newBinding);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->binding.generation, 6u);
  EXPECT_EQ(completion->binding.sessionId, "session_00000006");
  EXPECT_FALSE(coordinator.TakeCompletion(oldBinding));
  EXPECT_EQ(transport->DiscoverCalls(), 2);
  EXPECT_EQ(transport->MaxActive(), 1);
  EXPECT_TRUE(coordinator.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, ExplicitCancellationStopsTransportAndPublishesNothing)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->BlockFirstDiscover(false);
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding binding = Binding(7, "session_00000007");
  ASSERT_TRUE(coordinator.Queue(Request(binding)));
  ASSERT_TRUE(transport->WaitForDiscoverCalls(1));
  ASSERT_TRUE(coordinator.Cancel(binding));
  for (int attempt = 0; attempt < 100 && transport->Active() != 0; ++attempt)
    std::this_thread::sleep_for(5ms);
  EXPECT_FALSE(coordinator.TakeCompletion(binding));
  EXPECT_FALSE(coordinator.Cancel(Binding(8, "session_00000008")));
  EXPECT_TRUE(coordinator.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, RejectsOldInvalidAndPostStopRequests)
{
  auto transport = std::make_shared<WorkflowTransport>();
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding binding = Binding(9, "session_00000009");
  ASSERT_TRUE(coordinator.Queue(Request(binding)));
  EXPECT_FALSE(coordinator.Queue(Request(binding)));
  EXPECT_FALSE(coordinator.Queue(Request(Binding(8, "session_00000008"))));

  JumpgateSubtitleBinding invalid = Binding(10, "session_00000010");
  invalid.bridgeOrigin = "https://bridge.example?credential=x";
  EXPECT_FALSE(coordinator.Queue(Request(invalid)));
  EXPECT_FALSE(coordinator.Queue(Request(Binding(10, "session_00000010"), {"EN"})));
  EXPECT_TRUE(coordinator.Stop());
  EXPECT_FALSE(coordinator.Queue(Request(Binding(11, "session_00000011"))));
}

TEST(TestJumpgateSubtitleCoordinator, RejectsArtifactThatExpiresBeforePublication)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->SetExpiresAt(1783900800000LL);
  CJumpgateSubtitleCoordinator coordinator{transport, Options()};
  const JumpgateSubtitleBinding binding = Binding(12, "session_00000012");
  ASSERT_TRUE(coordinator.Queue(Request(binding)));

  const auto completion = WaitForCompletion(coordinator, binding);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->status, JumpgateSubtitleResultStatus::Stale);
  EXPECT_TRUE(completion->artifact.artifactId.empty());
  EXPECT_EQ(completion->artifact.expiresAt, 0);
  EXPECT_TRUE(completion->artifact.parts.empty());
  EXPECT_EQ(transport->DownloadCalls(), 0);
  EXPECT_TRUE(coordinator.Stop());
}

TEST(TestJumpgateSubtitleCoordinator, CompletedPayloadReturnedBeforeCancelClearsOffCaller)
{
  auto transport = std::make_shared<WorkflowTransport>();
  transport->SetTextPayloadSize(8ULL * 1024ULL * 1024ULL);
  std::mutex observationMutex;
  std::condition_variable observationCondition;
  bool published = false;
  bool cleared = false;
  std::thread::id clearThread;
  JumpgateSubtitleCoordinatorOptions options = Options();
  options.completionPublishedObserver = [&](const JumpgateSubtitleBinding&)
  {
    std::lock_guard lock(observationMutex);
    published = true;
    observationCondition.notify_all();
  };
  options.completionClearObserver = [&](const JumpgateSubtitleCompletion& completion)
  {
    std::lock_guard lock(observationMutex);
    clearThread = std::this_thread::get_id();
    cleared = completion.artifact.parts.empty();
    observationCondition.notify_all();
  };
  CJumpgateSubtitleCoordinator coordinator{transport, std::move(options)};
  const JumpgateSubtitleBinding binding = Binding(9);
  ASSERT_TRUE(coordinator.Queue(Request(binding)));
  {
    std::unique_lock lock(observationMutex);
    ASSERT_TRUE(observationCondition.wait_for(lock, 5s, [&] { return published; }));
  }

  std::optional<JumpgateSubtitleCompletion> completion = coordinator.TakeCompletion(binding);
  ASSERT_TRUE(completion);
  ASSERT_TRUE(coordinator.ReturnCompletion(std::move(*completion)));
  const std::thread::id caller = std::this_thread::get_id();
  ASSERT_TRUE(coordinator.Cancel(binding));
  {
    std::unique_lock lock(observationMutex);
    ASSERT_TRUE(observationCondition.wait_for(lock, 5s, [&] { return cleared; }));
  }
  EXPECT_NE(clearThread, caller);
  EXPECT_TRUE(coordinator.Stop(1s));
}

TEST(TestJumpgateSubtitleCoordinator, StopOwnsCoordinatorUntilBlockedWorkerIsJoined)
{
  auto transport = std::make_shared<WorkflowTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->BlockFirstDiscover(true);
  CJumpgateSubtitleCoordinator coordinator{transport, Options(), registry};
  const JumpgateSubtitleBinding binding = Binding(1, "session_contention_0001");
  ASSERT_TRUE(coordinator.Queue(Request(binding)));
  ASSERT_TRUE(transport->WaitForDiscoverCalls(1));

  auto primaryStop =
      std::async(std::launch::async, [&coordinator] { return coordinator.Stop(5s); });
  const bool cancellationObserved = transport->WaitForCancellationObserved();
  if (!cancellationObserved)
    transport->ReleaseFirstDiscover();
  ASSERT_TRUE(cancellationObserved);

  std::mutex gateMutex;
  std::condition_variable gateCondition;
  std::size_t readyContenders{0};
  std::size_t releasedContenders{0};
  bool releaseContenders{false};
  const auto contend = [&]
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    ++readyContenders;
    gateCondition.notify_all();
    gateCondition.wait(lock, [&releaseContenders] { return releaseContenders; });
    ++releasedContenders;
    gateCondition.notify_all();
  };
  auto queue =
      std::async(std::launch::async,
                 [&]
                 {
                   contend();
                   return coordinator.Queue(Request(Binding(2, "session_contention_0002")));
                 });
  auto cancel = std::async(std::launch::async,
                           [&]
                           {
                             contend();
                             return coordinator.Cancel(binding);
                           });
  auto take = std::async(std::launch::async,
                         [&]
                         {
                           contend();
                           return coordinator.TakeCompletion(binding);
                         });
  auto repeatedStopOne = std::async(std::launch::async,
                                    [&]
                                    {
                                      contend();
                                      return coordinator.Stop(5s);
                                    });
  auto repeatedStopTwo = std::async(std::launch::async,
                                    [&]
                                    {
                                      contend();
                                      return coordinator.Stop(5s);
                                    });

  bool allContendersReady = false;
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    allContendersReady =
        gateCondition.wait_for(lock, 2s, [&readyContenders] { return readyContenders == 5; });
    releaseContenders = true;
  }
  gateCondition.notify_all();
  bool allContendersReleased = false;
  if (allContendersReady)
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    allContendersReleased =
        gateCondition.wait_for(lock, 2s, [&releasedContenders] { return releasedContenders == 5; });
  }
  if (!allContendersReady || !allContendersReleased)
    transport->ReleaseFirstDiscover();
  ASSERT_TRUE(allContendersReady);
  ASSERT_TRUE(allContendersReleased);

  // Removing owner serialization makes at least Queue, Cancel, or TakeCompletion finish here.
  EXPECT_TRUE(queue.wait_for(100ms) == std::future_status::timeout);
  EXPECT_TRUE(cancel.wait_for(100ms) == std::future_status::timeout);
  EXPECT_TRUE(take.wait_for(100ms) == std::future_status::timeout);
  EXPECT_TRUE(repeatedStopOne.wait_for(100ms) == std::future_status::timeout);
  EXPECT_TRUE(repeatedStopTwo.wait_for(100ms) == std::future_status::timeout);

  transport->ReleaseFirstDiscover();
  EXPECT_TRUE(primaryStop.get());
  EXPECT_FALSE(queue.get());
  EXPECT_FALSE(cancel.get());
  EXPECT_FALSE(take.get());
  EXPECT_TRUE(repeatedStopOne.get());
  EXPECT_TRUE(repeatedStopTwo.get());
  EXPECT_EQ(transport->Active(), 0);
  EXPECT_EQ(registry->Pending(), 0u);
}

TEST(TestJumpgateSubtitleCoordinator, DestructionCancelsAndWaitsWithoutStagingArtifact)
{
  auto transport = std::make_shared<WorkflowTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->BlockFirstDiscover(false);
  {
    CJumpgateSubtitleCoordinator coordinator{transport, Options(), registry};
    const JumpgateSubtitleBinding binding = Binding(16, "session_00000016");
    ASSERT_TRUE(coordinator.Queue(Request(binding)));
    ASSERT_TRUE(transport->WaitForDiscoverCalls(1));
  }

  EXPECT_TRUE(transport->CancellationObserved());
  EXPECT_EQ(transport->Active(), 0);
  EXPECT_EQ(transport->ResolveCalls(), 0);
  EXPECT_EQ(transport->DownloadCalls(), 0);
  EXPECT_EQ(registry->Pending(), 0u);
}
