/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateThreadRegistry.h"

#include <algorithm>
#include <utility>

namespace KODI::JUMPGATE
{

CJumpgateThreadRegistry::~CJumpgateThreadRegistry()
{
  JoinAll();
}

void CJumpgateThreadRegistry::Adopt(std::thread worker, WaitForCompletion waitForCompletion)
{
  if (!worker.joinable())
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  m_workers.push_back({std::move(worker), std::move(waitForCompletion)});
}

bool CJumpgateThreadRegistry::JoinAllFor(std::chrono::milliseconds timeout)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{0});
  std::vector<Worker> workers;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    workers.swap(m_workers);
  }

  std::vector<Worker> pending;
  for (Worker& worker : workers)
  {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                       : std::chrono::milliseconds{0};
    const bool completed = worker.waitForCompletion && worker.waitForCompletion(remaining);
    if (completed)
      worker.thread.join();
    else
      pending.emplace_back(std::move(worker));
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  for (Worker& worker : pending)
    m_workers.emplace_back(std::move(worker));
  return m_workers.empty();
}

void CJumpgateThreadRegistry::JoinAll()
{
  while (true)
  {
    std::vector<Worker> workers;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      workers.swap(m_workers);
    }
    if (workers.empty())
      return;

    for (Worker& worker : workers)
    {
      if (worker.thread.joinable())
        worker.thread.join();
    }
  }
}

std::size_t CJumpgateThreadRegistry::Pending() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_workers.size();
}

std::shared_ptr<CJumpgateThreadRegistry> CJumpgateThreadRegistry::Global()
{
  static std::shared_ptr<CJumpgateThreadRegistry> registry =
      std::make_shared<CJumpgateThreadRegistry>();
  return registry;
}

} // namespace KODI::JUMPGATE
