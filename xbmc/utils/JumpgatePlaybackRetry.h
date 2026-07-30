/*
 *  Copyright (C) 2026 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

namespace KODI::JUMPGATE
{

enum class JumpgatePlaybackRetryAction
{
  None,
  Retry,
  Expire,
};

JumpgatePlaybackRetryAction GetJumpgatePlaybackRetryAction(bool profileBacked,
                                                           bool contentIdentified,
                                                           bool identifyFailed,
                                                           int64_t playbackStartTime,
                                                           int64_t now,
                                                           int64_t retryWindowSeconds);

} // namespace KODI::JUMPGATE
