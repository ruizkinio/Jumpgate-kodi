/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "URL.h"
#include "filesystem/CurlFile.h"
#include "network/Socket.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using XFILE::CCurlFile;

namespace
{
static_assert(!std::is_copy_constructible_v<CCurlFile>);
static_assert(!std::is_copy_assignable_v<CCurlFile>);

struct HttpReply
{
  std::string first;
  std::string second;
  bool waitBetweenParts{false};
};

class CLoopbackHttpServer
{
public:
  using ReplyFactory = std::function<HttpReply(std::size_t, const std::string&)>;

  explicit CLoopbackHttpServer(ReplyFactory replyFactory) : m_replyFactory(std::move(replyFactory))
  {
#ifdef TARGET_WINDOWS
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("WSAStartup failed");
#endif

    try
    {
      m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (m_listenSocket == INVALID_SOCKET)
        throw std::runtime_error("socket failed");

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      if (bind(m_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
          SOCKET_ERROR)
        throw std::runtime_error("bind failed");

      socklen_t addressLength = sizeof(address);
      if (getsockname(m_listenSocket, reinterpret_cast<sockaddr*>(&address), &addressLength) ==
          SOCKET_ERROR)
        throw std::runtime_error("getsockname failed");
      m_port = ntohs(address.sin_port);

      if (listen(m_listenSocket, 8) == SOCKET_ERROR)
        throw std::runtime_error("listen failed");

      m_acceptThread = std::thread([this] { AcceptLoop(); });
    }
    catch (...)
    {
      if (m_listenSocket != INVALID_SOCKET)
        closesocket(m_listenSocket);
#ifdef TARGET_WINDOWS
      WSACleanup();
#endif
      throw;
    }
  }

  ~CLoopbackHttpServer()
  {
    m_stop.store(true, std::memory_order_release);
    Release();
    if (m_acceptThread.joinable())
      m_acceptThread.join();
    if (m_listenSocket != INVALID_SOCKET)
    {
      closesocket(m_listenSocket);
      m_listenSocket = INVALID_SOCKET;
    }
    for (std::thread& client : m_clientThreads)
    {
      if (client.joinable())
        client.join();
    }
#ifdef TARGET_WINDOWS
    WSACleanup();
#endif
  }

  CLoopbackHttpServer(const CLoopbackHttpServer&) = delete;
  CLoopbackHttpServer& operator=(const CLoopbackHttpServer&) = delete;

  std::string Url(std::string_view path = "/") const
  {
    return "http://127.0.0.1:" + std::to_string(m_port) + std::string{path};
  }

  bool WaitForRequests(std::size_t count, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock{m_mutex};
    return m_requestChanged.wait_for(lock, timeout,
                                     [this, count] { return m_requests.size() >= count; });
  }

  std::size_t RequestCount() const
  {
    std::lock_guard lock{m_mutex};
    return m_requests.size();
  }

  std::string Request(std::size_t index) const
  {
    std::lock_guard lock{m_mutex};
    return index < m_requests.size() ? m_requests[index] : std::string{};
  }

  void Release()
  {
    {
      std::lock_guard lock{m_releaseMutex};
      m_released = true;
    }
    m_releaseChanged.notify_all();
  }

private:
  void AcceptLoop()
  {
    while (!m_stop.load(std::memory_order_acquire))
    {
      fd_set sockets;
      FD_ZERO(&sockets);
      FD_SET(m_listenSocket, &sockets);
      timeval timeout{0, 50 * 1000};
      const int selected =
          select(static_cast<int>(m_listenSocket) + 1, &sockets, nullptr, nullptr, &timeout);
      if (selected <= 0)
        continue;

      SOCKET client = accept(m_listenSocket, nullptr, nullptr);
      if (client == INVALID_SOCKET)
        continue;
      m_clientThreads.emplace_back([this, client] { ServeClient(client); });
    }
  }

  std::string ReadRequest(SOCKET client)
  {
    std::string request;
    while (!m_stop.load(std::memory_order_acquire) && request.size() < 64 * 1024)
    {
      fd_set sockets;
      FD_ZERO(&sockets);
      FD_SET(client, &sockets);
      timeval timeout{0, 50 * 1000};
      const int selected =
          select(static_cast<int>(client) + 1, &sockets, nullptr, nullptr, &timeout);
      if (selected <= 0)
        continue;

      char buffer[2048];
      const int received = recv(client, buffer, sizeof(buffer), 0);
      if (received <= 0)
        break;
      request.append(buffer, received);
      if (request.find("\r\n\r\n") != std::string::npos)
        break;
    }
    return request;
  }

  static void SendAll(SOCKET client, std::string_view data)
  {
    while (!data.empty())
    {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const int sent = send(client, data.data(), static_cast<int>(data.size()), flags);
      if (sent <= 0)
        return;
      data.remove_prefix(sent);
    }
  }

  void ServeClient(SOCKET client)
  {
#ifdef SO_NOSIGPIPE
    const int noSignal = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&noSignal),
               sizeof(noSignal));
#endif
    const std::string request = ReadRequest(client);
    if (request.empty())
    {
      closesocket(client);
      return;
    }

