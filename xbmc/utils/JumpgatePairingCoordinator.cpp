/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePairingCoordinator.h"

#include "JSONVariantParser.h"
#include "JSONVariantWriter.h"
#include "Variant.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
class CSystemJumpgatePairingClock final : public IJumpgatePairingClock
{
public:
  TimePoint Now() const override { return std::chrono::steady_clock::now(); }

  bool WaitFor(std::chrono::milliseconds duration,
               const std::function<bool()>& interrupted) override
  {
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, duration, interrupted);
  }

  void Wake() override { m_condition.notify_all(); }

private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
};

std::string GetString(const CVariant& value, const char* snakeCase, const char* camelCase)
{
  if (value.isMember(snakeCase) && value[snakeCase].isString())
    return value[snakeCase].asString();
  if (value.isMember(camelCase) && value[camelCase].isString())
    return value[camelCase].asString();
  return {};
}

int GetPositiveInt(const CVariant& value, const char* snakeCase, const char* camelCase)
{
  std::int64_t parsed = 0;
  if (value.isMember(snakeCase))
    parsed = value[snakeCase].asInteger(0);
  if (parsed <= 0 && value.isMember(camelCase))
    parsed = value[camelCase].asInteger(0);
  if (parsed <= 0 || parsed > std::numeric_limits<int>::max())
    return 0;
  return static_cast<int>(parsed);
}

std::string GetError(const CVariant& value, const std::string& fallback)
{
  std::string error = GetString(value, "error", "error");
  if (error.empty())
    error = GetString(value, "message", "message");
  return error.empty() ? fallback : error;
}

bool ParseObject(const std::string& body, CVariant& value)
{
  return CJSONVariantParser::Parse(body, value) && value.isObject();
}

bool IsTransientStatus(int statusCode)
{
  return statusCode == 0 || statusCode == 408 || statusCode == 425 || statusCode == 429 ||
         statusCode >= 500;
}
} // namespace

CJumpgatePairingCoordinator::CJumpgatePairingCoordinator(
    std::shared_ptr<IJumpgatePairingTransport> transport,
    std::shared_ptr<IJumpgatePairingClock> clock)
  : m_transport(std::move(transport)),
    m_clock(clock ? std::move(clock) : std::make_shared<CSystemJumpgatePairingClock>())
{
  if (!m_transport)
    throw std::invalid_argument("pairing transport is required");
}

CJumpgatePairingCoordinator::~CJumpgatePairingCoordinator()
{
  Stop(true);
  ReleasePendingQrArtifacts();
}

bool CJumpgatePairingCoordinator::IsActiveStage(JumpgatePairingStage stage)
{
  return stage == JumpgatePairingStage::Issuing ||
         stage == JumpgatePairingStage::AwaitingActivation ||
         stage == JumpgatePairingStage::Applying;
}

void CJumpgatePairingCoordinator::ClearSensitiveString(std::string& value)
{
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
}

bool CJumpgatePairingCoordinator::Start(JumpgatePairingRequest request,
                                        JumpgatePairingRedemptionHandler redemptionHandler,
                                        JumpgatePairingQrRenderer qrRenderer,
                                        JumpgatePairingQrCleanup qrCleanup)
{
  if (request.bridgeOrigin.empty() || !redemptionHandler || !qrRenderer || !qrCleanup)
    return false;

  {
    std::lock_guard lock(m_stateMutex);
    if (IsActiveStage(m_snapshot.stage))
      return false;
  }

  Stop(true);
  std::lock_guard lifecycleLock(m_lifecycleMutex);
  m_stopRequested.store(false, std::memory_order_release);
  {
    std::lock_guard lock(m_stateMutex);
    m_request = std::move(request);
    m_redemptionHandler = std::move(redemptionHandler);
    m_qrRenderer = std::move(qrRenderer);
    m_qrCleanup = std::move(qrCleanup);
    m_deadline = {};
    m_snapshot = {};
    m_snapshot.stage = JumpgatePairingStage::Issuing;
    m_snapshot.status = "Requesting a secure pairing code...";
    m_snapshot.canCancel = true;
    ++m_snapshot.revision;
  }

  m_worker = std::thread(&CJumpgatePairingCoordinator::Run, this);
  return true;
}

