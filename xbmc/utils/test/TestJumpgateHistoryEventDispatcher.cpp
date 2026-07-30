/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgateHistoryEventDispatcher.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
constexpr const char* ORIGIN = "https://bridge.example";
const std::string DEVICE_TOKEN(43, 'T');

struct CapturedRequest final
{
  std::string url;
  std::string body;
  std::vector<JumpgatePlaybackHttpHeader> headers;
};

struct ScriptedReply final
{
  bool transportSuccess{true};
  int status{200};
  std::string body;
  bool block{false};
  bool throwException{false};
};

std::string HeaderValue(const CapturedRequest& request, const std::string& name)
{
  for (const auto& header : request.headers)
  {
    if (header.name == name)
      return header.value;
  }
  return {};
}

std::string SessionForGrant(const std::string& grant)
{
  return grant.empty() ? "session_00000001" : "session_0000000" + std::string(1, grant.back());
}

std::string Accepted(const std::string& event,
                     const std::string& sessionId,
                     const std::string& grantKind,
                     std::uint64_t revision,
                     const std::string& status = "applied")
{
  std::string state = "playing";
  if (event == "pause")
    state = "paused";
  else if (event == "background")
    state = "backgrounded";
  else if (event == "stop" || event == "completion")
    state = "released";
  return "{\"dispatchIntent\":null,\"event\":\"" + event + "\",\"grantKind\":\"" + grantKind +
         "\",\"history\":null,\"ok\":true,\"sessionId\":\"" + sessionId +
         "\",\"sessionRevision\":" + std::to_string(revision) + ",\"sessionState\":\"" + state +
         "\",\"status\":\"" + status + "\"}";
}

class ScriptedTransport final : public IJumpgatePlaybackClaimTransport
{
public:
  explicit ScriptedTransport(std::vector<ScriptedReply> replies = {})
    : m_replies(std::move(replies))
  {
  }

  bool Post(const JumpgatePlaybackHttpRequest& request,
            JumpgatePlaybackHttpResponse& response) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    const std::size_t index = m_requests.size();
    m_requests.push_back({request.url, request.body, request.headers});
    ScriptedReply reply;
    if (index < m_replies.size())
      reply = m_replies[index];
    else if (request.url == std::string(ORIGIN) + "/v1/playback/release")
      reply.body = R"({"status":"released"})";
    else
      reply.body = AutoHistoryResponse(m_requests.back());
    m_condition.notify_all();
    if (reply.block)
      m_condition.wait(lock, [this] { return m_released; });
    lock.unlock();

    if (reply.throwException)
      throw std::runtime_error("scripted transport failure");
    if (!reply.transportSuccess)
      return false;
    response.statusCode = reply.status;
    response.body = std::move(reply.body);
    return true;
  }

  bool WaitForCalls(std::size_t count, std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, timeout,
                                [this, count] { return m_requests.size() >= count; });
  }

  void ReleaseBlocked()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_condition.notify_all();
  }

  std::vector<CapturedRequest> Requests() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
  }

  std::size_t Calls() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests.size();
  }

private:
  static std::string AutoHistoryResponse(const CapturedRequest& request)
  {
    CVariant body;
    if (!CJSONVariantParser::Parse(request.body, body))
      return {};
    const std::string event = body["event"].asString();
    const std::uint64_t revision = body["sessionRevision"].asUnsignedInteger();
    const std::string grant = HeaderValue(request, "x-jumpgate-history-grant");
    const std::string kind = grant.size() > 4 && grant[4] == 'L' ? "local" : "canonical";
    return Accepted(event, SessionForGrant(grant), kind, revision + 1,
                    kind == "local" ? "local_only" : "applied");
  }

  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::vector<ScriptedReply> m_replies;
  std::vector<CapturedRequest> m_requests;
  bool m_released{false};
};

JumpgateHistoryEventBinding Binding(std::uint64_t generation, const std::string& kind = "canonical")
{
  JumpgateHistoryEventBinding binding;
  binding.generation = generation;
  binding.profileId = "profile_00000001";
  binding.deviceId = "device_00000001";
  binding.bridgeOrigin = ORIGIN;
  binding.deviceToken = DEVICE_TOKEN;
  binding.historyGrant =
      "hg1_" + std::string(31, kind == "local" ? 'L' : 'H') + std::to_string(generation);
  binding.historyGrantKind = kind;
  binding.sessionId = "session_0000000" + std::to_string(generation);
  binding.sessionRevision = 1;
  return binding;
}

