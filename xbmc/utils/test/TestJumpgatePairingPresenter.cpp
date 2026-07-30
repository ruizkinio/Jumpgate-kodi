/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePairingPresenter.h"

#include <gtest/gtest.h>

namespace KODI::JUMPGATE
{

TEST(TestJumpgatePairingPresenter, FormatsCountdown)
{
  EXPECT_EQ(CJumpgatePairingPresenter::FormatCountdown(600), "10:00");
  EXPECT_EQ(CJumpgatePairingPresenter::FormatCountdown(61), "1:01");
  EXPECT_EQ(CJumpgatePairingPresenter::FormatCountdown(-4), "0:00");
}

TEST(TestJumpgatePairingPresenter, ShowsQrAndCountdownOnlyWhileAwaitingActivation)
{
  JumpgatePairingSnapshot snapshot;
  snapshot.stage = JumpgatePairingStage::AwaitingActivation;
  snapshot.userCode = "ABCD-EFGH";
  snapshot.verificationUrl = "https://bridge.example.test/configure";
  snapshot.qrImagePath = "special://temp/test.png";
  snapshot.remainingSeconds = 73;
  snapshot.remainingPercent = 12;
  snapshot.canCancel = true;

  const auto view = CJumpgatePairingPresenter::Build(snapshot);
  EXPECT_TRUE(view.showQr);
  EXPECT_TRUE(view.showCancel);
  EXPECT_FALSE(view.showRetry);
  EXPECT_EQ(view.countdown, "Expires in 1:13");
  EXPECT_EQ(view.remainingPercent, 12);
}

TEST(TestJumpgatePairingPresenter, ClosesOnlyAfterSecureCommit)
{
  JumpgatePairingSnapshot snapshot;
  snapshot.stage = JumpgatePairingStage::Applying;
  EXPECT_FALSE(CJumpgatePairingPresenter::Build(snapshot).shouldClose);

  snapshot.stage = JumpgatePairingStage::Applied;
  EXPECT_TRUE(CJumpgatePairingPresenter::Build(snapshot).shouldClose);

  snapshot.stage = JumpgatePairingStage::Failed;
  snapshot.canRetry = true;
  const auto failed = CJumpgatePairingPresenter::Build(snapshot);
  EXPECT_FALSE(failed.shouldClose);
  EXPECT_TRUE(failed.showRetry);
}

} // namespace KODI::JUMPGATE