    std::size_t requestIndex;
    {
      std::lock_guard lock{m_mutex};
      requestIndex = m_requests.size();
      m_requests.emplace_back(request);
    }
    m_requestChanged.notify_all();

    const HttpReply reply = m_replyFactory(requestIndex, request);
    SendAll(client, reply.first);
    if (reply.waitBetweenParts)
    {
      std::unique_lock lock{m_releaseMutex};
      m_releaseChanged.wait(lock, [this]
                            { return m_released || m_stop.load(std::memory_order_acquire); });
    }
    SendAll(client, reply.second);
    closesocket(client);
  }

  ReplyFactory m_replyFactory;
  SOCKET m_listenSocket{INVALID_SOCKET};
  uint16_t m_port{0};
  std::atomic_bool m_stop{false};
  std::thread m_acceptThread;
  std::vector<std::thread> m_clientThreads;

  mutable std::mutex m_mutex;
  std::condition_variable m_requestChanged;
  std::vector<std::string> m_requests;

  std::mutex m_releaseMutex;
  std::condition_variable m_releaseChanged;
  bool m_released{false};
};

constexpr std::string_view OK_RESPONSE =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Encoding: identity\r\n"
    "Accept-Ranges: none\r\nX-Curl-Test: visible\r\nConnection: close\r\n\r\nOK";

TEST(TestCurlFileRedaction, RedactsSensitiveNamesCaseInsensitively)
{
  bool continuation = false;
  for (const std::string_view name :
       {"Authorization", "proxy-authorization", "COOKIE", "Set-Cookie", "X-Api-Key", "X-Auth-Token",
        "X-Amz-Credential", "X-Signature", "Location", "Referer"})
  {
    const std::string secret = std::string{name} + ": do-not-log";
    const std::string redacted = XFILE::RedactCurlHeaderForLogging(secret, continuation);
    EXPECT_EQ(redacted, std::string{name} + ": [REDACTED]");
    EXPECT_EQ(redacted.find("do-not-log"), std::string::npos);
    continuation = false;
  }
}

TEST(TestCurlFileRedaction, RedactsFoldedSensitiveValuesOnly)
{
  bool continuation = false;
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging("Cookie: first-secret", continuation),
            "Cookie: [REDACTED]");
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging("\tsecond-secret", continuation), "\t[REDACTED]");
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging("X-Safe: visible", continuation), "X-Safe: visible");
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging(" continuation-visible", continuation),
            " continuation-visible");
}

TEST(TestCurlFileRedaction, PreservesRequestStatusAndSafeHeaders)
{
  bool continuation = false;
  EXPECT_EQ(
      XFILE::RedactCurlHeaderForLogging("GET /private/path?token=secret HTTP/1.1", continuation),
      "GET [REDACTED] HTTP/1.1");
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging("HTTP/1.1 206 Partial Content", continuation),
            "HTTP/1.1 206 Partial Content");
  EXPECT_EQ(XFILE::RedactCurlHeaderForLogging("Content-Type: text/plain", continuation),
            "Content-Type: text/plain");
}

