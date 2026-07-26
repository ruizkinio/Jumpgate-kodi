/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePairingCoordinator.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace KODI::JUMPGATE
{
namespace
{
constexpr const char* ORIGIN = "https://bridge.example.test";

class CManualPairingClock final : public IJumpgatePairingClock
{
public:
  TimePoint Now() const override
  {
    std::unique_lock lock(m_mutex);
    if (m_blockNextNow)
    {
      m_blockNextNow = false;
      m_nowBlocked = true;
      m_condition.notify_all();
      m_condition.wait(lock, [this] { return m_releaseNow; });
      m_nowBlocked = false;
      m_releaseNow = false;
    }
    return m_now;
  }

  bool WaitFor(std::chrono::milliseconds duration,
               const std::function<bool()>& interrupted) override
  {
    std::unique_lock lock(m_mutex);
    const TimePoint target = m_now + duration;
    m_condition.wait(lock, [&] { return m_now >= target || interrupted(); });
    return interrupted();
  }

  void Wake() override
  {
    {
      std::lock_guard lock(m_mutex);
      m_blockNextNow = false;
      m_releaseNow = true;
    }
    m_condition.notify_all();
  }

  void Advance(std::chrono::milliseconds duration)
  {
    {
      std::lock_guard lock(m_mutex);
      m_now += duration;
    }
    m_condition.notify_all();
  }

  void BlockNextNow()
  {
    std::lock_guard lock(m_mutex);
    m_blockNextNow = true;
    m_releaseNow = false;
  }

  bool WaitForBlockedNow(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_nowBlocked; });
  }

  void ReleaseBlockedNow()
  {
    {
      std::lock_guard lock(m_mutex);
      m_releaseNow = true;
    }
    m_condition.notify_all();
  }

private:
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_condition;
  mutable bool m_blockNextNow{false};
  mutable bool m_nowBlocked{false};
  mutable bool m_releaseNow{false};
  TimePoint m_now{};
};

class CScriptedPairingTransport final : public IJumpgatePairingTransport
{
public:
  JumpgatePairingHttpResponse Post(const std::string& url,
                                   const std::string& body,
                                   std::chrono::steady_clock::time_point deadline,
                                   const std::function<bool()>& cancelled) override
  {
    std::unique_lock lock(m_mutex);
    const std::size_t attempt = ++m_attempts;
    m_condition.notify_all();
    if (attempt == m_blockBeforePublishCall)
      m_condition.wait(lock, [this] { return m_releaseBlockedCall; });
    if (cancelled())
      return {};

    m_urls.push_back(url);
    m_bodies.push_back(body);
    m_deadlines.push_back(deadline);
    const std::size_t call = m_urls.size();
    const int cancelGeneration = m_cancelGeneration;
    m_condition.notify_all();
    if (call == m_blockCall)
      m_condition.wait(lock, [this, cancelGeneration]
                       { return m_releaseBlockedCall || m_cancelGeneration != cancelGeneration; });
    if (m_responses.empty())
      return {};
    JumpgatePairingHttpResponse response = std::move(m_responses.front());
    m_responses.pop_front();
    return response;
  }

  void Cancel() override
  {
    {
      std::lock_guard lock(m_mutex);
      ++m_cancelGeneration;
      ++m_cancelCalls;
    }
    m_condition.notify_all();
  }

  void Push(JumpgatePairingHttpResponse response)
  {
    std::lock_guard lock(m_mutex);
    m_responses.push_back(std::move(response));
  }

  void BlockCall(std::size_t call)
  {
    std::lock_guard lock(m_mutex);
    m_blockCall = call;
    m_releaseBlockedCall = false;
  }

  void BlockBeforePublish(std::size_t call)
  {
    std::lock_guard lock(m_mutex);
    m_blockBeforePublishCall = call;
    m_releaseBlockedCall = false;
  }

