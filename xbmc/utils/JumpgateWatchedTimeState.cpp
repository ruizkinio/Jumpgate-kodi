/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateWatchedTimeState.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace KODI::JUMPGATE
{
namespace
{
constexpr double MIN_PLAYBACK_SPEED = 0.25;
constexpr double MAX_PLAYBACK_SPEED = 4.0;
constexpr std::int64_t CLOCK_JITTER_ALLOWANCE_MS = 250;
} // namespace

void CJumpgateWatchedTimeState::Reset() noexcept
{
  m_watchedMs = 0;
  m_observedAtMs = 0;
  m_positionMs = 0;
  m_anchorValid = false;
  m_playing = false;
  m_paused = false;
  m_backgrounded = false;
}

void CJumpgateWatchedTimeState::SetPlaying(bool playing) noexcept
{
  if (m_playing == playing)
    return;
  m_playing = playing;
  InvalidateAnchor();
}

void CJumpgateWatchedTimeState::SetPaused(bool paused) noexcept
{
  if (m_paused == paused)
    return;
  m_paused = paused;
  InvalidateAnchor();
}

void CJumpgateWatchedTimeState::SetBackgrounded(bool backgrounded) noexcept
{
  if (m_backgrounded == backgrounded)
    return;
  m_backgrounded = backgrounded;
  InvalidateAnchor();
}

std::int64_t CJumpgateWatchedTimeState::Observe(std::int64_t nowMs,
                                                std::int64_t positionMs,
                                                double playbackSpeed) noexcept
{
  positionMs = std::max<std::int64_t>(0, positionMs);
  if (nowMs <= 0)
  {
    InvalidateAnchor();
    return m_watchedMs;
  }

  if (!m_anchorValid)
  {
    m_observedAtMs = nowMs;
    m_positionMs = positionMs;
    m_anchorValid = true;
    return m_watchedMs;
  }

  const std::int64_t elapsedMs = nowMs > m_observedAtMs ? nowMs - m_observedAtMs : 0;
  const std::int64_t advancedMs = positionMs > m_positionMs ? positionMs - m_positionMs : 0;
  if (IsCrediting() && elapsedMs > 0 && advancedMs > 0 && std::isfinite(playbackSpeed) &&
      playbackSpeed >= MIN_PLAYBACK_SPEED && playbackSpeed <= MAX_PLAYBACK_SPEED)
  {
    const long double expectedAdvance =
        static_cast<long double>(elapsedMs) * static_cast<long double>(playbackSpeed);
    const long double boundedAdvance = expectedAdvance + CLOCK_JITTER_ALLOWANCE_MS;
    const long double maximumInteger =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    const std::int64_t maximumCreditableMs =
        boundedAdvance >= maximumInteger ? std::numeric_limits<std::int64_t>::max()
                                         : static_cast<std::int64_t>(std::ceil(boundedAdvance));
    const std::int64_t creditedMs = std::min(advancedMs, maximumCreditableMs);
    if (m_watchedMs <= std::numeric_limits<std::int64_t>::max() - creditedMs)
      m_watchedMs += creditedMs;
    else
      m_watchedMs = std::numeric_limits<std::int64_t>::max();
  }

  m_observedAtMs = nowMs;
  m_positionMs = positionMs;
  return m_watchedMs;
}

void CJumpgateWatchedTimeState::InvalidateAnchor() noexcept
{
  m_observedAtMs = 0;
  m_positionMs = 0;
  m_anchorValid = false;
}

bool CJumpgateWatchedTimeState::IsCrediting() const noexcept
{
  return m_playing && !m_paused && !m_backgrounded;
}

} // namespace KODI::JUMPGATE
