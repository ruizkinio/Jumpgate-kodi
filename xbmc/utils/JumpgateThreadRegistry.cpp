/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateThreadRegistry.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace KODI::JUMPGATE
{

struct CJumpgateThreadRegistry::Worker
{
  std::thread thread;
  std::shared_ptr<SlotLease> lease;
  WaitForCompletion waitForCompletion;
  std::shared_ptr<State> state;
  std::shared_ptr<Worker> next;
};

struct CJumpgateThreadRegistry::State
{
  explicit State(std::size_t capacity) : maximumWorkers(capacity) {}

  std::mutex mutex;
  std::condition_variable condition;
  std::size_t maximumWorkers{0};
  std::size_t occupiedSlots{0};
  std::size_t pendingWorkers{0};
  bool admissionClosed{false};
};

struct CJumpgateThreadRegistry::SlotLease
{
  explicit SlotLease(std::shared_ptr<State> registryState) : state(std::move(registryState)) {}
  ~SlotLease()
  {
    const std::shared_ptr<State> current = state;
    if (!current)
      return;
    {
      std::lock_guard lock(current->mutex);
      if (current->occupiedSlots > 0)
        --current->occupiedSlots;
    }
    current->condition.notify_all();
  }

  std::shared_ptr<State> state;
};

struct CJumpgateThreadRegistry::Reaper
{
  Reaper() : thread([this] { Run(); }) {}

  void Submit(const std::shared_ptr<Worker>& worker)
  {
    {
      std::lock_guard lock(mutex);
      worker->next = std::move(head);
      head = worker;
    }
    condition.notify_one();
  }

  void Run()
  {
    std::unique_lock lock(mutex);
    while (true)
    {
      if (!head)
        condition.wait(lock, [this] { return head != nullptr; });
      else
        condition.wait_for(lock, std::chrono::milliseconds{2});
      std::shared_ptr<Worker> candidate = head;
      while (candidate)
      {
        const std::shared_ptr<Worker> next = candidate->next;
        bool completed = false;
        lock.unlock();
        try
        {
          completed = candidate->waitForCompletion(std::chrono::milliseconds{0});
        }
        catch (...)
        {
        }
        lock.lock();
        if (!completed)
        {
          candidate = next;
          continue;
        }

        std::shared_ptr<Worker>* link = &head;
        while (*link && link->get() != candidate.get())
          link = &(*link)->next;
        if (!*link)
        {
          candidate = next;
          continue;
        }
        *link = candidate->next;
        lock.unlock();
        if (candidate->thread.joinable())
          candidate->thread.join();
        const std::shared_ptr<State> state = candidate->state;
        candidate->lease.reset();
        if (state)
        {
          {
            std::lock_guard stateLock(state->mutex);
            if (state->pendingWorkers > 0)
              --state->pendingWorkers;
          }
          state->condition.notify_all();
        }
        lock.lock();
        candidate->next.reset();
        candidate = next;
      }
    }
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::shared_ptr<Worker> head;
  std::thread thread;
};

CJumpgateThreadRegistry::Reaper& CJumpgateThreadRegistry::ProcessReaper()
{
  // This intentionally outlives static destruction so timed-out joinable workers always retain a
  // valid owner and can be joined when they eventually finish.
  static Reaper* const reaper = new Reaper();
  return *reaper;
}

CJumpgateThreadRegistry::CJumpgateThreadRegistry(std::size_t maximumWorkers)
  : m_state(std::make_shared<State>(maximumWorkers == 0 ? 1 : maximumWorkers))
{
  static_cast<void>(ProcessReaper());
}

CJumpgateThreadRegistry::~CJumpgateThreadRegistry()
{
  JoinAll(std::chrono::seconds{1});
  // Timed-out workers retain State through the process-lifetime reaper.
  m_state.reset();
}

CJumpgateThreadRegistry::Reservation CJumpgateThreadRegistry::Reserve()
{
  const std::shared_ptr<State> state = m_state;
  if (!state)
    return {};
  {
    std::lock_guard lock(state->mutex);
    if (state->admissionClosed || state->occupiedSlots >= state->maximumWorkers)
      return {};
    ++state->occupiedSlots;
  }
  std::shared_ptr<SlotLease> lease;
  try
  {
    lease = std::make_shared<SlotLease>(state);
    return Reservation{lease, std::make_shared<Worker>()};
  }
  catch (...)
  {
    if (lease)
      lease.reset();
    else
    {
      {
        std::lock_guard lock(state->mutex);
        --state->occupiedSlots;
      }
      state->condition.notify_all();
    }
    return {};
  }
}

void CJumpgateThreadRegistry::Adopt(std::thread& worker,
                                    Reservation&& reservation,
                                    WaitForCompletion waitForCompletion)
{
  const std::shared_ptr<State> state = m_state;
  if (!state || !worker.joinable() || !reservation || !waitForCompletion ||
      reservation.m_lease->state != state)
  {
    throw std::logic_error("Invalid Jumpgate worker adoption");
  }

  const std::shared_ptr<Worker> workerSlot = std::move(reservation.m_workerSlot);
  workerSlot->thread = std::move(worker);
  workerSlot->lease = std::move(reservation.m_lease);
  workerSlot->waitForCompletion = std::move(waitForCompletion);
  workerSlot->state = state;
  {
    std::lock_guard lock(state->mutex);
    ++state->pendingWorkers;
  }
  ProcessReaper().Submit(workerSlot);
  state->condition.notify_all();
}

bool CJumpgateThreadRegistry::Drain(std::chrono::milliseconds timeout, bool closeAdmission)
{
  const std::shared_ptr<State> state = m_state;
  if (!state)
    return true;
  const auto deadline =
      std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{0});

  std::unique_lock lock(state->mutex);
  if (closeAdmission)
    state->admissionClosed = true;
  const auto drained = [&state, closeAdmission]
  { return state->pendingWorkers == 0 && (!closeAdmission || state->occupiedSlots == 0); };
  return drained() || state->condition.wait_until(lock, deadline, drained);
}

bool CJumpgateThreadRegistry::JoinAllFor(std::chrono::milliseconds timeout)
{
  return Drain(timeout, false);
}

bool CJumpgateThreadRegistry::JoinAll(std::chrono::milliseconds timeout)
{
  return Drain(timeout, true);
}

std::size_t CJumpgateThreadRegistry::Pending() const
{
  const std::shared_ptr<State> state = m_state;
  if (!state)
    return 0;
  std::lock_guard lock(state->mutex);
  return state->pendingWorkers;
}

std::size_t CJumpgateThreadRegistry::Occupied() const
{
  const std::shared_ptr<State> state = m_state;
  if (!state)
    return 0;
  std::lock_guard lock(state->mutex);
  return state->occupiedSlots;
}

bool CJumpgateThreadRegistry::AdmissionClosed() const
{
  const std::shared_ptr<State> state = m_state;
  if (!state)
    return true;
  std::lock_guard lock(state->mutex);
  return state->admissionClosed;
}

std::shared_ptr<CJumpgateThreadRegistry> CJumpgateThreadRegistry::Global()
{
  static const auto* registry =
      new std::shared_ptr<CJumpgateThreadRegistry>(std::make_shared<CJumpgateThreadRegistry>());
  return *registry;
}

} // namespace KODI::JUMPGATE
