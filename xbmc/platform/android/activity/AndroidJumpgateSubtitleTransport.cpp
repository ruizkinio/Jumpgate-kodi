/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AndroidJumpgateSubtitleTransport.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/CurlFile.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/HttpHeader.h"
#include "utils/ScopeGuard.h"
#include "utils/log.h"

#include <array>
#include <charconv>
#include <chrono>
#include <string_view>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_RESPONSE_BYTES = ANDROID_JUMPGATE_MAX_PART_BYTES;
constexpr std::size_t MAX_URL_BYTES = 2048;
constexpr std::size_t MAX_PROTOCOL_LINE_BYTES = 128;
constexpr std::size_t MAX_CONTENT_TYPE_BYTES = 128;
constexpr std::size_t MAX_RETRY_AFTER_BYTES = 32;
constexpr std::size_t MAX_REDIRECT_BYTES = 1024;
constexpr std::size_t MAX_CONTENT_ENCODING_BYTES = 32;
constexpr std::size_t MAX_ACCEPT_RANGES_BYTES = 32;
constexpr int CONNECT_TIMEOUT_SECONDS = 3;
constexpr int TOTAL_TIMEOUT_SECONDS = 10;
constexpr auto TEMP_ROOT = "special://temp/jumpgate-subtitles/";

int ParseHttpStatus(std::string_view protocolLine);
std::optional<std::uint64_t> ParseContentLength(std::string_view value);
std::string BoundedHeaderValue(const CHttpHeader& headers, const char* name, std::size_t maximum);

class CBoundedSubtitleCurlFile final : public XFILE::CCurlFile
{
public:
  void SetPostBody(const std::string& body)
  {
    m_postdata = body;
    m_postdataset = true;
  }

  void ClearSensitiveState()
  {
    std::fill(m_postdata.begin(), m_postdata.end(), '\0');
    m_postdata.clear();
    m_postdataset = false;
    for (auto& [name, value] : m_requestheaders)
    {
      static_cast<void>(name);
      std::fill(value.begin(), value.end(), '\0');
    }
    m_requestheaders.clear();
    std::fill(m_url.begin(), m_url.end(), '\0');
    m_url.clear();
  }
};