TEST(TestCurlFileRedaction, RemovesRoutesQueriesCredentialsAndProtocolOptions)
{
  const std::string redacted = XFILE::RedactCurlUrlForLogging(CURL{
      "https://user:password@example.test/private/capability?token=secret|Authorization=bearer"});
  EXPECT_NE(redacted.find("https://example.test/"), std::string::npos);
  EXPECT_NE(redacted.find("[REDACTED]"), std::string::npos);
  EXPECT_EQ(redacted.find("user"), std::string::npos);
  EXPECT_EQ(redacted.find("password"), std::string::npos);
  EXPECT_EQ(redacted.find("private"), std::string::npos);
  EXPECT_EQ(redacted.find("capability"), std::string::npos);
  EXPECT_EQ(redacted.find("token"), std::string::npos);
  EXPECT_EQ(redacted.find("Authorization"), std::string::npos);
  EXPECT_EQ(redacted.find("secret"), std::string::npos);
}

TEST(TestCurlFileCancellation, CancelsBeforeHeadersAndReusesAfterOwnerReset)
{
  CLoopbackHttpServer server(
      [](std::size_t index, const std::string&)
      {
        if (index == 0)
          return HttpReply{"", std::string{OK_RESPONSE}, true};
        return HttpReply{std::string{OK_RESPONSE}, "", false};
      });

  CCurlFile curl;
  curl.SetRetry(false);
  curl.SetTotalTimeout(2);
  bool firstOpened = true;
  bool secondOpened = false;
  std::string secondBody;
  std::promise<void> firstFinished;
  std::promise<void> ownerFinished;
  auto firstFuture = firstFinished.get_future();
  auto ownerFuture = ownerFinished.get_future();
  std::thread owner(
      [&]
      {
        firstOpened = curl.Open(CURL{server.Url("/blocked-headers")});
        curl.Close();
        firstFinished.set_value();

        curl.Reset();
        secondOpened = curl.Open(CURL{server.Url("/reuse")});
        if (secondOpened)
          curl.ReadData(secondBody);
        curl.Close();
        ownerFinished.set_value();
      });

  const bool firstRequestArrived = server.WaitForRequests(1, 2s);
  const auto cancelStarted = std::chrono::steady_clock::now();
  curl.Cancel();
  const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStarted;
  const bool firstCompleted = firstFuture.wait_for(1s) == std::future_status::ready;
  const bool secondRequestArrived = server.WaitForRequests(2, 2s);
  bool ownerCompleted = ownerFuture.wait_for(2s) == std::future_status::ready;
  server.Release();
  if (!ownerCompleted)
    ownerCompleted = ownerFuture.wait_for(3s) == std::future_status::ready;
  owner.join();

  EXPECT_TRUE(firstRequestArrived);
  EXPECT_LT(cancelElapsed, 100ms);
  EXPECT_TRUE(firstCompleted);
  EXPECT_TRUE(secondRequestArrived);
  EXPECT_TRUE(ownerCompleted);
  EXPECT_FALSE(firstOpened);
  EXPECT_TRUE(secondOpened);
  EXPECT_EQ(secondBody, "OK");
}

TEST(TestCurlFileCancellation, CancelsWhileBodyIsBlockedWithBoundedJoin)
{
  CLoopbackHttpServer server(
      [](std::size_t, const std::string&)
      {
        return HttpReply{"HTTP/1.1 200 OK\r\nContent-Length: 16\r\nAccept-Ranges: "
                         "none\r\nConnection: close\r\n\r\n"
                         "12345678",
                         "abcdefgh", true};
      });

  CCurlFile curl;
  curl.SetRetry(false);
  curl.SetTotalTimeout(2);
  bool opened = false;
  ssize_t firstRead = -1;
  ssize_t blockedRead = 1;
  std::promise<void> blockingReadStarted;
  std::promise<void> ownerFinished;
  auto blockingFuture = blockingReadStarted.get_future();
  auto ownerFuture = ownerFinished.get_future();
  std::thread owner(
      [&]
      {
        opened = curl.Open(CURL{server.Url("/blocked-body")});
        char buffer[8];
        if (opened)
        {
          firstRead = 0;
          while (firstRead < static_cast<ssize_t>(sizeof(buffer)))
          {
            const ssize_t read = curl.Read(buffer + firstRead, sizeof(buffer) - firstRead);
            if (read <= 0)
              break;
            firstRead += read;
          }
        }
        blockingReadStarted.set_value();
        if (opened)
          blockedRead = curl.Read(buffer, sizeof(buffer));
        curl.Close();
        ownerFinished.set_value();
      });

  const bool requestArrived = server.WaitForRequests(1, 2s);
  const bool readStarted = blockingFuture.wait_for(2s) == std::future_status::ready;
  const auto cancelStarted = std::chrono::steady_clock::now();
  curl.Cancel();
  bool ownerCompleted = ownerFuture.wait_for(1s) == std::future_status::ready;
  const auto joinElapsed = std::chrono::steady_clock::now() - cancelStarted;
  server.Release();
  if (!ownerCompleted)
    ownerCompleted = ownerFuture.wait_for(3s) == std::future_status::ready;
  owner.join();

  EXPECT_TRUE(requestArrived);
  EXPECT_TRUE(readStarted);
  EXPECT_TRUE(ownerCompleted);
  EXPECT_LT(joinElapsed, 1s);
  EXPECT_TRUE(opened);
  EXPECT_EQ(firstRead, 8);
  EXPECT_LE(blockedRead, 0);
}

