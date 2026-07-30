/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateShutdownCoordinator.h"

namespace KODI::JUMPGATE
{

bool CJumpgateShutdownCoordinator::RunOnceAndWait(const std::function<void()>& drain)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  m_condition.wait(lock, [this] { return m_state != State::Draining; });
  if (m_state == State::Drained)
    return false;

  m_state = State::Draining;
  lock.unlock();
  try
  {
    drain();
  }
  catch (...)
  {
    lock.lock();
    m_state = State::Ready;
    lock.unlock();
    m_condition.notify_all();
    throw;
  }

  lock.lock();
  m_state = State::Drained;
  lock.unlock();
  m_condition.notify_all();
  return true;
}

bool CJumpgateShutdownCoordinator::IsDrained() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state == State::Drained;
}

} // namespace KODI::JUMPGATE
