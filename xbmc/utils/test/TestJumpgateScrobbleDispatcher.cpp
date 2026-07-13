/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateScrobbleDispatcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

namespace
{
class BlockingStopTransport final : public IJumpgateScrobbleStopTransport
{
public:
  bool SendStop(const std::string& jsonBody, const std::string&) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_calls;
    m_json.emplace_back(jsonBody);
    m_condition.notify_all();
    if (m_block)
      m_condition.wait(lock, [this] { return m_released; });
    return true;
  }

  void Block()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_block = true;
  }

  void Release()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_condition.notify_all();
  }

  bool WaitForCalls(int calls)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, 2s, [this, calls] { return m_calls >= calls; });
  }

  int Calls() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_calls;
  }

  std::vector<std::string> Json() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_json;
  }

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  int m_calls{0};
  bool m_block{false};
  bool m_released{false};
  std::vector<std::string> m_json;
};
} // namespace

TEST(TestJumpgateScrobbleDispatcher, StopWithoutDrainReturnsAndRegistryJoinsLater)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->Block();
  CJumpgateScrobbleDispatcher dispatcher{transport, registry};
  ASSERT_TRUE(dispatcher.QueueStop("one", "{\"progress\":10}", "token"));
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(dispatcher.Stop(false, 500ms));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
  EXPECT_EQ(registry->Pending(), 1u);
  transport->Release();
  EXPECT_TRUE(registry->JoinAllFor(2s));
}

TEST(TestJumpgateScrobbleDispatcher, CoalescesIdentityAndCompletesEverySubscriber)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  transport->Block();
  CJumpgateScrobbleDispatcher dispatcher{transport};
  std::atomic<int> completions{0};
  ASSERT_TRUE(
      dispatcher.QueueStop("same", "first", "token", [&completions](bool) { ++completions; }));
  ASSERT_TRUE(transport->WaitForCalls(1));
  ASSERT_TRUE(
      dispatcher.QueueStop("same", "duplicate", "token", [&completions](bool) { ++completions; }));
  transport->Release();
  EXPECT_TRUE(dispatcher.Stop(true));
  EXPECT_EQ(transport->Calls(), 1);
  EXPECT_EQ(completions.load(), 2);
  ASSERT_EQ(transport->Json().size(), 1u);
  EXPECT_EQ(transport->Json().front(), "first");
}

TEST(TestJumpgateScrobbleDispatcher, CapacityOverflowRetainsEveryDistinctCleanup)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  transport->Block();
  CJumpgateScrobbleDispatcher dispatcher{transport};
  ASSERT_TRUE(dispatcher.QueueStop("cleanup-0", "json-0", "token"));
  ASSERT_TRUE(transport->WaitForCalls(1));
  for (int index = 1; index < 8; ++index)
  {
    ASSERT_TRUE(dispatcher.QueueStop("cleanup-" + std::to_string(index),
                                     "json-" + std::to_string(index), "token"));
  }
  EXPECT_TRUE(dispatcher.QueueStop("cleanup-8", "json-8", "token"));

  transport->Release();
  EXPECT_TRUE(dispatcher.Stop(true));
  EXPECT_EQ(transport->Calls(), 9);
  const auto json = transport->Json();
  EXPECT_EQ(json.front(), "json-0");
  EXPECT_EQ(json.back(), "json-8");
}

TEST(TestJumpgateScrobbleDispatcher, CapacityRejectionRetriesOnFreshOwnedDispatcher)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->Block();
  CJumpgateScrobbleDispatcher primary{transport, registry};
  ASSERT_TRUE(primary.QueueStop("cleanup-0", "json-0", "token"));
  ASSERT_TRUE(transport->WaitForCalls(1));
  for (int index = 1; index < 64; ++index)
  {
    ASSERT_TRUE(primary.QueueStop("cleanup-" + std::to_string(index),
                                  "json-" + std::to_string(index), "token"));
  }

  EXPECT_FALSE(primary.QueueStop("cleanup-64", "json-64", "token"));
  CJumpgateScrobbleDispatcher fallback{transport, registry};
  EXPECT_TRUE(fallback.QueueStop("cleanup-64", "json-64", "token"));

  transport->Release();
  EXPECT_TRUE(primary.Stop(true));
  EXPECT_TRUE(fallback.Stop(true));
  EXPECT_EQ(transport->Calls(), 65);
}

TEST(TestJumpgateScrobbleDispatcher, RejectsEmptyAndPostStopJobs)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  CJumpgateScrobbleDispatcher dispatcher{transport};
  EXPECT_FALSE(dispatcher.QueueStop("", "json", "token"));
  EXPECT_FALSE(dispatcher.QueueStop("id", "", "token"));
  EXPECT_FALSE(dispatcher.QueueStop("id", "json", ""));
  EXPECT_TRUE(dispatcher.Stop(true));
  EXPECT_FALSE(dispatcher.QueueStop("id", "json", "token"));
}

TEST(TestJumpgateScrobbleDispatcher, DrainReturnsAtDeadlineAndRegistryEventuallyJoins)
{
  auto transport = std::make_shared<BlockingStopTransport>();
  auto registry = std::make_shared<CJumpgateThreadRegistry>();
  transport->Block();
  CJumpgateScrobbleDispatcher dispatcher{transport, registry};
  for (int index = 0; index < 8; ++index)
  {
    ASSERT_TRUE(dispatcher.QueueStop("cleanup-" + std::to_string(index),
                                     "json-" + std::to_string(index), "token"));
  }
  ASSERT_TRUE(transport->WaitForCalls(1));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(dispatcher.Stop(true, 50ms));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, 50ms);
  EXPECT_LT(elapsed, 250ms);
  EXPECT_EQ(registry->Pending(), 1u);
  EXPECT_EQ(transport->Calls(), 1);

  transport->Release();
  EXPECT_TRUE(registry->JoinAllFor(2s));
  EXPECT_EQ(transport->Calls(), 8);
}