class CCurlSubtitleHttpExecutor final : public IAndroidJumpgateSubtitleHttpExecutor
{
public:
  bool Execute(const JumpgateSubtitleHttpRequest& request,
               JumpgateSubtitleHttpResponse& response,
               const CJumpgateSubtitleCancellationToken& cancellation) override
  {
    CBoundedSubtitleCurlFile curl;
    KODI::UTILS::CScopeGuard<CBoundedSubtitleCurlFile*, nullptr, void(CBoundedSubtitleCurlFile*)>
        activeRegistration{[this](CBoundedSubtitleCurlFile* active)
                           {
                             std::lock_guard lock(m_mutex);
                             if (m_activeCurl == active)
                               m_activeCurl = nullptr;
                           },
                           &curl};
    {
      std::lock_guard lock(m_mutex);
      if (m_activeCurl)
        return false;
      m_activeCurl = &curl;
    }

    bool readSucceeded = false;
    try
    {
      curl.SetRequestHeader("Authorization", request.authorization);
      if (request.method == JumpgateSubtitleHttpMethod::Post)
      {
        curl.SetRequestHeader("Content-Type", request.contentType);
        curl.SetPostBody(request.body);
      }
      curl.SetRetry(false);
      curl.SetAcceptEncoding("identity");
      curl.SetRequestHeader("Range", "");
      curl.SetTimeout(CONNECT_TIMEOUT_SECONDS);
      curl.SetTotalTimeout(TOTAL_TIMEOUT_SECONDS);
      curl.SetBufferSize(64 * 1024);

      CURL requestUrl{request.url};
      requestUrl.SetProtocolOption("redirect-limit", "0");
      requestUrl.SetProtocolOption("failonerror", "false");

      const bool opened = !cancellation.IsCancelled() && curl.Open(requestUrl);
      response.statusCode =
          ParseHttpStatus(curl.GetProperty(XFILE::FileProperty::RESPONSE_PROTOCOL));
      response.effectiveUrl = curl.GetProperty(XFILE::FileProperty::EFFECTIVE_URL);
      if (response.effectiveUrl.size() > MAX_URL_BYTES)
        response.effectiveUrl.clear();
      response.redirectUrl = curl.GetRedirectURL();
      if (response.redirectUrl.size() > MAX_REDIRECT_BYTES)
        response.redirectUrl = "<rejected>";

      const CHttpHeader& headers = curl.GetHttpHeader();
      response.contentType = BoundedHeaderValue(headers, "Content-Type", MAX_CONTENT_TYPE_BYTES);
      response.contentLength = ParseContentLength(headers.GetValue("Content-Length"));
      response.retryAfter = BoundedHeaderValue(headers, "Retry-After", MAX_RETRY_AFTER_BYTES);
      response.contentEncoding =
          BoundedHeaderValue(headers, "Content-Encoding", MAX_CONTENT_ENCODING_BYTES);
      response.acceptRanges = BoundedHeaderValue(headers, "Accept-Ranges", MAX_ACCEPT_RANGES_BYTES);

      readSucceeded = opened;
      if (opened)
      {
        std::array<std::uint8_t, 16 * 1024> buffer{};
        response.body.reserve(std::min<std::size_t>(request.maximumResponseBytes, 256 * 1024));
        while (response.body.size() < request.maximumResponseBytes && !cancellation.IsCancelled())
        {
          const std::size_t remaining = request.maximumResponseBytes - response.body.size();
          const ssize_t read = curl.Read(buffer.data(), std::min(buffer.size(), remaining));
          if (read < 0)
          {
            readSucceeded = false;
            break;
          }
          if (read == 0)
            break;
          response.body.insert(response.body.end(), buffer.begin(),
                               buffer.begin() + static_cast<std::size_t>(read));
        }

        if (readSucceeded && response.body.size() == request.maximumResponseBytes &&
            !cancellation.IsCancelled())
        {
          std::uint8_t overflowProbe = 0;
          const ssize_t overflowRead = curl.Read(&overflowProbe, 1);
          overflowProbe = 0;
          if (overflowRead != 0)
            readSucceeded = false;
        }
      }
    }
    catch (...)
    {
      readSucceeded = false;
    }
    curl.Close();
    curl.ClearSensitiveState();
    return readSucceeded && !cancellation.IsCancelled();
  }

  bool SupportsSafeConcurrentCancellation() const noexcept override { return true; }

  void RequestSafeCancellation() override
  {
    std::lock_guard lock(m_mutex);
    if (m_activeCurl)
      m_activeCurl->Cancel();
  }

private:
  mutable std::mutex m_mutex;
  CBoundedSubtitleCurlFile* m_activeCurl{nullptr};
};

bool IsDeviceBearer(std::string_view authorization)
{
  constexpr std::string_view prefix{"Bearer "};
  if (!authorization.starts_with(prefix))
    return false;
  authorization.remove_prefix(prefix.size());
  if (authorization.size() < 32 || authorization.size() > 128)
    return false;
  return std::all_of(authorization.begin(), authorization.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '_' || item == '-';
                     });
}

bool IsRequestForOrigin(const JumpgateSubtitleHttpRequest& request, const std::string& origin)
{
  if (request.url.size() <= origin.size() || request.url.size() > MAX_URL_BYTES ||
      request.url.compare(0, origin.size(), origin) != 0 || request.url[origin.size()] != '/' ||
      request.url.find_first_of("?#|") != std::string::npos)
  {
    return false;
  }

  const std::string_view path{request.url.data() + origin.size(),
                              request.url.size() - origin.size()};
  if (request.method == JumpgateSubtitleHttpMethod::Post)
    return path == "/v1/subtitles/discover" || path == "/v1/subtitles/resolve";
  return path.starts_with("/v1/subtitles/");
}

