/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePairingPresenter.h"

#include <algorithm>

namespace KODI::JUMPGATE
{

JumpgatePairingDialogView CJumpgatePairingPresenter::Build(const JumpgatePairingSnapshot& snapshot)
{
  JumpgatePairingDialogView view;
  view.heading = "Pair Jumpgate";
  view.status = snapshot.status;
  view.verificationUrl = snapshot.verificationUrl;
  view.userCode = snapshot.userCode;
  view.qrImagePath = snapshot.qrImagePath;
  view.remainingPercent = std::clamp(snapshot.remainingPercent, 0, 100);
  view.showQr =
      snapshot.stage == JumpgatePairingStage::AwaitingActivation && !snapshot.qrImagePath.empty();
  view.showCancel = snapshot.canCancel;
  view.showRetry = snapshot.canRetry;
  view.shouldClose = snapshot.stage == JumpgatePairingStage::Applied;

  switch (snapshot.stage)
  {
    case JumpgatePairingStage::Issuing:
      view.instruction = "Preparing a private one-time link";
      break;
    case JumpgatePairingStage::AwaitingActivation:
      view.instruction = "Scan with your phone, or open this address and enter the code";
      view.countdown = "Expires in " + FormatCountdown(snapshot.remainingSeconds);
      break;
    case JumpgatePairingStage::Applying:
      view.instruction = "Browser confirmed. Finishing securely on this device.";
      break;
    case JumpgatePairingStage::Expired:
      view.instruction = "This one-time code is no longer valid.";
      break;
    case JumpgatePairingStage::Failed:
      view.instruction = "Nothing was applied. You can safely request another code.";
      break;
    case JumpgatePairingStage::Cancelled:
      view.instruction = "No profile was changed.";
      break;
    case JumpgatePairingStage::Applied:
      view.instruction = "Profile secured and ready for external playback.";
      break;
    case JumpgatePairingStage::Idle:
      break;
  }
  return view;
}

std::string CJumpgatePairingPresenter::FormatCountdown(int remainingSeconds)
{
  const int bounded = std::max(0, remainingSeconds);
  const int seconds = bounded % 60;
  return std::to_string(bounded / 60) + ":" + (seconds < 10 ? "0" : "") + std::to_string(seconds);
}

} // namespace KODI::JUMPGATE