JumpgateHistorySnapshot Snapshot(std::int64_t position = 10000,
                                 std::int64_t duration = 100000,
                                 std::int64_t watched = 9000)
{
  return {position, duration, watched, std::nullopt};
}

CVariant ParseBody(const CapturedRequest& request)
{
  CVariant body;
  EXPECT_TRUE(CJSONVariantParser::Parse(request.body, body));
  return body;
}
} // namespace

TEST(TestJumpgateHistoryEventDispatcher, StartPauseResumeUsesAcknowledgedRevisionAndSnapshots)
{
  auto transport = std::make_shared<ScriptedTransport>();
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  dispatcher.PlaybackStarted(false, Snapshot(12000, 100000, 10000), 1000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  dispatcher.PlaybackPaused(Snapshot(25000, 100000, 22000));
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  dispatcher.PlaybackStarted(true, Snapshot(30000, 100000, 24000), 2000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(ParseBody(requests[0])["event"].asString(), "start");
  EXPECT_EQ(ParseBody(requests[0])["sessionRevision"].asUnsignedInteger(), 1u);
  EXPECT_EQ(ParseBody(requests[1])["event"].asString(), "pause");
  EXPECT_EQ(ParseBody(requests[1])["sessionRevision"].asUnsignedInteger(), 2u);
  EXPECT_EQ(ParseBody(requests[1])["positionMs"].asInteger(), 25000);
  EXPECT_EQ(ParseBody(requests[2])["event"].asString(), "resume");
  EXPECT_EQ(ParseBody(requests[2])["sessionRevision"].asUnsignedInteger(), 3u);
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, ProgressIsSuppressedWhilePausedOrBackgrounded)
{
  auto transport = std::make_shared<ScriptedTransport>();
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  dispatcher.PlaybackStarted(false, Snapshot(), 1000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  dispatcher.ProcessSlow(Snapshot(20000), 10999, 10000);
  EXPECT_EQ(transport->Calls(), 1u);
  dispatcher.ProcessSlow(Snapshot(21000), 11000, 10000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  dispatcher.SetBackgrounded(true, Snapshot(22000));
  dispatcher.PlaybackPaused(Snapshot(22000));
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  dispatcher.ProcessSlow(Snapshot(30000), 30000, 10000);
  EXPECT_EQ(transport->Calls(), 3u);
  dispatcher.SetBackgrounded(false, Snapshot(30000));
  dispatcher.PlaybackStarted(true, Snapshot(30000), 30000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_EQ(ParseBody(requests[1])["event"].asString(), "progress");
  EXPECT_EQ(ParseBody(requests[2])["event"].asString(), "background");
  EXPECT_EQ(ParseBody(requests[3])["event"].asString(), "resume");
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, LocalGrantUsesTheSameLifecycleWithoutTraktGating)
{
  auto transport = std::make_shared<ScriptedTransport>();
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1, "local"), Snapshot(), 1000));
  dispatcher.PlaybackStarted(false, Snapshot(), 1000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  ASSERT_EQ(transport->Calls(), 1u);
  EXPECT_EQ(ParseBody(transport->Requests()[0])["event"].asString(), "start");
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, TerminalReturnsImmediatelyThenReleasesWithSameReceipt)
{
  const std::string stopResponse = Accepted("stop", "session_00000001", "canonical", 3, "applied");
  auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedReply>{
      {true, 200, Accepted("start", "session_00000001", "canonical", 2)},
      {true, 200, stopResponse, true},
      {true, 200, R"({"status":"released"})"},
  });
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  dispatcher.PlaybackStarted(false, Snapshot(), 1000);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto started = std::chrono::steady_clock::now();
  const auto terminal = dispatcher.FinishTerminal(1, false, Snapshot(50000, 100000, 45000));
  EXPECT_EQ(terminal.status, JumpgateHistoryTerminalStatus::Pending);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
  ASSERT_TRUE(transport->WaitForCalls(2));
  EXPECT_EQ(dispatcher.GetTerminalResult(1).status, JumpgateHistoryTerminalStatus::Pending);

  transport->ReleaseBlocked();
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  const auto finished = dispatcher.GetTerminalResult(1);
  ASSERT_EQ(finished.status, JumpgateHistoryTerminalStatus::Finished);
  ASSERT_TRUE(finished.accepted);
  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[2].url, std::string(ORIGIN) + "/v1/playback/release");
  CVariant release;
  ASSERT_TRUE(CJSONVariantParser::Parse(requests[2].body, release));
  EXPECT_EQ(release["sessionId"].asString(), "session_00000001");
  EXPECT_EQ(release["terminalReceiptId"].asString(), finished.terminalReceiptId);
  EXPECT_EQ(HeaderValue(requests[1], "Idempotency-Key"), finished.terminalReceiptId);
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, RetryReusesExactTerminalBytesAndUuid)
{
  auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedReply>{
      {false, 0, {}},
      {true, 503, R"({"error":"history_unavailable","ok":false})"},
      {true, 200, Accepted("stop", "session_00000001", "canonical", 2)},
      {true, 200, R"({"status":"released"})"},
  });
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  const auto terminal = dispatcher.FinishTerminal(1, false, Snapshot());
  ASSERT_EQ(terminal.status, JumpgateHistoryTerminalStatus::Pending);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_EQ(requests[0].body, requests[1].body);
  EXPECT_EQ(requests[1].body, requests[2].body);
  EXPECT_EQ(HeaderValue(requests[0], "Idempotency-Key"),
            HeaderValue(requests[1], "Idempotency-Key"));
  EXPECT_EQ(HeaderValue(requests[1], "Idempotency-Key"),
            HeaderValue(requests[2], "Idempotency-Key"));
  const auto finished = dispatcher.GetTerminalResult(1);
  EXPECT_EQ(finished.attempts, 3u);
  EXPECT_TRUE(finished.accepted);
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, FailedTerminalExposesNoReceipt)
{
  auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedReply>{
      {false, 0, {}},
      {false, 0, {}},
      {false, 0, {}},
      {false, 0, {}},
      {false, 0, {}},
  });
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));

  const auto pending = dispatcher.FinishTerminal(1, false, Snapshot());
  ASSERT_EQ(pending.status, JumpgateHistoryTerminalStatus::Pending);
  EXPECT_TRUE(pending.terminalReceiptId.empty());
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto failed = dispatcher.GetTerminalResult(1);
  EXPECT_EQ(failed.status, JumpgateHistoryTerminalStatus::Finished);
  EXPECT_FALSE(failed.accepted);
  EXPECT_EQ(failed.attempts, 5u);
  EXPECT_TRUE(failed.terminalReceiptId.empty());
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, ReleaseExceptionPreservesAcceptedTerminalReceipt)
{
  auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedReply>{
      {true, 200, Accepted("stop", "session_00000001", "canonical", 2)},
      {true, 200, {}, false, true},
      {true, 200, R"({"status":"released"})"},
  });
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  ASSERT_EQ(dispatcher.FinishTerminal(1, false, Snapshot()).status,
            JumpgateHistoryTerminalStatus::Pending);
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));

  const auto finished = dispatcher.GetTerminalResult(1);
  ASSERT_EQ(finished.status, JumpgateHistoryTerminalStatus::Finished);
  EXPECT_TRUE(finished.accepted);
  EXPECT_FALSE(finished.terminalReceiptId.empty());
  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[1].url, std::string(ORIGIN) + "/v1/playback/release");
  EXPECT_EQ(requests[2].url, std::string(ORIGIN) + "/v1/playback/release");
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}

