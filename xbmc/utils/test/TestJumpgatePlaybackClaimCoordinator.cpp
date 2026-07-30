/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgatePlaybackClaimCoordinator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
constexpr const char* ATTEMPT_ID = "018f47a2-5b6c-7d8e-9f01-23456789abcd";
constexpr const char* RECEIPT_ID = "018f47a2-5b6c-7d8e-af01-23456789abcd";
const std::string DEVICE_TOKEN(43, 'D');

struct CapturedRequest final
{
  std::string url;
  std::string body;
  std::vector<JumpgatePlaybackHttpHeader> headers;
};

std::string HeaderValue(const JumpgatePlaybackHttpRequest& request, const std::string& name)
{
  for (const auto& header : request.headers)
  {
    if (header.name == name)
      return header.value;
  }
  return {};
}

class CoordinatedTransport final : public IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const JumpgatePlaybackHttpRequest& request,
            JumpgatePlaybackHttpResponse& response) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_requests.push_back({request.url, request.body, request.headers});
    m_condition.notify_all();

    if (request.url == std::string(ORIGIN) + "/v1/playback/claim")
    {
      ++m_claims;
      if (m_throwFirstClaim && m_claims == 1)
        throw std::runtime_error("scripted claim transport failure");
      if (m_blockFirstClaim && m_claims == 1)
        m_condition.wait(lock, [this] { return m_unblocked; });
      const std::size_t index = static_cast<std::size_t>(m_claims);
      const std::string status =
          index <= m_claimStatuses.size() ? m_claimStatuses[index - 1] : "claimed";
      const std::string session = "session_0000000" + std::to_string(index);
      const std::string grant = "hg1_" + std::string(31, 'G') + std::to_string(index);
      const std::string kind = status == "claimed" ? "canonical" : "negative";
      m_grants[grant] = {session, kind};
      if (status == "claimed")
      {
        response.body =
            std::string{R"({"claimedAt":"2026-07-13T10:00:00.123Z","context":{"contentKey":")"} +
            std::string(64, 'c') +
            R"(","traktEligible":true},"expiresAt":"2026-07-13T10:05:00.123Z","historyGrant":")" +
            grant + "\",\"historyGrantKind\":\"canonical\",\"sessionId\":\"" + session +
            "\",\"sessionRevision\":1,\"status\":\"claimed\"}";
      }
      else
      {
        response.body = "{\"historyGrant\":\"" + grant +
                        "\",\"historyGrantKind\":\"negative\",\"sessionId\":\"" + session +
                        "\",\"sessionRevision\":1,\"status\":\"" + status + "\"}";
      }
      response.statusCode = 200;
      return true;
    }

    if (request.url == std::string(ORIGIN) + "/v1/history/events")
    {
      ++m_historyEvents;
      if (m_throwFirstHistory && m_historyEvents == 1)
        throw std::runtime_error("scripted history transport failure");
      CVariant body;
      if (!CJSONVariantParser::Parse(request.body, body))
        return false;
      const std::string grant = HeaderValue(request, "x-jumpgate-history-grant");
      const auto found = m_grants.find(grant);
      if (found == m_grants.end())
        return false;
      const std::string event = body["event"].asString();
      const std::string state = event == "stop" || event == "completion" ? "released" : "playing";
      response.statusCode = 200;
      response.body = "{\"dispatchIntent\":null,\"event\":\"" + event + "\",\"grantKind\":\"" +
                      found->second.second + "\",\"history\":null,\"ok\":true,\"sessionId\":\"" +
                      found->second.first + "\",\"sessionRevision\":" +
                      std::to_string(body["sessionRevision"].asUnsignedInteger() + 1) +
                      ",\"sessionState\":\"" + state + "\",\"status\":\"" +
                      (found->second.second == "canonical" ? "applied" : "local_only") + "\"}";
      return true;
    }

    if (request.url == std::string(ORIGIN) + "/v1/playback/release")
    {
      ++m_releases;
      if (m_throwFirstRelease && m_releases == 1)
        throw std::runtime_error("scripted release transport failure");
      response.statusCode = 200;
      response.body = R"({"status":"released"})";
      return true;
    }
    return false;
  }

  void SetClaimStatuses(std::vector<std::string> statuses)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_claimStatuses = std::move(statuses);
  }

  void BlockFirstClaim()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockFirstClaim = true;
  }

  void ThrowFirstClaim()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_throwFirstClaim = true;
  }

  void ThrowFirstHistory()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_throwFirstHistory = true;
  }

  void ThrowFirstRelease()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_throwFirstRelease = true;
  }

  void UnblockFirstClaim()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_unblocked = true;
    }
    m_condition.notify_all();
  }

  bool WaitForCalls(std::size_t count, std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, timeout,
                                [this, count] { return m_requests.size() >= count; });
  }

  std::vector<CapturedRequest> Requests() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
  }

  int Claims() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_claims;
  }
  int HistoryEvents() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_historyEvents;
  }
  int Releases() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_releases;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<CapturedRequest> m_requests;
  std::vector<std::string> m_claimStatuses;
  std::map<std::string, std::pair<std::string, std::string>> m_grants;
  int m_claims{0};
  int m_historyEvents{0};
  int m_releases{0};
  bool m_blockFirstClaim{false};
  bool m_unblocked{false};
  bool m_throwFirstClaim{false};
  bool m_throwFirstHistory{false};
  bool m_throwFirstRelease{false};
};

