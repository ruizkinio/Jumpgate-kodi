/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateShutdownCoordinator.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

TEST(TestJumpgateShutdownCoordinator, DrainsExactlyOnceAndLaterCallsAreIdempotent)
{
  CJumpgateShutdownCoordinator coordinator;
  int calls = 0;

  EXPECT_TRUE(coordinator.RunOnceAndWait([&calls] { ++calls; }));
  EXPECT_FALSE(coordinator.RunOnceAndWait([&calls] { ++calls; }));
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(coordinator.IsDrained());
}

TEST(TestJumpgateShutdownCoordinator, ConcurrentCallerWaitsForDrainCompletion)
{
  CJumpgateShutdownCoordinator coordinator;
  std::promise<void> drainEntered;
  std::promise<void> releaseDrain;
  std::atomic<bool> waiterReturned{false};

  std::thread drainer(
      [&]
      {
        EXPECT_TRUE(coordinator.RunOnceAndWait(
            [&]
            {
              drainEntered.set_value();
              releaseDrain.get_future().wait();
            }));
      });
  drainEntered.get_future().wait();

  std::thread waiter(
      [&]
      {
        EXPECT_FALSE(coordinator.RunOnceAndWait([] {}));
        waiterReturned = true;
      });
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(waiterReturned.load());
  releaseDrain.set_value();
  drainer.join();
  waiter.join();
  EXPECT_TRUE(waiterReturned.load());
}

TEST(TestJumpgateShutdownCoordinator, DrainCompletesBeforeServiceTeardownContinues)
{
  CJumpgateShutdownCoordinator coordinator;
  std::atomic<int> sequence{0};
  int drainOrder = 0;
  int serviceTeardownOrder = 0;

  coordinator.RunOnceAndWait([&] { drainOrder = ++sequence; });
  serviceTeardownOrder = ++sequence;

  EXPECT_EQ(drainOrder, 1);
  EXPECT_EQ(serviceTeardownOrder, 2);
}

TEST(TestJumpgateShutdownCoordinator, FailedDrainCanBeRetried)
{
  CJumpgateShutdownCoordinator coordinator;
  EXPECT_THROW(
      coordinator.RunOnceAndWait([] { throw std::runtime_error("simulated drain failure"); }),
      std::runtime_error);
  EXPECT_FALSE(coordinator.IsDrained());
  EXPECT_TRUE(coordinator.RunOnceAndWait([] {}));
  EXPECT_TRUE(coordinator.IsDrained());
}