std::optional<std::pair<std::string, std::string>> DeliveryBudgetKeys(const std::string& url,
                                                                      const std::string& origin)
{
  constexpr std::string_view prefix{"/v1/subtitles/"};
  std::string_view path{url.data() + origin.size(), url.size() - origin.size()};
  if (!path.starts_with(prefix))
    return std::nullopt;
  path.remove_prefix(prefix.size());
  const std::size_t sessionEnd = path.find('/');
  const std::size_t artifactEnd =
      sessionEnd == std::string_view::npos ? sessionEnd : path.find('/', sessionEnd + 1);
  const std::size_t partEnd =
      artifactEnd == std::string_view::npos ? artifactEnd : path.find('/', artifactEnd + 1);
  if (sessionEnd == std::string_view::npos || artifactEnd == std::string_view::npos ||
      partEnd == std::string_view::npos || path.find('/', partEnd + 1) != std::string_view::npos ||
      sessionEnd == 0 || artifactEnd == sessionEnd + 1 || partEnd == artifactEnd + 1 ||
      partEnd + 1 == path.size())
  {
    return std::nullopt;
  }
  const std::string_view part = path.substr(artifactEnd + 1, partEnd - artifactEnd - 1);
  if (part != "1" && part != "2")
    return std::nullopt;
  return std::pair<std::string, std::string>{std::string{path.substr(0, artifactEnd)},
                                             std::string{path.substr(artifactEnd + 1)}};
}

int ParseHttpStatus(std::string_view protocolLine)
{
  if (protocolLine.empty() || protocolLine.size() > MAX_PROTOCOL_LINE_BYTES)
    return 0;
  const std::size_t separator = protocolLine.find(' ');
  if (separator == std::string_view::npos || separator + 4 > protocolLine.size())
    return 0;
  int status = 0;
  const auto [end, error] = std::from_chars(protocolLine.data() + separator + 1,
                                            protocolLine.data() + separator + 4, status);
  return error == std::errc{} && end == protocolLine.data() + separator + 4 ? status : 0;
}

std::optional<std::uint64_t> ParseContentLength(std::string_view value)
{
  if (value.empty() || value.size() > 20)
    return std::nullopt;
  std::uint64_t length = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
  if (error != std::errc{} || end != value.data() + value.size())
    return std::nullopt;
  return length;
}

std::string BoundedHeaderValue(const CHttpHeader& headers, const char* name, std::size_t maximum)
{
  std::string value = headers.GetValue(name);
  if (value.size() > maximum)
    value.clear();
  return value;
}

const char* SubtitleStatusName(JumpgateSubtitleResultStatus status)
{
  switch (status)
  {
    case JumpgateSubtitleResultStatus::Success:
      return "success";
    case JumpgateSubtitleResultStatus::NoMatch:
      return "no_match";
    case JumpgateSubtitleResultStatus::InvalidRequest:
      return "invalid_request";
    case JumpgateSubtitleResultStatus::RePairRequired:
      return "re_pair_required";
    case JumpgateSubtitleResultStatus::Stale:
      return "stale";
    case JumpgateSubtitleResultStatus::RetryableBusy:
      return "retryable_busy";
    case JumpgateSubtitleResultStatus::ProtocolFailure:
      return "protocol_failure";
    case JumpgateSubtitleResultStatus::SoftFailure:
      return "transport_failure";
    case JumpgateSubtitleResultStatus::HttpFailure:
      return "http_failure";
    case JumpgateSubtitleResultStatus::Cancelled:
      return "cancelled";
  }
  return "unknown";
}
} // namespace

CAndroidJumpgateSubtitleTransport::CAndroidJumpgateSubtitleTransport(
    std::string bridgeOrigin, std::shared_ptr<IAndroidJumpgateSubtitleHttpExecutor> executor)
  : m_bridgeOrigin(std::move(bridgeOrigin)),
    m_executor(executor ? std::move(executor) : std::make_shared<CCurlSubtitleHttpExecutor>())
{
  if (!CJumpgateSubtitleClient::IsCanonicalOrigin(m_bridgeOrigin))
    m_bridgeOrigin.clear();
}

