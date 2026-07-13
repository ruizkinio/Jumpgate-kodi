/*
 *  Copyright (C) 2026 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackRetry.h"

namespace KODI::JUMPGATE
{

JumpgatePlaybackRetryAction GetJumpgatePlaybackRetryAction(bool profileBacked,
                                                           bool contentIdentified,
                                                           bool identifyFailed,
                                                           int64_t playbackStartTime,
                                                           int64_t now,
                                                           int64_t retryWindowSeconds)
{
  if (profileBacked || contentIdentified || identifyFailed || playbackStartTime <= 0 ||
      now < playbackStartTime || retryWindowSeconds < 0)
  {
    return JumpgatePlaybackRetryAction::None;
  }
  return now - playbackStartTime <= retryWindowSeconds ? JumpgatePlaybackRetryAction::Retry
                                                       : JumpgatePlaybackRetryAction::Expire;
}

} // namespace KODI::JUMPGATE
