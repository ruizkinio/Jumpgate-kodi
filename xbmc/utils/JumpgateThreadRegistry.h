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
#include <thread>
#include <utility>

namespace KODI::JUMPGATE
{

class CJumpgateThreadRegistry final
{
  struct State;
  struct SlotLease;
  struct Worker;
  struct Reaper;

public:
  using WaitForCompletion = std::function<bool(std::chrono::milliseconds)>;

  class Reservation final
  {
  public:
    Reservation() = default;
    ~Reservation() = default;
    Reservation(Reservation&&) noexcept = default;
    Reservation& operator=(Reservation&&) noexcept = default;

    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    explicit operator bool() const noexcept
    {
      return static_cast<bool>(m_lease) && static_cast<bool>(m_workerSlot);
    }
    void Reset() noexcept
    {
      m_workerSlot.reset();
      m_lease.reset();
    }

  private:
    friend class CJumpgateThreadRegistry;
    Reservation(std::shared_ptr<SlotLease> lease, std::shared_ptr<Worker> workerSlot)
      : m_lease(std::move(lease)),
        m_workerSlot(std::move(workerSlot))
    {
    }

    std::shared_ptr<SlotLease> m_lease;
    std::shared_ptr<Worker> m_workerSlot;
  };

  explicit CJumpgateThreadRegistry(std::size_t maximumWorkers = 64);
  ~CJumpgateThreadRegistry();

  CJumpgateThreadRegistry(const CJumpgateThreadRegistry&) = delete;
  CJumpgateThreadRegistry& operator=(const CJumpgateThreadRegistry&) = delete;

  Reservation Reserve();
  // The completion callback must capture every object it accesses by shared ownership. The
  // process-lifetime reaper may invoke it after this registry and its owning service are gone.
  void Adopt(std::thread& worker, Reservation&& reservation, WaitForCompletion waitForCompletion);
  bool JoinAllFor(std::chrono::milliseconds timeout);
  bool JoinAll(std::chrono::milliseconds timeout = std::chrono::seconds{12});
  std::size_t Pending() const;
  std::size_t Occupied() const;
  bool AdmissionClosed() const;

  static std::shared_ptr<CJumpgateThreadRegistry> Global();

private:
  static Reaper& ProcessReaper();
  bool Drain(std::chrono::milliseconds timeout, bool closeAdmission);

  std::shared_ptr<State> m_state;
};

} // namespace KODI::JUMPGATE