bool CAndroidJumpgateSubtitleTransport::Perform(
    const JumpgateSubtitleHttpRequest& request,
    JumpgateSubtitleHttpResponse& response,
    const CJumpgateSubtitleCancellationToken& cancellation)
{
  response.ClearSensitive();
  response.statusCode = 0;
  response.contentType.clear();
  response.contentLength.reset();
  response.contentEncoding.clear();
  response.acceptRanges.clear();

  const bool post = request.method == JumpgateSubtitleHttpMethod::Post;
  if (m_bridgeOrigin.empty() || request.followRedirects || !IsDeviceBearer(request.authorization) ||
      !IsRequestForOrigin(request, m_bridgeOrigin) || request.maximumResponseBytes == 0 ||
      request.maximumResponseBytes > MAX_RESPONSE_BYTES ||
      (post && (request.contentType != "application/json" || request.body.size() > 2048)) ||
      (!post && (!request.contentType.empty() || !request.body.empty())) ||
      cancellation.IsCancelled())
  {
    return false;
  }

  if (post)
  {
    m_artifactBudget.Reset();
  }
  else
  {
    const auto keys = DeliveryBudgetKeys(request.url, m_bridgeOrigin);
    if (!keys || !m_artifactBudget.Reserve(keys->first, keys->second, request.maximumResponseBytes))
    {
      return false;
    }
  }

  const bool executed = m_executor && m_executor->Execute(request, response, cancellation);
  if (!executed || response.body.size() > request.maximumResponseBytes)
  {
    response.ClearSensitive();
    return false;
  }
  return true;
}

bool CAndroidJumpgateSubtitleTransport::RequestSafeCancellation()
{
  if (!m_executor || !m_executor->SupportsSafeConcurrentCancellation())
    return false;
  m_executor->RequestSafeCancellation();
  return true;
}

CAndroidJumpgateSubtitleController::CAndroidJumpgateSubtitleController()
  : CAndroidJumpgateSubtitleController(AndroidJumpgateSubtitleControllerDependencies{})
{
}

CAndroidJumpgateSubtitleController::CAndroidJumpgateSubtitleController(
    AndroidJumpgateSubtitleControllerDependencies dependencies)
  : m_fileStore(dependencies.fileStore ? std::move(dependencies.fileStore)
                                       : std::make_shared<CAndroidJumpgateSubtitleFileStore>(
                                             CSpecialProtocol::TranslatePath(TEMP_ROOT))),
    m_registry(dependencies.registry ? std::move(dependencies.registry)
                                     : CJumpgateThreadRegistry::Global()),
    m_transportFactory(dependencies.transportFactory ? std::move(dependencies.transportFactory)
                                                     : [](const std::string& origin)
                           { return std::make_shared<CAndroidJumpgateSubtitleTransport>(origin); }),
    m_subtitleInjector(std::move(dependencies.subtitleInjector)),
    m_rePairNotifier(std::move(dependencies.rePairNotifier)),
    m_failureLogger(std::move(dependencies.failureLogger)),
    m_restartTransitionBarrier(std::move(dependencies.restartTransitionBarrier))
{
  auto worker = std::make_shared<CAndroidJumpgateSubtitleStageWorker>(m_fileStore, m_registry);
  if (worker->IsOperational())
    m_stageWorker = std::move(worker);
}

CAndroidJumpgateSubtitleController::~CAndroidJumpgateSubtitleController()
{
  Stop(true);
}

void CAndroidJumpgateSubtitleController::SweepStartupOrphans()
{
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped || m_startupSweepComplete)
      return;
    m_startupSweepComplete = true;
    worker = m_stageWorker;
  }
  if (worker)
    worker->QueueStartupSweep();
}