TEST(TestCurlFileRetry, StrictNoRetryMakesExactlyOneRequest)
{
  CLoopbackHttpServer server(
      [](std::size_t, const std::string&)
      {
        return HttpReply{"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n",
                         "", false};
      });

  CCurlFile curl;
  curl.SetRetry(false);
  curl.SetTotalTimeout(2);
  EXPECT_FALSE(curl.Open(CURL{server.Url("/no-retry")}));
  curl.Close();
  EXPECT_TRUE(server.WaitForRequests(1, 2s));
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(server.RequestCount(), 1U);
}

TEST(TestCurlFileRetry, DefaultPolicyPreservesInitialRangeFallback)
{
  CLoopbackHttpServer server(
      [](std::size_t index, const std::string&)
      {
        if (index == 0)
        {
          return HttpReply{"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n",
                           "", false};
        }
        return HttpReply{std::string{OK_RESPONSE}, "", false};
      });

  CCurlFile curl;
  curl.SetTotalTimeout(2);
  ASSERT_TRUE(curl.Open(CURL{server.Url("/default-range-retry")}));
  std::string body;
  EXPECT_TRUE(curl.ReadData(body));
  EXPECT_EQ(body, "OK");
  curl.Close();

  ASSERT_TRUE(server.WaitForRequests(2, 2s));
  EXPECT_EQ(server.RequestCount(), 2U);
}

TEST(TestCurlFileRetry, StrictNoRetrySuppressesExistsGetFallback)
{
  CLoopbackHttpServer server(
      [](std::size_t, const std::string&)
      {
        return HttpReply{"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n",
                         "", false};
      });

  CCurlFile curl;
  curl.SetRetry(false);
  curl.SetTotalTimeout(2);
  EXPECT_FALSE(curl.Exists(CURL{server.Url("/no-head-fallback")}));
  curl.Close();

  EXPECT_TRUE(server.WaitForRequests(1, 2s));
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(server.RequestCount(), 1U);
}

TEST(TestCurlFileControls, PreservesIdentityRangeAndResponseHeaders)
{
  CLoopbackHttpServer server([](std::size_t, const std::string&)
                             { return HttpReply{std::string{OK_RESPONSE}, "", false}; });

  CCurlFile curl;
  curl.SetRetry(false);
  curl.SetAcceptEncoding("identity");
  curl.SetRequestHeader("Range", "");
  ASSERT_TRUE(curl.Open(CURL{server.Url("/controls")}));
  std::string body;
  EXPECT_TRUE(curl.ReadData(body));
  EXPECT_EQ(body, "OK");
  EXPECT_EQ(curl.GetProperty(XFILE::FileProperty::RESPONSE_PROTOCOL), "HTTP/1.1 200 OK");
  EXPECT_EQ(curl.GetProperty(XFILE::FileProperty::RESPONSE_HEADER, "X-Curl-Test"), "visible");
  curl.Close();

  ASSERT_TRUE(server.WaitForRequests(1, 2s));
  std::string request = server.Request(0);
  std::transform(request.begin(), request.end(), request.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  EXPECT_NE(request.find("\r\naccept-encoding: identity\r\n"), std::string::npos);
  EXPECT_EQ(request.find("\r\nrange:"), std::string::npos) << request;
}
} // namespace
