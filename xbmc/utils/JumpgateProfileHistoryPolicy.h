/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgateProfileRuntime.h"

namespace KODI::JUMPGATE
{

enum class JumpgateForgetHistoryAction
{
  None,
  UnblockPreserving,
  PurgeKeepingBlocked,
};

enum class JumpgatePairingHistoryAction
{
  None,
  UnblockPreserving,
  PurgeThenUnblock,
};

JumpgateForgetHistoryAction GetJumpgateForgetHistoryAction(bool historyBlocked,
                                                           const ForgetLocalResult& result);
JumpgatePairingHistoryAction GetJumpgatePairingHistoryAction(bool historyBlocked,
                                                             bool profileForgotten,
                                                             bool profilePreviouslyKnown);

} // namespace KODI::JUMPGATE