bool CAndroidJumpgateSubtitleController::PrepareGeneration(std::uint64_t generation)
{
  std::optional<JumpgateSubtitleBinding> previous;
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped)
      return false;
    previous = m_lifecycle.CurrentBinding();
    if (!m_lifecycle.PrepareGeneration(generation))
      return false;
    if (previous && m_coordinator)
      m_coordinator->Cancel(*previous);
    worker = m_stageWorker;
  }
  if (previous && worker)
    worker->Cancel(*previous);
  return true;
}

bool CAndroidJumpgateSubtitleController::Queue(JumpgateSubtitleRequest request)
{
  std::unique_ptr<CJumpgateSubtitleCoordinator> replacedCoordinator;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped || !m_stageWorker || !m_lifecycle.Bind(request.binding))
      return false;
    if (m_coordinator && m_coordinatorOrigin != request.binding.bridgeOrigin)
    {
      replacedCoordinator = std::move(m_coordinator);
      m_coordinatorOrigin.clear();
    }
  }

  if (replacedCoordinator)
    replacedCoordinator->Stop();

  std::lock_guard lock(m_mutex);
  if (m_stopped || !m_lifecycle.IsCurrent(request.binding))
    return false;
  if (!m_coordinator)
  {
    m_coordinatorOrigin = request.binding.bridgeOrigin;
    const std::shared_ptr<CAndroidJumpgateSubtitleTransport> transport =
        m_transportFactory(m_coordinatorOrigin);
    if (!transport)
    {
      m_coordinatorOrigin.clear();
      return false;
    }
    JumpgateSubtitleCoordinatorOptions options;
    options.requestSafeTransportCancellation =
        [weakTransport = std::weak_ptr<CAndroidJumpgateSubtitleTransport>(transport)]
    {
      const std::shared_ptr<CAndroidJumpgateSubtitleTransport> current = weakTransport.lock();
      return current && current->RequestSafeCancellation();
    };
    m_coordinator =
        std::make_unique<CJumpgateSubtitleCoordinator>(transport, std::move(options), m_registry);
  }
  return m_coordinator->Queue(std::move(request));
}

void CAndroidJumpgateSubtitleController::MarkPlaybackReady(std::uint64_t generation)
{
  std::lock_guard lock(m_mutex);
  if (m_stopped)
    return;
  m_lifecycle.MarkPlaybackReady(generation);
}

