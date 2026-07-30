/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>

namespace KODI::JUMPGATE
{

class CJumpgateShutdownCoordinator final
{
public:
  bool RunOnceAndWait(const std::function<void()>& drain);
  bool IsDrained() const;

private:
  enum class State
  {
    Ready,
    Draining,
    Drained,
  };

  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  State m_state{State::Ready};
};

} // namespace KODI::JUMPGATE
