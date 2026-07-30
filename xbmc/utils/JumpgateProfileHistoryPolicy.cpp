/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateProfileHistoryPolicy.h"

namespace KODI::JUMPGATE
{

JumpgateForgetHistoryAction GetJumpgateForgetHistoryAction(bool historyBlocked,
                                                           const ForgetLocalResult& result)
{
  if (!historyBlocked)
    return JumpgateForgetHistoryAction::None;
  return result.IsCommitted() ? JumpgateForgetHistoryAction::PurgeKeepingBlocked
                              : JumpgateForgetHistoryAction::UnblockPreserving;
}

JumpgatePairingHistoryAction GetJumpgatePairingHistoryAction(bool historyBlocked,
                                                             bool profileForgotten,
                                                             bool profilePreviouslyKnown)
{
  if (!historyBlocked && !profileForgotten)
    return JumpgatePairingHistoryAction::None;
  return profileForgotten || !profilePreviouslyKnown
             ? JumpgatePairingHistoryAction::PurgeThenUnblock
             : JumpgatePairingHistoryAction::UnblockPreserving;
}

} // namespace KODI::JUMPGATE
