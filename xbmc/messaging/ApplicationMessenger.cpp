/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ApplicationMessenger.h"

#include "ServiceBroker.h"
#include "guilib/GUIMessage.h"
#include "messaging/IMessageTarget.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <memory>
#include <mutex>
#include <utility>

namespace KODI
{
namespace MESSAGING
{

class CDelayedMessage : public CThread
{
  public:
    CDelayedMessage(const ThreadMessage& msg, unsigned int delay);
    void Process() override;

  private:
    unsigned int   m_delay;
    ThreadMessage  m_msg;
};

CDelayedMessage::CDelayedMessage(const ThreadMessage& msg, unsigned int delay)
  : CThread("DelayedMessage"), m_msg(msg)
{
  m_delay = delay;
}

void CDelayedMessage::Process()
{
  CThread::Sleep(std::chrono::milliseconds(m_delay));

  if (!m_bStop)
    CServiceBroker::GetAppMessenger()->PostMsg(m_msg.dwMessage, m_msg.param1, m_msg.param1,
                                               m_msg.lpVoid, m_msg.strParam, m_msg.params);
}

CApplicationMessenger::CApplicationMessenger() = default;

CApplicationMessenger::~CApplicationMessenger()
{
  Cleanup();
}

void CApplicationMessenger::Cleanup()
{
  m_asyncCallbackExecutor.Stop();
  std::vector<ThreadMessage*> messages;
  {
    std::unique_lock lock(m_critSection);
    m_bStop = true;
    m_ownedCallbackGate.Stop();

    while (!m_vecMessages.empty())
    {
      messages.push_back(m_vecMessages.front());
      m_vecMessages.pop();
    }

    while (!m_vecWindowMessages.empty())
    {
      messages.push_back(m_vecWindowMessages.front());
      m_vecWindowMessages.pop();
    }
  }

  for (ThreadMessage* message : messages)
  {
    if (message->waitEvent)
      message->waitEvent->Set();
    if (message->ownedCallback)
      message->ownedCallback->Cancel();
    delete message;
  }
}

void CApplicationMessenger::Stop()
{
  m_asyncCallbackExecutor.Stop();
  {
    std::unique_lock lock(m_critSection);
    m_bStop = true;
    m_ownedCallbackGate.Stop();
  }
  CancelPendingOwnedCallbacks();
}

void CApplicationMessenger::CancelPendingOwnedCallbacks()
{
  std::vector<ThreadMessage*> cancelled;
  {
    std::unique_lock lock(m_critSection);
    const auto extractOwned = [&cancelled](std::queue<ThreadMessage*>& queue)
    {
      const std::size_t count = queue.size();
      for (std::size_t index = 0; index < count; ++index)
      {
        ThreadMessage* message = queue.front();
        queue.pop();
        if (message->dwMessage == TMSG_OWNED_CALLBACK || message->ownedPayload)
          cancelled.push_back(message);
        else
          queue.push(message);
      }
    };
    extractOwned(m_vecMessages);
    extractOwned(m_vecWindowMessages);
  }

  for (ThreadMessage* message : cancelled)
  {
    if (message->ownedCallback)
      message->ownedCallback->Cancel();
    delete message;
  }
}

int CApplicationMessenger::SendMsg(ThreadMessage&& message, bool wait)
{
  std::shared_ptr<CEvent> waitEvent;
  std::shared_ptr<int> result;

  if (wait)
  {
    //Initialize result here as it's not needed for posted messages
    message.result = std::make_shared<int>(-1);
    // check that we're not being called from our application thread, else we'll be waiting
    // forever!
    if (m_guiThreadId != CThread::GetCurrentThreadId())
    {
      message.waitEvent = std::make_shared<CEvent>(true);
      waitEvent = message.waitEvent;
      result = message.result;
    }
    else
    {
      //OutputDebugString("Attempting to wait on a SendMessage() from our application thread will cause lockup!\n");
      //OutputDebugString("Sending immediately\n");
      ProcessMessage(&message);
      return *message.result;
    }
  }


  std::unique_ptr<ThreadMessage> msg = std::make_unique<ThreadMessage>(std::move(message));

  std::unique_lock lock(m_critSection);
  if (m_bStop)
    return -1;

  if (msg->dwMessage == TMSG_GUI_MESSAGE)
    m_vecWindowMessages.push(msg.release());
  else
    m_vecMessages.push(msg.release());
  lock.unlock(); // this releases the lock on the vec of messages and
      //   allows the ProcessMessage to execute and therefore
      //   delete the message itself. Therefore any access
      //   of the message itself after this point constitutes
      //   a race condition (yarc - "yet another race condition")
      //
  if (waitEvent) // ... it just so happens we have a spare reference to the
                 //  waitEvent ... just for such contingencies :)
  {
    // ensure the thread doesn't hold the graphics lock
    CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
    //! @todo This won't really help as winSystem can die every single
    // moment on shutdown. A shared ptr would be a more valid solution
    // depending on the design dependencies.
    if (winSystem)
    {
      CSingleExit exit(winSystem->GetGfxContext());
      waitEvent->Wait();
    }
    return *result;
  }

  return -1;
}

int CApplicationMessenger::SendMsg(uint32_t messageId)
{
   return SendMsg(ThreadMessage{ messageId }, true);
}

int CApplicationMessenger::SendMsg(uint32_t messageId, int param1, int param2, void* payload)
{
  return SendMsg(ThreadMessage{ messageId, param1, param2, payload }, true);
}

int CApplicationMessenger::SendMsg(uint32_t messageId, int param1, int param2, void* payload, std::string strParam)
{
  return SendMsg(ThreadMessage{messageId, param1, param2, payload, std::move(strParam),
                               std::vector<std::string>{}},
                 true);
}

int CApplicationMessenger::SendMsg(uint32_t messageId, int param1, int param2, void* payload, std::string strParam, std::vector<std::string> params)
{
  return SendMsg(
      ThreadMessage{messageId, param1, param2, payload, std::move(strParam), std::move(params)},
      true);
}

void CApplicationMessenger::PostMsg(uint32_t messageId)
{
  SendMsg(ThreadMessage{ messageId }, false);
}

void CApplicationMessenger::PostMsg(uint32_t messageId, int64_t param3)
{
  SendMsg(ThreadMessage{ messageId, param3 }, false);
}

void CApplicationMessenger::PostMsg(uint32_t messageId, int param1, int param2, void* payload)
{
  SendMsg(ThreadMessage{ messageId, param1, param2, payload }, false);
}

void CApplicationMessenger::PostMsg(uint32_t messageId, int param1, int param2, void* payload, std::string strParam)
{
  SendMsg(ThreadMessage{messageId, param1, param2, payload, std::move(strParam),
                        std::vector<std::string>{}},
          false);
}

void CApplicationMessenger::PostMsg(uint32_t messageId, int param1, int param2, void* payload, std::string strParam, std::vector<std::string> params)
{
  SendMsg(ThreadMessage{messageId, param1, param2, payload, std::move(strParam), std::move(params)},
          false);
}

bool CApplicationMessenger::PostOwnedMsg(ThreadMessage&& message)
{
  CBoundedApplicationCallbackGate::ReservationPtr reservation =
      m_ownedCallbackGate.TryReserve();
  if (!reservation || !message.ownedPayload)
    return false;

  message.queueReservation = std::move(reservation);
  auto queued = std::make_unique<ThreadMessage>(std::move(message));
  std::unique_lock lock(m_critSection);
  if (m_bStop)
    return false;

  if (queued->dwMessage == TMSG_GUI_MESSAGE)
    m_vecWindowMessages.push(queued.release());
  else
    m_vecMessages.push(queued.release());
  return true;
}

bool CApplicationMessenger::PostCallback(std::shared_ptr<IApplicationCallback> callback)
{
  if (!callback)
    return false;

  CBoundedApplicationCallbackGate::ReservationPtr reservation =
      m_ownedCallbackGate.TryReserve();
  if (!reservation)
  {
    callback->Cancel();
    return false;
  }

  auto owned = std::make_shared<COwnedApplicationCallback>(std::move(callback), reservation);
  auto message = std::make_unique<ThreadMessage>(TMSG_OWNED_CALLBACK);
  message->ownedCallback = owned;
  {
    std::unique_lock lock(m_critSection);
    if (m_bStop)
      return false;
    m_vecMessages.push(message.release());
  }
  return true;
}

bool CApplicationMessenger::PostAsyncCallback(std::shared_ptr<IApplicationCallback> callback)
{
  return m_asyncCallbackExecutor.Post(std::move(callback));
}

void CApplicationMessenger::ProcessMessages()
{
  // process threadmessages
  std::unique_lock lock(m_critSection);
  while (!m_vecMessages.empty())
  {
    ThreadMessage* pMsg = m_vecMessages.front();
    //first remove the message from the queue, else the message could be processed more then once
    m_vecMessages.pop();

    //Leave here as the message might make another
    //thread call processmessages or sendmessage

    std::shared_ptr<CEvent> waitEvent = pMsg->waitEvent;
    lock.unlock(); // <- see the large comment in SendMessage ^

    ProcessMessage(pMsg);

    if (waitEvent)
      waitEvent->Set();
    delete pMsg;

    lock.lock();
  }
}

void CApplicationMessenger::ProcessMessage(ThreadMessage *pMsg)
{
  //special case for this that we handle ourselves
  if (pMsg->dwMessage == TMSG_CALLBACK)
  {
    ThreadMessageCallback *callback = static_cast<ThreadMessageCallback*>(pMsg->lpVoid);
    callback->callback(callback->userptr);
    return;
  }
  if (pMsg->dwMessage == TMSG_OWNED_CALLBACK)
  {
    if (pMsg->ownedCallback)
      pMsg->ownedCallback->Execute();
    return;
  }

  std::unique_lock lock(m_critSection);
  int mask = pMsg->dwMessage & TMSG_MASK_MESSAGE;

  const auto it = m_mapTargets.find(mask);
  if (it != m_mapTargets.end())
  {
    CSingleExit exit(m_critSection);
    it->second->OnApplicationMessage(pMsg);
  }
  else
    CLog::LogF(LOGERROR, "receiver {} is not defined", mask);
}

void CApplicationMessenger::ProcessWindowMessages()
{
  std::unique_lock lock(m_critSection);
  //message type is window, process window messages
  while (!m_vecWindowMessages.empty())
  {
    ThreadMessage* pMsg = m_vecWindowMessages.front();
    //first remove the message from the queue, else the message could be processed more then once
    m_vecWindowMessages.pop();

    // leave here in case we make more thread messages from this one

    std::shared_ptr<CEvent> waitEvent = pMsg->waitEvent;
    lock.unlock(); // <- see the large comment in SendMessage ^

    ProcessMessage(pMsg);
    if (waitEvent)
      waitEvent->Set();
    delete pMsg;

    lock.lock();
  }
}

void CApplicationMessenger::SendGUIMessage(const CGUIMessage &message, int windowID, bool waitResult)
{
  ThreadMessage tMsg(TMSG_GUI_MESSAGE);
  tMsg.param1 = windowID == WINDOW_INVALID ? 0 : windowID;
  tMsg.lpVoid = new CGUIMessage(message);
  SendMsg(std::move(tMsg), waitResult);
}

void CApplicationMessenger::RegisterReceiver(IMessageTarget* target)
{
  std::unique_lock lock(m_critSection);
  m_mapTargets.insert(std::make_pair(target->GetMessageMask(), target));
}

bool CApplicationMessenger::IsProcessThread() const
{
  return m_processThreadId == CThread::GetCurrentThreadId();
}
}
}
