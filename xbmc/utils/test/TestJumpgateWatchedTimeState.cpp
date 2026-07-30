/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateWatchedTimeState.h"

#include <limits>

#include <gtest/gtest.h>

using KODI::JUMPGATE::CJumpgateWatchedTimeState;

TEST(TestJumpgateWatchedTimeState, CreditsOnlyBoundedPlaybackClockAdvance)
{
  CJumpgateWatchedTimeState state;
  state.SetPlaying(true);

  EXPECT_EQ(state.Observe(1000, 0, 1.0), 0);
  EXPECT_EQ(state.Observe(2000, 1000, 1.0), 1000);
  EXPECT_EQ(state.Observe(3000, 1000, 1.0), 1000);
  EXPECT_EQ(state.Observe(4000, 2000, 1.0), 2000);

  // A seek cannot turn a short observation interval into hours of watched time.
  EXPECT_EQ(state.Observe(4100, 3600000, 1.0), 2350);
  EXPECT_EQ(state.Observe(4200, 1000, 1.0), 2350);
}

TEST(TestJumpgateWatchedTimeState, PauseAndBackgroundNeverBridgeInactiveTime)
{
  CJumpgateWatchedTimeState state;
  state.SetPlaying(true);
  state.Observe(1000, 0, 1.0);
  EXPECT_EQ(state.Observe(2000, 1000, 1.0), 1000);

  state.SetPaused(true);
  state.Observe(10000, 1000, 1.0);
  EXPECT_EQ(state.Observe(11000, 2000, 1.0), 1000);
  state.SetPaused(false);
  state.Observe(12000, 2000, 1.0);
  EXPECT_EQ(state.Observe(13000, 3000, 1.0), 2000);

  state.SetBackgrounded(true);
  state.Observe(20000, 3000, 1.0);
  EXPECT_EQ(state.Observe(21000, 4000, 1.0), 2000);
  state.SetBackgrounded(false);
  state.Observe(22000, 4000, 1.0);
  EXPECT_EQ(state.Observe(23000, 5000, 1.0), 3000);
}

TEST(TestJumpgateWatchedTimeState, PlaybackSpeedBoundsContentTimeAndRejectsInvalidClocks)
{
  CJumpgateWatchedTimeState state;
  state.SetPlaying(true);
  state.Observe(1000, 0, 2.0);
  EXPECT_EQ(state.Observe(2000, 2000, 2.0), 2000);

  EXPECT_EQ(state.Observe(3000, 3000, 0.0), 2000);
  EXPECT_EQ(state.Observe(4000, 4000, 8.0), 2000);
  EXPECT_EQ(state.Observe(5000, 5000, std::numeric_limits<double>::infinity()), 2000);

  state.Reset();
  EXPECT_EQ(state.WatchedMs(), 0);
  state.SetPlaying(true);
  EXPECT_EQ(state.Observe(6000, 5000, 1.0), 0);
}

TEST(TestJumpgateWatchedTimeState, SaturatesExtremePlaybackClockAdvance)
{
  CJumpgateWatchedTimeState state;
  state.SetPlaying(true);
  state.Observe(1, 0, 4.0);
  EXPECT_EQ(state.Observe(std::numeric_limits<std::int64_t>::max(),
                          std::numeric_limits<std::int64_t>::max(), 4.0),
            std::numeric_limits<std::int64_t>::max());
}
