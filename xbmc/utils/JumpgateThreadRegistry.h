/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace KODI::JUMPGATE
{

class CJumpgateThreadRegistry final
{
public:
  using WaitForCompletion = std::function<bool(std::chrono::milliseconds)>;

  CJumpgateThreadRegistry() = default;
  ~CJumpgateThreadRegistry();

  CJumpgateThreadRegistry(const CJumpgateThreadRegistry&) = delete;
  CJumpgateThreadRegistry& operator=(const CJumpgateThreadRegistry&) = delete;

  void Adopt(std::thread worker, WaitForCompletion waitForCompletion);
  bool JoinAllFor(std::chrono::milliseconds timeout);
  void JoinAll();
  std::size_t Pending() const;

  static std::shared_ptr<CJumpgateThreadRegistry> Global();

private:
  struct Worker
  {
    std::thread thread;
    WaitForCompletion waitForCompletion;
  };

  mutable std::mutex m_mutex;
  std::vector<Worker> m_workers;
};

} // namespace KODI::JUMPGATE
