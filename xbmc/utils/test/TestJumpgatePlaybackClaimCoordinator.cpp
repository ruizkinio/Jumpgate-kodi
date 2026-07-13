/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackClaimCoordinator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
bool EndsWith(const std::string& value, const std::string& suffix)
{
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

class CoordinatedTransport final : public IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const JumpgatePlaybackHttpRequest& request,
            JumpgatePlaybackHttpResponse& response) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_urls.push_back(request.url);
    ++m_calls;
    m_condition.notify_all();
    if (EndsWith(request.url, "/claim") && m_blockFirstClaim && m_claims == 0)
    {
      ++m_claims;
      m_condition.wait(lock, [this] { return m_releaseFirstClaim; });
    }
    else if (EndsWith(request.url, "/claim"))
    {
      ++m_claims;
    }
    else
    {
      ++m_releases;
    }

    response.statusCode = 200;
    if (EndsWith(request.url, "/claim"))
    {
      const std::string session = "session_" + std::to_string(m_claims) + "0000000";
      response.body =
          "{\"claimedAt\":\"2026-07-13T10:00:00.123Z\",\"context\":{\"schemaVersion\":1},"
          "\"expiresAt\":\"2026-07-13T10:05:00.123Z\",\"sessionId\":\"" +
          session + "\",\"status\":\"claimed\"}";
    }
    else
    {
      response.body = R"({"status":"released"})";
    }
    return true;
  }

  bool WaitForCalls(int count)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [this, count] { return m_calls >= count; });
  }

  void UnblockFirstClaim()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_releaseFirstClaim = true;
    m_condition.notify_all();
  }

  int Claims() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_claims;
  }

  int Releases() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_releases;
  }

  void BlockFirstClaim()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_blockFirstClaim = true;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<std::string> m_urls;
  int m_calls{0};
  int m_claims{0};
  int m_releases{0};
  bool m_blockFirstClaim{false};
  bool m_releaseFirstClaim{false};
};

PlaybackClaimRequest ClaimRequest(char hash)
{
  PlaybackClaimRequest request;
  request.bridgeOrigin = "https://bridge.example";
  request.deviceToken = std::string(43, 'A');
  request.fingerprints = {"v1:url:sha256:" + std::string(64, hash)};
  request.intentUrlHash = std::string(64, hash);
  request.launchedAt = 1783900800123LL;
  request.client = PlaybackClaimClientInfo{"android", "3.0.0"};
  return request;
}

PlaybackReleaseRequest ReleaseRequest(std::string session)
{
  return {"https://bridge.example", std::string(43, 'A'), std::move(session)};
}

std::optional<PlaybackClaimCompletion> WaitForCompletion(
    CJumpgatePlaybackClaimCoordinator& coordinator)
{
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    if (auto completion = coordinator.TakeCompletion())
      return completion;
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}
} // namespace

TEST(TestJumpgatePlaybackClaimCoordinator, NewestIntentWinsAndStaleClaimIsReleased)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));
  ASSERT_TRUE(coordinator.QueueClaim(2, ClaimRequest('2')));
  transport->UnblockFirstClaim();

  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->generation, 2u);
  EXPECT_TRUE(completion->result.IsClaimed());
  EXPECT_EQ(transport->Claims(), 2);
  EXPECT_GE(transport->Releases(), 1);
  coordinator.Stop(true);
}

TEST(TestJumpgatePlaybackClaimCoordinator, ReplacingUntakenCompletionReleasesItsSession)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));
  for (int attempt = 0; attempt < 100 && transport->Claims() < 1; ++attempt)
    std::this_thread::sleep_for(5ms);
  ASSERT_TRUE(coordinator.QueueClaim(2, ClaimRequest('2')));

  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->generation, 2u);
  EXPECT_GE(transport->Releases(), 1);
  coordinator.Stop(true);
}

TEST(TestJumpgatePlaybackClaimCoordinator, StopDrainsPendingReleasesWhenRequested)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueRelease(ReleaseRequest("session_drain")));
  coordinator.Stop(true);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, AcceptKeepsClaimedSession)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion);
  ASSERT_TRUE(completion->result.IsClaimed());
  EXPECT_TRUE(coordinator.AcceptCompletion(1));
  EXPECT_FALSE(coordinator.AcceptCompletion(1));
  coordinator.Stop(true);
  EXPECT_EQ(transport->Releases(), 0);
}