TEST(TestJumpgateHistoryEventDispatcher, NewGenerationAdmissionDoesNotWaitForOldNetworkCleanup)
{
  auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedReply>{
      {true, 200, Accepted("stop", "session_00000001", "canonical", 2), true},
      {true, 200, R"({"status":"released"})"},
      {true, 200, Accepted("start", "session_00000002", "canonical", 2)},
  });
  CJumpgateHistoryEventDispatcher dispatcher{transport, [](auto) {}};
  ASSERT_TRUE(dispatcher.AdvanceGeneration(1));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(1), Snapshot(), 1000));
  ASSERT_EQ(dispatcher.FinishTerminal(1, false, Snapshot()).status,
            JumpgateHistoryTerminalStatus::Pending);
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(dispatcher.AdvanceGeneration(2));
  ASSERT_TRUE(dispatcher.BindClaim(Binding(2), Snapshot(), 2000));
  dispatcher.PlaybackStarted(false, Snapshot(), 2000);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);

  transport->ReleaseBlocked();
  ASSERT_TRUE(dispatcher.WaitForIdle(2s));
  const auto requests = transport->Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(ParseBody(requests[0])["event"].asString(), "stop");
  EXPECT_EQ(ParseBody(requests[2])["event"].asString(), "start");
  EXPECT_TRUE(dispatcher.Stop(true, 2s));
}