  bool WaitForAttempts(std::size_t count, std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [&] { return m_attempts >= count; });
  }

  void ReleaseBlockedCall()
  {
    {
      std::lock_guard lock(m_mutex);
      m_releaseBlockedCall = true;
    }
    m_condition.notify_all();
  }

  bool WaitForCalls(std::size_t count, std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [&] { return m_urls.size() >= count; });
  }

  std::vector<std::string> Urls() const
  {
    std::lock_guard lock(m_mutex);
    return m_urls;
  }

  std::vector<std::chrono::steady_clock::time_point> Deadlines() const
  {
    std::lock_guard lock(m_mutex);
    return m_deadlines;
  }

  int CancelCalls() const
  {
    std::lock_guard lock(m_mutex);
    return m_cancelCalls;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::deque<JumpgatePairingHttpResponse> m_responses;
  std::vector<std::string> m_urls;
  std::vector<std::string> m_bodies;
  std::vector<std::chrono::steady_clock::time_point> m_deadlines;
  std::size_t m_attempts{0};
  std::size_t m_blockBeforePublishCall{0};
  std::size_t m_blockCall{0};
  bool m_releaseBlockedCall{false};
  int m_cancelGeneration{0};
  int m_cancelCalls{0};
};

JumpgatePairingHttpResponse JsonResponse(std::string body,
                                         int statusCode = 200,
                                         bool completed = true,
                                         int retryAfterSeconds = 0)
{
  return {completed, statusCode, retryAfterSeconds, std::move(body)};
}

std::string IssueResponse(int expiresIn = 600,
                          int interval = 2,
                          const std::string& userCode = "ABCD-EFGH",
                          char deviceCodeFill = 'D')
{
  return std::string{R"({"ok":true,"user_code":")"} + userCode + R"(","device_code":")" +
         std::string(43, deviceCodeFill) +
         R"(","verification_url":"https://bridge.example.test/configure","verification_short_url":"https://bridge.example.test/p/)" +
         userCode + R"(","expires_in":)" + std::to_string(expiresIn) +
         ",\"interval\":" + std::to_string(interval) + "}";
}