bool CJumpgatePairingCoordinator::Restart()
{
  JumpgatePairingRequest request;
  JumpgatePairingRedemptionHandler redemptionHandler;
  JumpgatePairingQrRenderer qrRenderer;
  JumpgatePairingQrCleanup qrCleanup;
  {
    std::lock_guard lock(m_stateMutex);
    if (IsActiveStage(m_snapshot.stage) || m_snapshot.stage == JumpgatePairingStage::Applied)
      return false;
    request = m_request;
    redemptionHandler = m_redemptionHandler;
    qrRenderer = m_qrRenderer;
    qrCleanup = m_qrCleanup;
  }
  return Start(std::move(request), std::move(redemptionHandler), std::move(qrRenderer),
               std::move(qrCleanup));
}

void CJumpgatePairingCoordinator::Cancel()
{
  CancelInternal(false);
}

void CJumpgatePairingCoordinator::CancelInternal(bool cancelApplying)
{
  m_stopRequested.store(true, std::memory_order_release);
  m_transport->Cancel();
  m_clock->Wake();

  std::lock_guard deliveryLock(m_redemptionDeliveryMutex);
  {
    std::lock_guard lock(m_stateMutex);
    if (m_snapshot.stage == JumpgatePairingStage::Issuing ||
        m_snapshot.stage == JumpgatePairingStage::AwaitingActivation ||
        (cancelApplying && m_snapshot.stage == JumpgatePairingStage::Applying))
    {
      DeferQrCleanupLocked(ClearPresentationLocked());
      m_snapshot.stage = JumpgatePairingStage::Cancelled;
      m_snapshot.status = "Pairing cancelled";
      m_snapshot.canCancel = false;
      m_snapshot.canRetry = true;
      ++m_snapshot.revision;
    }
  }
}

void CJumpgatePairingCoordinator::Stop(bool waitForCompletion)
{
  std::unique_lock lifecycleLock(m_lifecycleMutex);
  CancelInternal(true);
  std::thread worker;
  if (waitForCompletion && m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id())
    worker = std::move(m_worker);
  lifecycleLock.unlock();
  if (worker.joinable())
    worker.join();
}

void CJumpgatePairingCoordinator::CompleteApply(bool committed, const std::string& message)
{
  {
    std::lock_guard lock(m_stateMutex);
    if (m_snapshot.stage != JumpgatePairingStage::Applying)
      return;
    DeferQrCleanupLocked(ClearPresentationLocked());
    m_snapshot.stage = committed ? JumpgatePairingStage::Applied : JumpgatePairingStage::Failed;
    m_snapshot.status = committed ? "Paired and applied"
                                  : (message.empty() ? "Pairing could not be applied" : message);
    m_snapshot.canCancel = false;
    m_snapshot.canRetry = !committed;
    ++m_snapshot.revision;
  }
}

void CJumpgatePairingCoordinator::ReleaseQrArtifact(const std::string& path)
{
  PendingQrCleanup pending;
  {
    std::lock_guard lock(m_stateMutex);
    const auto cleanup =
        std::find_if(m_pendingQrCleanup.begin(), m_pendingQrCleanup.end(),
                     [&path](const PendingQrCleanup& candidate) { return candidate.path == path; });
    if (cleanup == m_pendingQrCleanup.end())
      return;
    pending = std::move(*cleanup);
    m_pendingQrCleanup.erase(cleanup);
  }
  if (!pending.path.empty() && pending.cleanup)
    pending.cleanup(pending.path);
}

void CJumpgatePairingCoordinator::ReleasePendingQrArtifacts()
{
  std::vector<PendingQrCleanup> pending;
  {
    std::lock_guard lock(m_stateMutex);
    pending.swap(m_pendingQrCleanup);
  }
  for (const auto& artifact : pending)
  {
    if (!artifact.path.empty() && artifact.cleanup)
      artifact.cleanup(artifact.path);
  }
}

JumpgatePairingSnapshot CJumpgatePairingCoordinator::GetSnapshot() const
{
  std::lock_guard lock(m_stateMutex);
  JumpgatePairingSnapshot snapshot = m_snapshot;
  if (snapshot.stage == JumpgatePairingStage::AwaitingActivation && snapshot.totalSeconds > 0)
  {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - m_clock->Now());
    snapshot.remainingSeconds =
        remaining.count() <= 0 ? 0 : static_cast<int>((remaining.count() + 999) / 1000);
    snapshot.remainingPercent =
        std::clamp((snapshot.remainingSeconds * 100) / snapshot.totalSeconds, 0, 100);
  }
  return snapshot;
}

