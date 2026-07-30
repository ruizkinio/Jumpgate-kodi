/*
 *  Copyright (C) 2026 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackRetry.h"

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

TEST(TestJumpgatePlaybackRetry, RetriesThroughBoundaryThenExpiresWithoutNestedStateAccess)
{
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(false, false, false, 100, 125, 25),
            JumpgatePlaybackRetryAction::Retry);
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(false, false, false, 100, 126, 25),
            JumpgatePlaybackRetryAction::Expire);
}

TEST(TestJumpgatePlaybackRetry, InactiveAndClockRollbackStatesDoNothing)
{
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(true, false, false, 100, 200, 25),
            JumpgatePlaybackRetryAction::None);
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(false, true, false, 100, 200, 25),
            JumpgatePlaybackRetryAction::None);
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(false, false, true, 100, 200, 25),
            JumpgatePlaybackRetryAction::None);
  EXPECT_EQ(GetJumpgatePlaybackRetryAction(false, false, false, 100, 99, 25),
            JumpgatePlaybackRetryAction::None);
}
