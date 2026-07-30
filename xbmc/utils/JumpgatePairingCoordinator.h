/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace KODI::JUMPGATE
{

struct JumpgatePairingHttpResponse
{
  bool completed{false};
  int statusCode{0};
  int retryAfterSeconds{0};
  std::string body;
};

class IJumpgatePairingTransport
{
public:
  virtual ~IJumpgatePairingTransport() = default;
  virtual JumpgatePairingHttpResponse Post(const std::string& url,
                                           const std::string& body,
                                           std::chrono::steady_clock::time_point deadline,
                                           const std::function<bool()>& cancelled) = 0;
  virtual void Cancel() = 0;
};

class IJumpgatePairingClock
{
public:
  using TimePoint = std::chrono::steady_clock::time_point;

  virtual ~IJumpgatePairingClock() = default;
  virtual TimePoint Now() const = 0;
  virtual bool WaitFor(std::chrono::milliseconds duration,
                       const std::function<bool()>& interrupted) = 0;
  virtual void Wake() = 0;
};

enum class JumpgatePairingStage
{
  Idle,
  Issuing,
  AwaitingActivation,
  Applying,
  Applied,
  Expired,
  Failed,
  Cancelled,
};

struct JumpgatePairingSnapshot
{
  JumpgatePairingStage stage{JumpgatePairingStage::Idle};
  std::string userCode;
  std::string verificationUrl;
  std::string qrImagePath;
  std::string status;
  int remainingSeconds{0};
  int totalSeconds{0};
  int remainingPercent{0};
  bool canCancel{false};
  bool canRetry{false};
  std::uint64_t revision{0};
};

struct JumpgatePairingRequest
{
  std::string bridgeOrigin;
  std::string deviceName{"Jumpgate"};
};

using JumpgatePairingRedemptionHandler = std::function<void(
    std::string responseJson, const std::string& origin, const std::string& profileName)>;
using JumpgatePairingQrRenderer = std::function<std::string(const std::string& verificationUrl)>;
using JumpgatePairingQrCleanup = std::function<void(const std::string& qrImagePath)>;

class CJumpgatePairingCoordinator final
{
public:
  explicit CJumpgatePairingCoordinator(std::shared_ptr<IJumpgatePairingTransport> transport,
                                       std::shared_ptr<IJumpgatePairingClock> clock = {});
  ~CJumpgatePairingCoordinator();

  CJumpgatePairingCoordinator(const CJumpgatePairingCoordinator&) = delete;
  CJumpgatePairingCoordinator& operator=(const CJumpgatePairingCoordinator&) = delete;

  bool Start(JumpgatePairingRequest request,
             JumpgatePairingRedemptionHandler redemptionHandler,
             JumpgatePairingQrRenderer qrRenderer,
             JumpgatePairingQrCleanup qrCleanup);
  bool Restart();
  void Cancel();
  void Stop(bool waitForCompletion = true);
  void CompleteApply(bool committed, const std::string& message = {});
  void ReleaseQrArtifact(const std::string& path);
  void ReleasePendingQrArtifacts();

  JumpgatePairingSnapshot GetSnapshot() const;

private:
  static bool IsActiveStage(JumpgatePairingStage stage);
  static void ClearSensitiveString(std::string& value);
  void Run();
  bool WaitForNextPoll(IJumpgatePairingClock::TimePoint deadline, std::chrono::milliseconds delay);
  void CancelInternal(bool cancelApplying);
  void SetTerminal(JumpgatePairingStage stage, const std::string& status);
  std::string ClearPresentationLocked();
  void DeferQrCleanupLocked(std::string path);
  void CleanupQr(std::string path) const;

  struct PendingQrCleanup
  {
    std::string path;
    JumpgatePairingQrCleanup cleanup;
  };

  std::shared_ptr<IJumpgatePairingTransport> m_transport;
  std::shared_ptr<IJumpgatePairingClock> m_clock;

  mutable std::mutex m_stateMutex;
  JumpgatePairingSnapshot m_snapshot;
  IJumpgatePairingClock::TimePoint m_deadline{};
  JumpgatePairingRequest m_request;
  JumpgatePairingRedemptionHandler m_redemptionHandler;
  JumpgatePairingQrRenderer m_qrRenderer;
  JumpgatePairingQrCleanup m_qrCleanup;
  std::vector<PendingQrCleanup> m_pendingQrCleanup;

  std::mutex m_lifecycleMutex;
  std::mutex m_redemptionDeliveryMutex;
  std::thread m_worker;
  std::atomic<bool> m_stopRequested{false};
};

} // namespace KODI::JUMPGATE
