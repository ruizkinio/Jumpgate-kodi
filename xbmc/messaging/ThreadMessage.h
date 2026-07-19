/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class CEvent;

namespace KODI
{
namespace MESSAGING
{

class CApplicationMessenger;
class COwnedApplicationCallback;

class COwnedThreadMessagePayload final
{
public:
  using Deleter = void (*)(void*);

  COwnedThreadMessagePayload(void* payload,
                             Deleter deleter,
                             std::function<void()> cancellation)
    : m_payload(payload),
      m_deleter(deleter),
      m_cancellation(std::move(cancellation))
  {
  }

  ~COwnedThreadMessagePayload() { Cancel(); }

  COwnedThreadMessagePayload(const COwnedThreadMessagePayload&) = delete;
  COwnedThreadMessagePayload& operator=(const COwnedThreadMessagePayload&) = delete;

  void* Take() noexcept
  {
    std::lock_guard lock(m_mutex);
    void* payload = m_payload;
    m_payload = nullptr;
    m_cancellation = {};
    return payload;
  }

  bool Cancel() noexcept
  {
    void* payload{nullptr};
    std::function<void()> cancellation;
    {
      std::lock_guard lock(m_mutex);
      if (m_payload == nullptr)
        return false;
      payload = m_payload;
      m_payload = nullptr;
      cancellation = std::move(m_cancellation);
    }

    m_deleter(payload);
    if (cancellation)
    {
      try
      {
        cancellation();
      }
      catch (...)
      {
      }
    }
    return true;
  }

private:
  std::mutex m_mutex;
  void* m_payload{nullptr};
  Deleter m_deleter{nullptr};
  std::function<void()> m_cancellation;
};

class ThreadMessage
{
  friend CApplicationMessenger;
public:
  ThreadMessage()
    : ThreadMessage{ 0, -1, -1, nullptr }
  {
  }

  explicit ThreadMessage(uint32_t messageId)
    : ThreadMessage{ messageId, -1, -1, nullptr }
  {
  }

  ThreadMessage(uint32_t messageId, int64_t p3)
  : ThreadMessage{ messageId, -1, -1, nullptr, p3 }
  {
  }

  ThreadMessage(uint32_t messageId, int p1, int p2, void* payload, int64_t p3 = 0)
    : dwMessage{ messageId }
    , param1{ p1 }
    , param2{ p2 }
    , param3{ p3 }
    , lpVoid{ payload }
  {
  }

  ThreadMessage(uint32_t messageId,
                int p1,
                int p2,
                void* payload,
                std::string param,
                std::vector<std::string> vecParams)
    : dwMessage{messageId},
      param1{p1},
      param2{p2},
      param3{0},
      lpVoid{payload},
      strParam(std::move(param)),
      params(std::move(vecParams))
  {
  }

  ThreadMessage(const ThreadMessage& other) = default;

  ThreadMessage(ThreadMessage&& other) noexcept
    : dwMessage(other.dwMessage),
      param1(other.param1),
      param2(other.param2),
      param3(other.param3),
      lpVoid(other.lpVoid),
      strParam(std::move(other.strParam)),
      params(std::move(other.params)),
      waitEvent(std::move(other.waitEvent)),
      result(std::move(other.result)),
      ownedCallback(std::move(other.ownedCallback)),
      ownedPayload(std::move(other.ownedPayload)),
      queueReservation(std::move(other.queueReservation))
  {
  }

  ThreadMessage& operator=(const ThreadMessage& other)
  {
    if (this == &other)
      return *this;
    dwMessage = other.dwMessage;
    param1 = other.param1;
    param2 = other.param2;
    param3 = other.param3;
    lpVoid = other.lpVoid;
    strParam = other.strParam;
    params = other.params;
    waitEvent = other.waitEvent;
    result = other.result;
    ownedCallback = other.ownedCallback;
    ownedPayload = other.ownedPayload;
    queueReservation = other.queueReservation;
    return *this;
  }

  ThreadMessage& operator=(ThreadMessage&& other) noexcept
  {
    if (this == &other)
      return *this;
    dwMessage = other.dwMessage;
    param1 = other.param1;
    param2 = other.param2;
    param3 = other.param3;
    lpVoid = other.lpVoid;
    strParam = std::move(other.strParam);
    params = std::move(other.params);
    waitEvent = std::move(other.waitEvent);
    result = std::move(other.result);
    ownedCallback = std::move(other.ownedCallback);
    ownedPayload = std::move(other.ownedPayload);
    queueReservation = std::move(other.queueReservation);
    return *this;
  }

  template<typename Payload>
  std::shared_ptr<COwnedThreadMessagePayload> SetOwnedPayload(
      std::unique_ptr<Payload> payload, std::function<void()> cancellation = {})
  {
    if (!payload)
      return {};

    auto owned = std::make_shared<COwnedThreadMessagePayload>(
        payload.get(), [](void* value) { delete static_cast<Payload*>(value); },
        std::move(cancellation));
    lpVoid = payload.release();
    ownedPayload = owned;
    return owned;
  }

  template<typename Payload>
  std::unique_ptr<Payload> TakeOwnedPayload() noexcept
  {
    if (ownedPayload)
    {
      lpVoid = nullptr;
      return std::unique_ptr<Payload>(static_cast<Payload*>(ownedPayload->Take()));
    }

    Payload* payload = static_cast<Payload*>(lpVoid);
    lpVoid = nullptr;
    return std::unique_ptr<Payload>(payload);
  }

  uint32_t dwMessage;
  int param1;
  int param2;
  int64_t param3;
  void* lpVoid;
  std::string strParam;
  std::vector<std::string> params;

  /*!
   * \brief set the message return value, will only be returned when
   *        the message is sent using SendMsg
   * \param [in] res the return value or a result status code that is returned to the caller
   */
  void SetResult(int res) const
  {
    //On posted messages result will be zero, since they can't
    //retrieve the response we silently ignore this to let message
    //handlers not have to worry about it
    if (result)
      *result = res;
  }
protected:
  std::shared_ptr<CEvent> waitEvent;
  std::shared_ptr<int> result;
  std::shared_ptr<COwnedApplicationCallback> ownedCallback;
  std::shared_ptr<COwnedThreadMessagePayload> ownedPayload;
  std::shared_ptr<void> queueReservation;
};
}
}