PlaybackClaimRequest ClaimRequest(char fingerprint)
{
  PlaybackClaimRequest request;
  request.bridgeOrigin = ORIGIN;
  request.deviceToken = DEVICE_TOKEN;
  request.attemptId = ATTEMPT_ID;
  request.fingerprints = {"v1:url:sha256:" + std::string(64, fingerprint)};
  request.intentUrlHash = std::string(64, fingerprint);
  request.launchedAt = 1783900800123LL;
  request.client = PlaybackClaimClientInfo{"android", "0.2.12"};
  return request;
}

std::optional<PlaybackClaimCompletion> WaitForCompletion(
    CJumpgatePlaybackClaimCoordinator& coordinator, std::chrono::milliseconds timeout = 2s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (auto completion = coordinator.TakeCompletion())
      return completion;
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}
} // namespace

TEST(TestJumpgatePlaybackClaimCoordinator, AcceptedClaimTransfersWithoutCoordinatorCleanup)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion);
  ASSERT_TRUE(completion->result.IsClaimed());
  ASSERT_TRUE(coordinator.AcceptCompletion(1));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->Claims(), 1);
  EXPECT_EQ(transport->HistoryEvents(), 0);
  EXPECT_EQ(transport->Releases(), 0);
}

TEST(TestJumpgatePlaybackClaimCoordinator, RejectedClaimTerminatesBeforeReceiptBoundRelease)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(WaitForCompletion(coordinator));
  ASSERT_TRUE(coordinator.RejectCompletion(1));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->HistoryEvents(), 1);
  EXPECT_EQ(transport->Releases(), 1);

  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 3u);
  CVariant release;
  ASSERT_TRUE(CJSONVariantParser::Parse(requests[2].body, release));
  std::string receipt;
  for (const auto& header : requests[1].headers)
  {
    if (header.name == "Idempotency-Key")
      receipt = header.value;
  }
  EXPECT_FALSE(receipt.empty());
  EXPECT_EQ(release["terminalReceiptId"].asString(), receipt);
}

TEST(TestJumpgatePlaybackClaimCoordinator, AcceptedNegativeClaimIsStillClosedWithTerminalProof)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->SetClaimStatuses({"not_found"});
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->result.status, PlaybackClaimStatus::NotFound);
  ASSERT_TRUE(coordinator.AcceptCompletion(1));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->HistoryEvents(), 1);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, NewClaimRunsBeforeStaleCleanup)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));
  ASSERT_TRUE(coordinator.QueueClaim(2, ClaimRequest('2')));
  transport->UnblockFirstClaim();

  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->generation, 2u);
  ASSERT_TRUE(coordinator.AcceptCompletion(2));
  EXPECT_TRUE(coordinator.Stop(true));

  const auto requests = transport->Requests();
  ASSERT_GE(requests.size(), 4u);
  EXPECT_EQ(requests[0].url, std::string(ORIGIN) + "/v1/playback/claim");
  EXPECT_EQ(requests[1].url, std::string(ORIGIN) + "/v1/playback/claim");
  EXPECT_EQ(transport->HistoryEvents(), 1);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, NonDrainingStopAdoptsLateCleanupThread)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport, registry};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(coordinator.Stop(false));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
  EXPECT_EQ(registry->Pending(), 1u);

  transport->UnblockFirstClaim();
  EXPECT_TRUE(registry->JoinAllFor(2s));
  EXPECT_EQ(transport->HistoryEvents(), 1);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, ExplicitReleaseRequiresTerminalReceipt)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  EXPECT_FALSE(coordinator.QueueRelease({ORIGIN, DEVICE_TOKEN, "session_manual_01", ""}));
  EXPECT_TRUE(coordinator.QueueRelease({ORIGIN, DEVICE_TOKEN, "session_manual_01", RECEIPT_ID}));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, ClaimExceptionDoesNotTerminateWorker)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->ThrowFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  const auto failed = WaitForCompletion(coordinator);
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->result.status, PlaybackClaimStatus::TransportFailure);
  ASSERT_TRUE(coordinator.AcceptCompletion(1));

  ASSERT_TRUE(coordinator.QueueClaim(2, ClaimRequest('2')));
  const auto recovered = WaitForCompletion(coordinator);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->generation, 2u);
  EXPECT_TRUE(recovered->result.IsClaimed());
  ASSERT_TRUE(coordinator.AcceptCompletion(2));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->Claims(), 2);
}

TEST(TestJumpgatePlaybackClaimCoordinator, CleanupRetriesThrownHistoryAndReleaseTransports)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->ThrowFirstHistory();
  transport->ThrowFirstRelease();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(WaitForCompletion(coordinator));
  ASSERT_TRUE(coordinator.RejectCompletion(1));
  EXPECT_TRUE(coordinator.Stop(true));
  EXPECT_EQ(transport->HistoryEvents(), 2);
  EXPECT_EQ(transport->Releases(), 2);
}
