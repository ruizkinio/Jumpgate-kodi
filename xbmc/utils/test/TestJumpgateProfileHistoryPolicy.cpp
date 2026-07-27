/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateProfileHistoryPolicy.h"

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

TEST(TestJumpgateProfileHistoryPolicy, ForgetCommitControlsRecoveryWithoutAmbiguity)
{
  EXPECT_EQ(
      GetJumpgateForgetHistoryAction(true, ForgetLocalResult{ForgetLocalStatus::NotCommitted}),
      JumpgateForgetHistoryAction::UnblockPreserving);
  EXPECT_EQ(GetJumpgateForgetHistoryAction(true, ForgetLocalResult{ForgetLocalStatus::Committed}),
            JumpgateForgetHistoryAction::PurgeKeepingBlocked);
  EXPECT_EQ(GetJumpgateForgetHistoryAction(
                true, ForgetLocalResult{ForgetLocalStatus::CommittedRefreshFailed}),
            JumpgateForgetHistoryAction::PurgeKeepingBlocked);
  EXPECT_EQ(GetJumpgateForgetHistoryAction(
                false, ForgetLocalResult{ForgetLocalStatus::CommittedRefreshFailed}),
            JumpgateForgetHistoryAction::None);
}

TEST(TestJumpgateProfileHistoryPolicy, RepairPreservesKnownProfileButNotForgottenRecords)
{
  EXPECT_EQ(GetJumpgatePairingHistoryAction(true, false, true),
            JumpgatePairingHistoryAction::UnblockPreserving);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(true, false, false),
            JumpgatePairingHistoryAction::PurgeThenUnblock);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(true, true, true),
            JumpgatePairingHistoryAction::PurgeThenUnblock);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(false, true, true),
            JumpgatePairingHistoryAction::PurgeThenUnblock);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(false, false, true),
            JumpgatePairingHistoryAction::None);
}