bool WaitForStage(CJumpgatePairingCoordinator& coordinator,
                  JumpgatePairingStage stage,
                  std::chrono::milliseconds timeout = 2s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (coordinator.GetSnapshot().stage == stage)
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

struct CoordinatorHarness
{
  CoordinatorHarness()
    : transport(std::make_shared<CScriptedPairingTransport>()),
      clock(std::make_shared<CManualPairingClock>()),
      coordinator(transport, clock)
  {
  }

  ~CoordinatorHarness()
  {
    ReleaseQrRender();
    clock->ReleaseBlockedNow();
  }

  bool Start()
  {
    JumpgatePairingRequest request;
    request.bridgeOrigin = ORIGIN;
    return coordinator.Start(
        std::move(request),
        [this](std::string json, const std::string& origin, const std::string& name)
        {
          {
            std::lock_guard lock(callbackMutex);
            redemption = std::move(json);
            redemptionOrigin = origin;
            profileName = name;
          }
          callbackCondition.notify_all();
        },
        [this](const std::string& verificationUrl)
        {
          std::unique_lock lock(callbackMutex);
          renderedUrls.push_back(verificationUrl);
          const std::string path =
              "special://temp/jumpgate-test-qr-" + std::to_string(++qrRenderCount) + ".png";
          qrRenderStarted = true;
          callbackCondition.notify_all();
          if (blockQrRender)
            callbackCondition.wait(lock, [this] { return releaseQrRender; });
          return path;
        },
        [this](const std::string& path)
        {
          {
            std::lock_guard lock(callbackMutex);
            cleanedQr = path;
            cleanedQrs.push_back(path);
          }
          callbackCondition.notify_all();
        });
  }

  bool WaitForRedemption(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(callbackMutex);
    return callbackCondition.wait_for(lock, timeout, [this] { return !redemption.empty(); });
  }

  bool WaitForQrCleanup(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(callbackMutex);
    return callbackCondition.wait_for(lock, timeout, [this] { return !cleanedQr.empty(); });
  }

  bool WaitForQrCleanupCount(std::size_t count, std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(callbackMutex);
    return callbackCondition.wait_for(lock, timeout,
                                      [this, count] { return cleanedQrs.size() >= count; });
  }

  void BlockQrRender()
  {
    std::lock_guard lock(callbackMutex);
    blockQrRender = true;
    releaseQrRender = false;
    qrRenderStarted = false;
  }

  bool WaitForQrRender(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock lock(callbackMutex);
    return callbackCondition.wait_for(lock, timeout, [this] { return qrRenderStarted; });
  }

  void ReleaseQrRender()
  {
    {
      std::lock_guard lock(callbackMutex);
      releaseQrRender = true;
    }
    callbackCondition.notify_all();
  }

  bool HasRedemption() const
  {
    std::lock_guard lock(callbackMutex);
    return !redemption.empty();
  }

  std::string RedemptionOrigin() const
  {
    std::lock_guard lock(callbackMutex);
    return redemptionOrigin;
  }

  std::string ProfileName() const
  {
    std::lock_guard lock(callbackMutex);
    return profileName;
  }

  std::string CleanedQr() const
  {
    std::lock_guard lock(callbackMutex);
    return cleanedQr;
  }

  std::vector<std::string> CleanedQrs() const
  {
    std::lock_guard lock(callbackMutex);
    return cleanedQrs;
  }

  std::vector<std::string> RenderedUrls() const
  {
    std::lock_guard lock(callbackMutex);
    return renderedUrls;
  }

  std::shared_ptr<CScriptedPairingTransport> transport;
  std::shared_ptr<CManualPairingClock> clock;
  mutable std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::string redemption;
  std::string redemptionOrigin;
  std::string profileName;
  std::string cleanedQr;
  std::vector<std::string> cleanedQrs;
  std::vector<std::string> renderedUrls;
  std::size_t qrRenderCount{0};
  bool blockQrRender{false};
  bool qrRenderStarted{false};
  bool releaseQrRender{false};
  CJumpgatePairingCoordinator coordinator;
};
} // namespace

TEST(TestJumpgatePairingCoordinator, PollsInOrderAndWaitsForSecureCommitBeforeApplied)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->Push(JsonResponse(R"({"ok":true,"paired":false})"));
  harness.transport->Push(
      JsonResponse(std::string{R"({"ok":true,"paired":true,"name":"Living Room","profileId":")"} +
                   std::string(24, 'P') + "\",\"deviceToken\":\"" + std::string(43, 'T') + "\"}"));

  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForCalls(1));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  EXPECT_EQ(harness.coordinator.GetSnapshot().remainingSeconds, 600);

  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));
  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(3));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::Applying));
  ASSERT_TRUE(harness.WaitForRedemption());
  EXPECT_TRUE(harness.HasRedemption());
  EXPECT_EQ(harness.RedemptionOrigin(), ORIGIN);
  EXPECT_EQ(harness.ProfileName(), "Living Room");

  harness.coordinator.CompleteApply(true);
  EXPECT_TRUE(harness.CleanedQr().empty());
  harness.coordinator.ReleaseQrArtifact("special://temp/not-the-active-qr.png");
  EXPECT_TRUE(harness.CleanedQr().empty());
  harness.coordinator.ReleaseQrArtifact("special://temp/jumpgate-test-qr-1.png");
  ASSERT_TRUE(harness.WaitForQrCleanup());
  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Applied);
  EXPECT_EQ(harness.CleanedQr(), "special://temp/jumpgate-test-qr-1.png");
}

