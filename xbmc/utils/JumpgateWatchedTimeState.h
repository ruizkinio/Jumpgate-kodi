/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

namespace KODI::JUMPGATE
{

class CJumpgateWatchedTimeState final
{
public:
  void Reset() noexcept;
  void SetPlaying(bool playing) noexcept;
  void SetPaused(bool paused) noexcept;
  void SetBackgrounded(bool backgrounded) noexcept;

  std::int64_t Observe(std::int64_t nowMs, std::int64_t positionMs, double playbackSpeed) noexcept;
  std::int64_t WatchedMs() const noexcept { return m_watchedMs; }

private:
  void InvalidateAnchor() noexcept;
  bool IsCrediting() const noexcept;

  std::int64_t m_watchedMs{0};
  std::int64_t m_observedAtMs{0};
  std::int64_t m_positionMs{0};
  bool m_anchorValid{false};
  bool m_playing{false};
  bool m_paused{false};
  bool m_backgrounded{false};
};

} // namespace KODI::JUMPGATE