TEST(TestJumpgatePlaybackClaimCoordinator, RejectReleasesClaimedSession)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};

  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  const auto completion = WaitForCompletion(coordinator);
  ASSERT_TRUE(completion);
  ASSERT_TRUE(completion->result.IsClaimed());
  EXPECT_FALSE(coordinator.RejectCompletion(2));
  EXPECT_TRUE(coordinator.RejectCompletion(1));
  EXPECT_FALSE(coordinator.RejectCompletion(1));
  coordinator.Stop(true);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, RejectsOldOrPostStopGenerations)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(2, ClaimRequest('2')));
  EXPECT_FALSE(coordinator.QueueClaim(2, ClaimRequest('3')));
  EXPECT_FALSE(coordinator.QueueClaim(1, ClaimRequest('4')));
  coordinator.Stop(true);
  EXPECT_FALSE(coordinator.QueueClaim(3, ClaimRequest('5')));
  EXPECT_FALSE(coordinator.QueueRelease(ReleaseRequest("session_after_stop")));
}

TEST(TestJumpgatePlaybackClaimCoordinator, StopWithoutDrainReturnsWhileLateClaimIsReleased)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport, registry};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(coordinator.Stop(false));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, 100ms);
  EXPECT_EQ(transport->Claims(), 1);
  EXPECT_EQ(transport->Releases(), 0);
  EXPECT_EQ(registry->Pending(), 1u);

  transport->UnblockFirstClaim();
  EXPECT_TRUE(registry->JoinAllFor(2s));
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, StopWithDrainWaitsForLateClaimCleanup)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));

  std::atomic<bool> stopStarted{false};
  std::atomic<bool> stopped{false};
  std::thread stopThread(
      [&]
      {
        stopStarted.store(true);
        coordinator.Stop(true);
        stopped = true;
      });
  while (!stopStarted.load())
    std::this_thread::yield();
  EXPECT_FALSE(stopped);
  transport->UnblockFirstClaim();
  stopThread.join();
  EXPECT_TRUE(stopped);
  EXPECT_EQ(transport->Claims(), 1);
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, StopWithDrainReturnsAtDeadlineAndRegistryJoinsLater)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport, registry};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(coordinator.Stop(true, 50ms));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, 50ms);
  EXPECT_LT(elapsed, 250ms);
  EXPECT_EQ(registry->Pending(), 1u);
  EXPECT_EQ(transport->Releases(), 0);

  transport->UnblockFirstClaim();
  EXPECT_TRUE(registry->JoinAllFor(2s));
  EXPECT_EQ(transport->Releases(), 1);
}

TEST(TestJumpgatePlaybackClaimCoordinator, CapacityNeverEvictsDistinctRelease)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator coordinator{transport};
  ASSERT_TRUE(coordinator.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));
  for (int index = 0; index < 8; ++index)
  {
    ASSERT_TRUE(
        coordinator.QueueRelease(ReleaseRequest("session_capacity_" + std::to_string(index))));
  }
  EXPECT_TRUE(coordinator.QueueRelease(ReleaseRequest("session_capacity_0")));
  EXPECT_TRUE(coordinator.QueueRelease(ReleaseRequest("session_capacity_8")));
  transport->UnblockFirstClaim();
  coordinator.Stop(true);
  EXPECT_GE(transport->Releases(), 10);
}

TEST(TestJumpgatePlaybackClaimCoordinator, CapacityRejectionRetriesOnFreshOwnedCoordinator)
{
  auto transport = std::make_shared<CoordinatedTransport>();
  transport->BlockFirstClaim();
  CJumpgatePlaybackClaimCoordinator primary{transport};
  ASSERT_TRUE(primary.QueueClaim(1, ClaimRequest('1')));
  ASSERT_TRUE(transport->WaitForCalls(1));
  for (int index = 0; index < 256; ++index)
  {
    ASSERT_TRUE(
        primary.QueueRelease(ReleaseRequest("session_capacity_full_" + std::to_string(index))));
  }

  EXPECT_FALSE(primary.QueueRelease(ReleaseRequest("session_capacity_full_256")));
  CJumpgatePlaybackClaimCoordinator fallback{transport};
  EXPECT_TRUE(fallback.QueueRelease(ReleaseRequest("session_capacity_full_256")));

  transport->UnblockFirstClaim();
  EXPECT_TRUE(primary.Stop(true));
  EXPECT_TRUE(fallback.Stop(true));
  EXPECT_GE(transport->Releases(), 257);
}
