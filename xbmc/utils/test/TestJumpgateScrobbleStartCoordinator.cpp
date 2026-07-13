/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateScrobbleStartCoordinator.h"

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
JumpgateScrobbleAuthority Authority(uint64_t generation)
{
  return {generation, generation + 1, generation + 2};
}
} // namespace

TEST(TestJumpgateScrobbleStartCoordinator, AllowsExactlyOneOverlappingStart)
{
  CJumpgateScrobbleStartCoordinator coordinator;
  const auto first = coordinator.Reserve(Authority(1));
  ASSERT_TRUE(first);
  EXPECT_FALSE(coordinator.Reserve(Authority(1)));
  EXPECT_EQ(coordinator.Complete(*first, Authority(1), true, true),
            JumpgateScrobbleStartCompletion::Commit);
  EXPECT_TRUE(coordinator.Reserve(Authority(1)));
}

TEST(TestJumpgateScrobbleStartCoordinator, StaleSuccessBlocksWinnerUntilCompensated)
{
  CJumpgateScrobbleStartCoordinator coordinator;
  const auto stale = coordinator.Reserve(Authority(1));
  ASSERT_TRUE(stale);
  coordinator.Invalidate();
  EXPECT_FALSE(coordinator.Reserve(Authority(2)));
  EXPECT_EQ(coordinator.Complete(*stale, Authority(2), true, true),
            JumpgateScrobbleStartCompletion::Compensate);
  EXPECT_FALSE(coordinator.Reserve(Authority(2)));
  EXPECT_TRUE(coordinator.FinishCompensation(*stale));
  EXPECT_TRUE(coordinator.Reserve(Authority(2)));
}

TEST(TestJumpgateScrobbleStartCoordinator, CleanupBarrierBlocksStartsUntilCompletion)
{
  CJumpgateScrobbleStartCoordinator coordinator;
  const uint64_t cleanup = coordinator.BeginCleanup();
  EXPECT_FALSE(coordinator.Reserve(Authority(1)));
  EXPECT_TRUE(coordinator.FinishCleanup(cleanup));
  EXPECT_TRUE(coordinator.Reserve(Authority(1)));
}