TEST(TestJumpgatePairingCoordinator, CountdownUsesIssueResponseDeadlineAndExpiresExactly)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse(3, 2)));
  harness.transport->Push(JsonResponse(R"({"ok":true,"paired":false})"));
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForCalls(1));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));

  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));
  ASSERT_EQ(harness.transport->Deadlines().size(), 2U);
  EXPECT_EQ(harness.transport->Deadlines()[1], IJumpgatePairingClock::TimePoint{} + 3s);
  EXPECT_EQ(harness.coordinator.GetSnapshot().remainingSeconds, 1);
  harness.clock->Advance(1s);
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::Expired));
  EXPECT_TRUE(harness.CleanedQr().empty());
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanup());
  EXPECT_EQ(harness.transport->Urls().size(), 2U);
  EXPECT_TRUE(harness.coordinator.GetSnapshot().userCode.empty());
}

TEST(TestJumpgatePairingCoordinator, CancelBeforeTransportPublicationIsSticky)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->BlockBeforePublish(1);
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForAttempts(1));

  harness.coordinator.Cancel();
  harness.transport->ReleaseBlockedCall();
  harness.coordinator.Stop(true);

  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Cancelled);
  EXPECT_GT(harness.transport->CancelCalls(), 0);
  EXPECT_TRUE(harness.transport->Urls().empty());
  EXPECT_FALSE(harness.HasRedemption());
}

TEST(TestJumpgatePairingCoordinator, CancelDuringQrRenderingCannotRepublishPairing)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.BlockQrRender();
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForCalls(1));
  ASSERT_TRUE(harness.WaitForQrRender());

  harness.coordinator.Cancel();
  harness.ReleaseQrRender();
  harness.coordinator.Stop(true);

  const JumpgatePairingSnapshot snapshot = harness.coordinator.GetSnapshot();
  EXPECT_EQ(snapshot.stage, JumpgatePairingStage::Cancelled);
  EXPECT_TRUE(snapshot.userCode.empty());
  EXPECT_TRUE(snapshot.verificationUrl.empty());
  EXPECT_TRUE(snapshot.qrImagePath.empty());
  ASSERT_TRUE(harness.WaitForQrCleanup());
  EXPECT_EQ(harness.CleanedQr(), "special://temp/jumpgate-test-qr-1.png");
  EXPECT_FALSE(harness.HasRedemption());
}

TEST(TestJumpgatePairingCoordinator, CancelInterruptsAnInflightTransport)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->Push({});
  harness.transport->BlockCall(2);
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForCalls(1));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));

  harness.coordinator.Cancel();
  harness.coordinator.Stop(true);
  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Cancelled);
  EXPECT_GT(harness.transport->CancelCalls(), 0);
  EXPECT_TRUE(harness.CleanedQr().empty());
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanup());
  EXPECT_EQ(harness.CleanedQr(), "special://temp/jumpgate-test-qr-1.png");
}

TEST(TestJumpgatePairingCoordinator, CancelAfterPollStopCheckRejectsLateTerminalState)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->Push(JsonResponse(R"({"ok":false,"error":"Rejected"})", 400));
  harness.transport->BlockCall(2);
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));

  harness.clock->BlockNextNow();
  harness.transport->ReleaseBlockedCall();
  ASSERT_TRUE(harness.clock->WaitForBlockedNow());
  harness.coordinator.Cancel();
  harness.clock->ReleaseBlockedNow();
  harness.coordinator.Stop(true);

  const JumpgatePairingSnapshot snapshot = harness.coordinator.GetSnapshot();
  EXPECT_EQ(snapshot.stage, JumpgatePairingStage::Cancelled);
  EXPECT_EQ(snapshot.status, "Pairing cancelled");
  EXPECT_TRUE(snapshot.userCode.empty());
  EXPECT_FALSE(harness.HasRedemption());
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanup());
}

TEST(TestJumpgatePairingCoordinator, RejectsPairedResponseReturnedAtDeadline)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse(3, 2)));
  harness.transport->Push(JsonResponse(R"({"ok":true,"paired":true,"name":"Too Late"})"));
  harness.transport->BlockCall(2);
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));

  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));
  harness.clock->Advance(1s);
  harness.transport->ReleaseBlockedCall();

  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::Expired));
  EXPECT_TRUE(harness.CleanedQr().empty());
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanup());
  EXPECT_FALSE(harness.HasRedemption());
  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Expired);
}

