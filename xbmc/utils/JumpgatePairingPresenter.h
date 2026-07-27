/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgatePairingCoordinator.h"

#include <string>

namespace KODI::JUMPGATE
{

struct JumpgatePairingDialogView
{
  std::string heading;
  std::string instruction;
  std::string verificationUrl;
  std::string userCode;
  std::string countdown;
  std::string status;
  std::string qrImagePath;
  int remainingPercent{0};
  bool showQr{false};
  bool showCancel{false};
  bool showRetry{false};
  bool shouldClose{false};
};

class CJumpgatePairingPresenter final
{
public:
  static JumpgatePairingDialogView Build(const JumpgatePairingSnapshot& snapshot);
  static std::string FormatCountdown(int remainingSeconds);
};

} // namespace KODI::JUMPGATE
