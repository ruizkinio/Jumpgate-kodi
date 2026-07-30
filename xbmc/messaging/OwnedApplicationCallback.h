/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace KODI::MESSAGING
{

class IApplicationCallback
{
public:
  virtual ~IApplicationCallback() = default;
  virtual void Execute() = 0;
  virtual void Cancel() noexcept = 0;
};

class COwnedApplicationCallback final
{
public:
  explicit COwnedApplicationCallback(std::shared_ptr<IApplicationCallback> callback,
                                     std::shared_ptr<void> reservation = {})
    : m_callback(std::move(callback)),
      m_reservation(std::move(reservation))
  {
  }

  ~COwnedApplicationCallback() { Cancel(); }

  COwnedApplicationCallback(const COwnedApplicationCallback&) = delete;
  COwnedApplicationCallback& operator=(const COwnedApplicationCallback&) = delete;

  bool Execute()
  {
    State expected = State::PENDING;
    if (!m_state.compare_exchange_strong(expected, State::EXECUTING, std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
      return false;
    }

    std::shared_ptr<IApplicationCallback> callback;
    {
      std::lock_guard lock(m_mutex);
      callback = m_callback;
    }

    try
    {
      if (callback)
        callback->Execute();
    }
    catch (...)
    {
      Finish();
      throw;
    }

    Finish();
    return true;
  }

  bool Cancel() noexcept
  {
    State expected = State::PENDING;
    if (!m_state.compare_exchange_strong(expected, State::CANCELLING, std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
      return false;
    }

    std::shared_ptr<IApplicationCallback> callback;
    {
      std::lock_guard lock(m_mutex);
      callback = m_callback;
    }
    if (callback)
      callback->Cancel();
    Finish();
    return true;
  }

  bool IsPending() const noexcept
  {
    return m_state.load(std::memory_order_acquire) == State::PENDING;
  }

private:
  enum class State : unsigned char
  {
    PENDING,
    EXECUTING,
    CANCELLING,
    FINISHED,
  };

  void Finish() noexcept
  {
    {
      std::lock_guard lock(m_mutex);
      m_callback.reset();
      m_reservation.reset();
    }
    m_state.store(State::FINISHED, std::memory_order_release);
  }

  mutable std::mutex m_mutex;
  std::shared_ptr<IApplicationCallback> m_callback;
  std::shared_ptr<void> m_reservation;
  std::atomic<State> m_state{State::PENDING};
};

class CBoundedApplicationCallbackGate final
{
  struct State
  {
    explicit State(std::size_t maximum) : capacity(maximum == 0 ? 1 : maximum) {}

    std::mutex mutex;
    const std::size_t capacity;
    std::size_t pending{0};
    bool stopped{false};
  };

public:
  class Reservation final
  {
  public:
    ~Reservation()
    {
      const std::shared_ptr<State> state = std::move(m_state);
      if (!state)
        return;

      std::lock_guard lock(state->mutex);
      if (state->pending != 0)
        --state->pending;
    }

  private:
    friend class CBoundedApplicationCallbackGate;
    explicit Reservation(std::shared_ptr<State> state) : m_state(std::move(state)) {}

    std::shared_ptr<State> m_state;
  };

  using ReservationPtr = std::shared_ptr<Reservation>;

  explicit CBoundedApplicationCallbackGate(std::size_t capacity)
    : m_state(std::make_shared<State>(capacity))
  {
  }

  ReservationPtr TryReserve()
  {
    const std::shared_ptr<State> state = m_state;
    {
      std::lock_guard lock(state->mutex);
      if (state->stopped || state->pending >= state->capacity)
        return {};
      ++state->pending;
    }

    try
    {
      return std::shared_ptr<Reservation>(new Reservation(state));
    }
    catch (...)
    {
      std::lock_guard lock(state->mutex);
      --state->pending;
      throw;
    }
  }

  void Stop()
  {
    const std::shared_ptr<State> state = m_state;
    std::lock_guard lock(state->mutex);
    state->stopped = true;
  }

  std::size_t Pending() const
  {
    const std::shared_ptr<State> state = m_state;
    std::lock_guard lock(state->mutex);
    return state->pending;
  }

  bool IsStopped() const
  {
    const std::shared_ptr<State> state = m_state;
    std::lock_guard lock(state->mutex);
    return state->stopped;
  }

private:
  std::shared_ptr<State> m_state;
};

class CBoundedApplicationCallbackExecutor final
{
  struct State
  {
    explicit State(std::size_t maximum) : capacity(maximum == 0 ? 1 : maximum) {}

    std::mutex mutex;
    std::condition_variable condition;
    const std::size_t capacity;
    std::deque<std::shared_ptr<COwnedApplicationCallback>> pending;
    bool stopped{false};
  };

public:
  explicit CBoundedApplicationCallbackExecutor(std::size_t capacity)
    : m_state(std::make_shared<State>(capacity))
  {
    const std::shared_ptr<State> state = m_state;
    std::thread([state] { Run(state); }).detach();
  }

  ~CBoundedApplicationCallbackExecutor() { Stop(); }

  CBoundedApplicationCallbackExecutor(const CBoundedApplicationCallbackExecutor&) = delete;
  CBoundedApplicationCallbackExecutor& operator=(const CBoundedApplicationCallbackExecutor&) =
      delete;

  bool Post(std::shared_ptr<IApplicationCallback> callback)
  {
    if (!callback)
      return false;

    auto owned = std::make_shared<COwnedApplicationCallback>(std::move(callback));
    const std::shared_ptr<State> state = m_state;
    {
      std::lock_guard lock(state->mutex);
      if (!state->stopped && state->pending.size() < state->capacity)
      {
        state->pending.emplace_back(std::move(owned));
        state->condition.notify_one();
        return true;
      }
    }

    owned->Cancel();
    return false;
  }

  void Stop() noexcept
  {
    const std::shared_ptr<State> state = m_state;
    std::vector<std::shared_ptr<COwnedApplicationCallback>> cancelled;
    {
      std::lock_guard lock(state->mutex);
      if (state->stopped)
        return;

      state->stopped = true;
      cancelled.assign(std::make_move_iterator(state->pending.begin()),
                       std::make_move_iterator(state->pending.end()));
      state->pending.clear();
    }
    state->condition.notify_all();
    for (const auto& callback : cancelled)
      callback->Cancel();
  }

  std::size_t Pending() const
  {
    const std::shared_ptr<State> state = m_state;
    std::lock_guard lock(state->mutex);
    return state->pending.size();
  }

  bool IsStopped() const
  {
    const std::shared_ptr<State> state = m_state;
    std::lock_guard lock(state->mutex);
    return state->stopped;
  }

private:
  static void Run(const std::shared_ptr<State>& state) noexcept
  {
    while (true)
    {
      std::shared_ptr<COwnedApplicationCallback> callback;
      {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&state] { return state->stopped || !state->pending.empty(); });
        if (state->stopped && state->pending.empty())
          return;

        callback = std::move(state->pending.front());
        state->pending.pop_front();
      }

      try
      {
        callback->Execute();
      }
      catch (...)
      {
        // Callback ownership has already reached a terminal state. The worker
        // remains available for unrelated exact-generation work.
      }
    }
  }

  std::shared_ptr<State> m_state;
};

} // namespace KODI::MESSAGING