void CAndroidJumpgateSubtitleController::Process(const JumpgateSubtitleBinding& current)
{
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  AndroidJumpgateSubtitleAction action;
  bool stagingQueued = true;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped || !m_lifecycle.IsCurrent(current))
      return;
    worker = m_stageWorker;
    if (m_coordinator)
    {
      std::optional<JumpgateSubtitleCompletion> completion = m_coordinator->TakeCompletion(current);
      if (completion)
      {
        const JumpgateSubtitleBinding completionBinding = completion->binding;
        const JumpgateSubtitleResultStatus completionStatus = completion->status;
        const int completionHttpStatus = completion->httpStatus;
        const bool needsStaging = completionStatus == JumpgateSubtitleResultStatus::Success ||
                                  !completion->artifact.parts.empty();
        if (needsStaging)
          stagingQueued = worker && worker->Queue(std::move(*completion));
        if (!needsStaging || stagingQueued)
        {
          if (completionStatus != JumpgateSubtitleResultStatus::Success)
            m_lifecycle.AcceptStatus(completionBinding, completionStatus, completionHttpStatus);
        }
        else
        {
          stagingQueued = m_coordinator->ReturnCompletion(std::move(*completion));
        }
      }
    }
    action = m_lifecycle.TakeAction(current);
  }

  if (!stagingQueued)
  {
    CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle staging was not queued");
  }

  if (action.binding && action.type == AndroidJumpgateSubtitleActionType::NotifyRePair)
  {
    if (m_rePairNotifier)
      m_rePairNotifier();
    else
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Warning, "Jumpgate",
          "Profile authorization expired; pair this profile again", 5000, true);
  }
  else if (action.binding && action.type == AndroidJumpgateSubtitleActionType::LogFailure)
  {
    if (m_failureLogger)
      m_failureLogger(action.status, action.httpStatus);
    else
      CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle workflow failed safely (status={}, http={})",
                SubtitleStatusName(action.status), action.httpStatus);
  }

  std::optional<AndroidJumpgateStageCompletion> staged;
  if (worker)
    staged = worker->TakeCompletion(current);
  if (staged)
  {
    if (!staged->artifact)
    {
      CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle staging failed safely");
    }
    else
    {
      std::string staleDirectory = staged->artifact->directory;
      bool accepted = false;
      {
        std::lock_guard lock(m_mutex);
        accepted = m_lifecycle.AcceptStaged(std::move(*staged->artifact));
      }
      if (accepted)
        staleDirectory.clear();
      else
        staged->artifact->anchor.reset();
      staged->artifact.reset();
      if (!staleDirectory.empty())
        QueueCleanup({std::move(staleDirectory)});
    }
  }

  std::function<void(const std::string&)> injector = m_subtitleInjector;
  if (!injector)
  {
    const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
    if (!appPlayer)
      return;
    injector = [appPlayer](const std::string& path) { appPlayer->AddSubtitle(path); };
  }

  std::optional<AndroidJumpgateStagedArtifact> injection;
  std::string injectionPath;
  {
    std::lock_guard lock(m_mutex);
    if (!m_stopped && m_lifecycle.IsPlaybackReady(current))
    {
      injection = m_lifecycle.TakeInjection(current);
      if (injection && injection->anchor && injection->anchor->Validate())
      {
        injectionPath = injection->anchor->InjectionPath(injection->injectionFileName);
        if (!injectionPath.empty())
          injector(injectionPath);
      }
      if (injection)
        injection->anchor.reset();
    }
  }
  if (injection && !injectionPath.empty())
  {
    CLog::Log(LOGINFO, "CXBMCApp: Bridge subtitle injected (language={}, format={})",
              injection->language, injection->format);
  }
  else if (injection)
  {
    CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle identity changed before injection");
  }
}

void CAndroidJumpgateSubtitleController::OnPlaybackTerminal(std::uint64_t generation)
{
  std::vector<std::string> cleanup;
  std::optional<JumpgateSubtitleBinding> binding;
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped)
      return;
    binding = m_lifecycle.CurrentBinding();
    if (binding && binding->generation == generation)
    {
      if (m_coordinator)
        m_coordinator->Cancel(*binding);
    }
    worker = m_stageWorker;
    cleanup = m_lifecycle.CommitTerminal(generation);
  }
  if (binding && binding->generation == generation && worker)
    worker->Cancel(*binding);
  QueueCleanup(std::move(cleanup));
}

std::size_t CAndroidJumpgateSubtitleController::PendingCleanupCount() const
{
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    worker = m_stageWorker;
  }
  return worker ? worker->PendingCleanupCount() : 0;
}

bool CAndroidJumpgateSubtitleController::WaitForCleanupIdle(std::chrono::milliseconds timeout) const
{
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    worker = m_stageWorker;
  }
  return !worker || worker->WaitForCleanupIdle(timeout);
}