bool CJumpgatePairingCoordinator::WaitForNextPoll(IJumpgatePairingClock::TimePoint deadline,
                                                  std::chrono::milliseconds delay)
{
  const auto now = m_clock->Now();
  if (now >= deadline)
    return false;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  const auto boundedDelay = std::min(delay, remaining);
  if (m_clock->WaitFor(boundedDelay,
                       [this] { return m_stopRequested.load(std::memory_order_acquire); }))
    return false;
  return !m_stopRequested.load(std::memory_order_acquire) && m_clock->Now() < deadline;
}

void CJumpgatePairingCoordinator::Run()
{
  JumpgatePairingRequest request;
  JumpgatePairingRedemptionHandler redemptionHandler;
  JumpgatePairingQrRenderer qrRenderer;
  {
    std::lock_guard lock(m_stateMutex);
    request = m_request;
    redemptionHandler = m_redemptionHandler;
    qrRenderer = m_qrRenderer;
  }

  CVariant issueBody(CVariant::VariantTypeObject);
  issueBody["deviceName"] = request.deviceName;
  std::string issueBodyJson;
  if (!CJSONVariantWriter::Write(issueBody, issueBodyJson, true))
  {
    SetTerminal(JumpgatePairingStage::Failed, "Pairing request could not be prepared");
    return;
  }

  JumpgatePairingHttpResponse issue =
      m_transport->Post(request.bridgeOrigin + "/pair/device/code", issueBodyJson,
                        m_clock->Now() + std::chrono::seconds(10),
                        [this] { return m_stopRequested.load(std::memory_order_acquire); });
  ClearSensitiveString(issueBodyJson);
  if (m_stopRequested.load(std::memory_order_acquire))
  {
    ClearSensitiveString(issue.body);
    return;
  }

  CVariant issueData;
  const bool issueParsed = ParseObject(issue.body, issueData);
  if (!issue.completed || issue.statusCode < 200 || issue.statusCode >= 300)
  {
    const std::string message = issueParsed ? GetError(issueData, "Bridge temporarily unavailable")
                                            : "Unable to reach the Jumpgate Bridge";
    ClearSensitiveString(issue.body);
    SetTerminal(JumpgatePairingStage::Failed, message);
    return;
  }
  ClearSensitiveString(issue.body);
  if (!issueParsed || (issueData.isMember("ok") && !issueData["ok"].asBoolean()))
  {
    SetTerminal(JumpgatePairingStage::Failed, issueParsed
                                                  ? GetError(issueData, "Bridge rejected pairing")
                                                  : "Bridge returned an invalid pairing response");
    return;
  }

  std::string userCode = GetString(issueData, "user_code", "userCode");
  std::string deviceCode = GetString(issueData, "device_code", "deviceCode");
  std::string verificationUrl = GetString(issueData, "verification_url", "verificationUrl");
  std::string verificationUrlWithCode =
      GetString(issueData, "verification_short_url", "verificationShortUrl");
  if (verificationUrlWithCode.empty())
    verificationUrlWithCode =
        GetString(issueData, "verification_url_with_code", "verificationUrlWithCode");
  const int parsedExpiresIn = GetPositiveInt(issueData, "expires_in", "expiresIn");
  const int expiresIn = std::clamp(parsedExpiresIn > 0 ? parsedExpiresIn : 600, 1, 3600);
  const int parsedPollInterval = GetPositiveInt(issueData, "interval", "interval");
  const int pollInterval = std::clamp(parsedPollInterval > 0 ? parsedPollInterval : 2, 1, 30);
  if (userCode.empty() || deviceCode.empty() || verificationUrl.empty() ||
      verificationUrlWithCode.empty())
  {
    ClearSensitiveString(deviceCode);
    SetTerminal(JumpgatePairingStage::Failed, "Bridge returned an incomplete pairing code");
    return;
  }

  const auto deadline = m_clock->Now() + std::chrono::seconds(expiresIn);
  const std::string qrPath = qrRenderer(verificationUrlWithCode);
  ClearSensitiveString(verificationUrlWithCode);
  bool published = false;
  {
    std::lock_guard lock(m_stateMutex);
    if (!m_stopRequested.load(std::memory_order_acquire) &&
        m_snapshot.stage == JumpgatePairingStage::Issuing)
    {
      m_deadline = deadline;
      m_snapshot.stage = JumpgatePairingStage::AwaitingActivation;
      m_snapshot.userCode = userCode;
      m_snapshot.verificationUrl = verificationUrl;
      m_snapshot.qrImagePath = qrPath;
      m_snapshot.status = qrPath.empty() ? "Scan unavailable; enter the code on your phone"
                                         : "Scan the QR code or enter the code on your phone";
      m_snapshot.totalSeconds = expiresIn;
      m_snapshot.remainingSeconds = expiresIn;
      m_snapshot.remainingPercent = 100;
      m_snapshot.canCancel = true;
      m_snapshot.canRetry = false;
      ++m_snapshot.revision;
      published = true;
    }
  }
  ClearSensitiveString(userCode);
  if (!published)
  {
    ClearSensitiveString(deviceCode);
    CleanupQr(qrPath);
    return;
  }

  std::chrono::milliseconds nextDelay = std::chrono::seconds(pollInterval);
  int transientFailures = 0;
  while (WaitForNextPoll(deadline, nextDelay))
  {
    CVariant pollBody(CVariant::VariantTypeObject);
    pollBody["device_code"] = deviceCode;
    std::string pollBodyJson;
    if (!CJSONVariantWriter::Write(pollBody, pollBodyJson, true))
    {
      ClearSensitiveString(deviceCode);
      SetTerminal(JumpgatePairingStage::Failed, "Pairing request could not be prepared");
      return;
    }

    JumpgatePairingHttpResponse response =
        m_transport->Post(request.bridgeOrigin + "/pair/device/token", pollBodyJson, deadline,
                          [this] { return m_stopRequested.load(std::memory_order_acquire); });
    ClearSensitiveString(pollBodyJson);
    if (m_stopRequested.load(std::memory_order_acquire))
    {
      ClearSensitiveString(response.body);
      ClearSensitiveString(deviceCode);
      return;
    }
    if (m_clock->Now() >= deadline)
    {
      ClearSensitiveString(response.body);
      ClearSensitiveString(deviceCode);
      SetTerminal(JumpgatePairingStage::Expired, "Pairing code expired. Request a new code.");
      return;
    }

    CVariant data;
    const bool parsed = ParseObject(response.body, data);
    const int retryAfter =
        response.retryAfterSeconds > 0 ? response.retryAfterSeconds : pollInterval;
    if (!response.completed || response.statusCode < 200 || response.statusCode >= 300)
    {
      if (IsTransientStatus(response.statusCode) && m_clock->Now() < deadline)
      {
        ++transientFailures;
        nextDelay = std::chrono::seconds(
            response.statusCode == 429
                ? std::clamp(retryAfter, 1, 30)
                : std::min(8, pollInterval * (1 << std::min(2, transientFailures - 1))));
        bool updateAccepted = false;
        {
          std::lock_guard lock(m_stateMutex);
          if (!m_stopRequested.load(std::memory_order_acquire) &&
              m_snapshot.stage == JumpgatePairingStage::AwaitingActivation)
          {
            m_snapshot.status = response.statusCode == 429
                                    ? "Bridge is busy; slowing down safely"
                                    : "Bridge connection interrupted; retrying";
            ++m_snapshot.revision;
            updateAccepted = true;
          }
        }
        ClearSensitiveString(response.body);
        if (!updateAccepted)
        {
          ClearSensitiveString(deviceCode);
          return;
        }
        continue;
      }
      const std::string error =
          parsed ? GetError(data, "Pairing was rejected") : "Pairing was rejected by the Bridge";
      ClearSensitiveString(response.body);
      ClearSensitiveString(deviceCode);
      SetTerminal(response.statusCode == 410 ? JumpgatePairingStage::Expired
                                             : JumpgatePairingStage::Failed,
                  error);
      return;
    }

    if (!parsed)
    {
      ClearSensitiveString(response.body);
      if (++transientFailures <= 3)
      {
        nextDelay = std::chrono::seconds(pollInterval);
        continue;
      }
      ClearSensitiveString(deviceCode);
      SetTerminal(JumpgatePairingStage::Failed, "Bridge returned an invalid pairing response");
      return;
    }
    transientFailures = 0;
    nextDelay = std::chrono::seconds(pollInterval);

    if (data.isMember("ok") && !data["ok"].asBoolean())
    {
      const std::string error = GetError(data, "Pairing failed");
      ClearSensitiveString(response.body);
      ClearSensitiveString(deviceCode);
      SetTerminal(JumpgatePairingStage::Failed, error);
      return;
    }

    const bool paired = data.isMember("paired") && data["paired"].asBoolean();
    if (!paired)
    {
      bool updateAccepted = false;
      {
        std::lock_guard lock(m_stateMutex);
        if (!m_stopRequested.load(std::memory_order_acquire) &&
            m_snapshot.stage == JumpgatePairingStage::AwaitingActivation)
        {
          m_snapshot.status = "Waiting for confirmation in your browser...";
          ++m_snapshot.revision;
          updateAccepted = true;
        }
      }
      ClearSensitiveString(response.body);
      if (!updateAccepted)
      {
        ClearSensitiveString(deviceCode);
        return;
      }
      continue;
    }

    std::string profileName = GetString(data, "name", "name");
    if (profileName.empty())
      profileName = GetString(data, "profile_name", "profileName");
    {
      std::lock_guard deliveryLock(m_redemptionDeliveryMutex);
      if (m_stopRequested.load(std::memory_order_acquire))
      {
        ClearSensitiveString(response.body);
        ClearSensitiveString(deviceCode);
        return;
      }
      {
        std::lock_guard lock(m_stateMutex);
        if (m_snapshot.stage != JumpgatePairingStage::AwaitingActivation)
        {
          ClearSensitiveString(response.body);
          ClearSensitiveString(deviceCode);
          return;
        }
        m_snapshot.stage = JumpgatePairingStage::Applying;
        m_snapshot.status = "Securing this profile on your device...";
        m_snapshot.canCancel = false;
        m_snapshot.canRetry = false;
        ++m_snapshot.revision;
      }
      ClearSensitiveString(deviceCode);
      redemptionHandler(std::move(response.body), request.bridgeOrigin, profileName);
    }
    ClearSensitiveString(response.body);
    return;
  }

  ClearSensitiveString(deviceCode);
  if (!m_stopRequested.load(std::memory_order_acquire))
    SetTerminal(JumpgatePairingStage::Expired, "Pairing code expired. Request a new code.");
}