TEST(TestJumpgatePairingCoordinator, UserCancelCannotOverrideSecureApplyBoundary)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->Push(JsonResponse(R"({"ok":true,"paired":true,"name":"Profile"})"));
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));

  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.WaitForRedemption());
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::Applying));

  harness.coordinator.Cancel();
  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Applying);

  harness.coordinator.Stop(true);
  EXPECT_EQ(harness.coordinator.GetSnapshot().stage, JumpgatePairingStage::Cancelled);
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanup());
}

TEST(TestJumpgatePairingCoordinator, RestartIssuesFreshCodeAfterTerminalCleanup)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse(600, 2, "FIRST-CODE")));
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  EXPECT_EQ(harness.coordinator.GetSnapshot().userCode, "FIRST-CODE");

  harness.coordinator.Cancel();

  harness.transport->Push(JsonResponse(IssueResponse(600, 2, "NEXT-CODE", 'E')));
  ASSERT_TRUE(harness.coordinator.Restart());
  ASSERT_TRUE(harness.transport->WaitForCalls(2));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  EXPECT_EQ(harness.coordinator.GetSnapshot().userCode, "NEXT-CODE");
  EXPECT_EQ(harness.coordinator.GetSnapshot().qrImagePath, "special://temp/jumpgate-test-qr-2.png");
  EXPECT_EQ(harness.transport->Urls()[0], std::string(ORIGIN) + "/pair/device/code");
  EXPECT_EQ(harness.transport->Urls()[1], std::string(ORIGIN) + "/pair/device/code");
  ASSERT_EQ(harness.RenderedUrls().size(), 2U);
  EXPECT_EQ(harness.RenderedUrls()[0], "https://bridge.example.test/p/FIRST-CODE");
  EXPECT_EQ(harness.RenderedUrls()[1], "https://bridge.example.test/p/NEXT-CODE");

  harness.coordinator.ReleaseQrArtifact("special://temp/jumpgate-test-qr-1.png");
  ASSERT_TRUE(harness.WaitForQrCleanupCount(1));
  EXPECT_EQ(harness.CleanedQrs()[0], "special://temp/jumpgate-test-qr-1.png");
  EXPECT_EQ(harness.coordinator.GetSnapshot().qrImagePath, "special://temp/jumpgate-test-qr-2.png");

  harness.coordinator.Cancel();
  harness.coordinator.ReleasePendingQrArtifacts();
  ASSERT_TRUE(harness.WaitForQrCleanupCount(2));
  EXPECT_EQ(harness.CleanedQrs()[1], "special://temp/jumpgate-test-qr-2.png");
}

TEST(TestJumpgatePairingCoordinator, RateLimitUsesRetryAfterWithoutAbandoningCode)
{
  CoordinatorHarness harness;
  harness.transport->Push(JsonResponse(IssueResponse()));
  harness.transport->Push(JsonResponse(R"({"ok":false,"retryAfterSec":5})", 429, false, 5));
  harness.transport->Push(JsonResponse(R"({"ok":true,"paired":true,"name":"Profile"})"));
  ASSERT_TRUE(harness.Start());
  ASSERT_TRUE(harness.transport->WaitForCalls(1));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::AwaitingActivation));
  harness.clock->Advance(2s);
  ASSERT_TRUE(harness.transport->WaitForCalls(2));
  harness.clock->Advance(5s);
  ASSERT_TRUE(harness.transport->WaitForCalls(3));
  ASSERT_TRUE(WaitForStage(harness.coordinator, JumpgatePairingStage::Applying));
  EXPECT_TRUE(harness.WaitForRedemption());
}

} // namespace KODI::JUMPGATE