bool CAndroidJumpgateSubtitleController::Restart(std::chrono::milliseconds timeout)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds{0});
  const auto remaining = [&deadline]
  {
    const auto now = std::chrono::steady_clock::now();
    return now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                          : std::chrono::milliseconds{0};
  };

  std::unique_ptr<CJumpgateSubtitleCoordinator> coordinator;
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> oldWorker;
  std::optional<JumpgateSubtitleBinding> binding;
  std::uint64_t transitionSerial = 0;
  {
    std::lock_guard lock(m_mutex);
    transitionSerial = ++m_transitionSerial;
    m_stopped = true;
    binding = m_lifecycle.CurrentBinding();
    if (binding && m_coordinator)
      m_coordinator->Cancel(*binding);
    coordinator = std::move(m_coordinator);
    oldWorker = std::move(m_stageWorker);
    m_coordinatorOrigin.clear();
  }
  if (binding && oldWorker)
    oldWorker->Cancel(*binding);
  if (coordinator)
    coordinator->Stop(remaining());
  if (oldWorker && m_restartTransitionBarrier)
    m_restartTransitionBarrier(transitionSerial);
  std::vector<std::string> cleanup;
  bool transitionCurrent = false;
  {
    std::lock_guard lock(m_mutex);
    transitionCurrent = m_transitionSerial == transitionSerial;
    if (transitionCurrent)
      cleanup = m_lifecycle.Shutdown(false);
  }
  if (transitionCurrent)
    QueueCleanup(std::move(cleanup), oldWorker);
  // A superseded transition still exclusively owns these removed objects. Quiescing them cannot
  // mutate the controller, and leaves no joinable worker unowned.
  if (oldWorker)
    oldWorker->Stop(remaining());

  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> replacement;
  bool published = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_transitionSerial == transitionSerial)
    {
      replacement = std::make_shared<CAndroidJumpgateSubtitleStageWorker>(m_fileStore, m_registry);
      if (replacement->IsOperational())
      {
        m_lifecycle = {};
        m_stageWorker = replacement;
        m_startupSweepComplete = false;
        m_stopped = false;
        published = true;
      }
    }
  }
  if (!published && replacement)
    replacement->Stop(remaining());
  return published;
}

void CAndroidJumpgateSubtitleController::Stop(bool playerMayRead,
                                              bool waitForCompletion,
                                              std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        (waitForCompletion ? std::max(timeout, std::chrono::milliseconds{0})
                                           : std::chrono::milliseconds{0});
  const auto remaining = [&deadline, waitForCompletion]
  {
    if (!waitForCompletion)
      return std::chrono::milliseconds{0};
    const auto now = std::chrono::steady_clock::now();
    return now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                          : std::chrono::milliseconds{0};
  };

  std::unique_ptr<CJumpgateSubtitleCoordinator> coordinator;
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  std::optional<JumpgateSubtitleBinding> binding;
  std::uint64_t transitionSerial = 0;
  {
    std::lock_guard lock(m_mutex);
    transitionSerial = ++m_transitionSerial;
    m_stopped = true;
    binding = m_lifecycle.CurrentBinding();
    if (binding && m_coordinator)
      m_coordinator->Cancel(*binding);
    coordinator = std::move(m_coordinator);
    worker = std::move(m_stageWorker);
    m_coordinatorOrigin.clear();
  }
  if (binding && worker)
    worker->Cancel(*binding);
  if (coordinator)
    coordinator->Stop(remaining());
  std::vector<std::string> cleanup;
  bool transitionCurrent = false;
  {
    std::lock_guard lock(m_mutex);
    transitionCurrent = m_transitionSerial == transitionSerial;
    if (transitionCurrent)
      cleanup = m_lifecycle.Shutdown(playerMayRead);
  }
  if (transitionCurrent)
    QueueCleanup(std::move(cleanup), worker);
  if (worker)
    worker->Stop(remaining());
}

void CAndroidJumpgateSubtitleController::QueueCleanup(std::vector<std::string> directories)
{
  std::shared_ptr<CAndroidJumpgateSubtitleStageWorker> worker;
  {
    std::lock_guard lock(m_mutex);
    worker = m_stageWorker;
  }
  QueueCleanup(std::move(directories), worker);
}

void CAndroidJumpgateSubtitleController::QueueCleanup(
    std::vector<std::string> directories,
    const std::shared_ptr<CAndroidJumpgateSubtitleStageWorker>& worker)
{
  for (std::string& directory : directories)
  {
    if (directory.empty())
      continue;
    if (!worker || !worker->QueueCleanup(directory))
      CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle cleanup deferred to startup sweep");
  }
}

} // namespace KODI::JUMPGATE