void CJumpgatePairingCoordinator::SetTerminal(JumpgatePairingStage stage, const std::string& status)
{
  {
    std::lock_guard lock(m_stateMutex);
    if (m_stopRequested.load(std::memory_order_acquire) ||
        (m_snapshot.stage != JumpgatePairingStage::Issuing &&
         m_snapshot.stage != JumpgatePairingStage::AwaitingActivation))
      return;
    DeferQrCleanupLocked(ClearPresentationLocked());
    m_snapshot.stage = stage;
    m_snapshot.status = status;
    m_snapshot.canCancel = false;
    m_snapshot.canRetry =
        stage == JumpgatePairingStage::Expired || stage == JumpgatePairingStage::Failed;
    ++m_snapshot.revision;
  }
}

std::string CJumpgatePairingCoordinator::ClearPresentationLocked()
{
  ClearSensitiveString(m_snapshot.userCode);
  m_snapshot.verificationUrl.clear();
  std::string qrPath;
  qrPath.swap(m_snapshot.qrImagePath);
  m_snapshot.remainingSeconds = 0;
  m_snapshot.totalSeconds = 0;
  m_snapshot.remainingPercent = 0;
  m_deadline = {};
  return qrPath;
}

void CJumpgatePairingCoordinator::DeferQrCleanupLocked(std::string path)
{
  if (!path.empty() && m_qrCleanup)
    m_pendingQrCleanup.push_back({std::move(path), m_qrCleanup});
}

void CJumpgatePairingCoordinator::CleanupQr(std::string path) const
{
  JumpgatePairingQrCleanup cleanup;
  {
    std::lock_guard lock(m_stateMutex);
    cleanup = m_qrCleanup;
  }
  if (!path.empty() && cleanup)
    cleanup(path);
}

} // namespace KODI::JUMPGATE
