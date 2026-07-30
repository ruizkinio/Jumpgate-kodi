/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "XBMCApp.h"

#include "AndroidJumpgateCredentialStore.h"
#include "AndroidJumpgateSubtitleTransport.h"
#include "AndroidKey.h"
#include "CompileInfo.h"
#include "FileItem.h"
#include "JumpgateProfileStorage.h"
#include "JumpgateThresholds.h"
#include "SubtitleDownloader.h"
#include "TraktScrobbler.h"
// Audio Engine includes for Factory and interfaces
#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "TextureCache.h"
#include "application/AppEnvironment.h"
#include "application/AppParams.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "application/ApplicationPowerHandling.h"
#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Sinks/AESinkAUDIOTRACK.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "dialogs/GUIDialogJumpgatePairing.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogSelect.h"
#include "dialogs/GUIDialogYesNo.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "filesystem/VideoDatabaseFile.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIDialog.h"
#include "guilib/GUIKeyboardFactory.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/guiinfo/GUIInfoLabels.h"
#include "input/InputManager.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "input/keyboard/Key.h"
#include "input/keyboard/KeyboardTypes.h"
#include "input/keyboard/XBMC_vkeys.h"
#include "input/mouse/MouseStat.h"
#include "interfaces/AnnouncementManager.h"
#include "messaging/ApplicationMessenger.h"
#include "platform/xbmc.h"
#include "powermanagement/PowerManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/Event.h"
#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/JumpgatePairingCoordinator.h"
#include "utils/JumpgatePlaybackClaimCoordinator.h"
#include "utils/JumpgatePlaybackContext.h"
#include "utils/JumpgatePlaybackHistory.h"
#include "utils/JumpgateProfileHistoryPolicy.h"
#include "utils/JumpgateProfileRuntime.h"
#include "utils/JumpgateQrCode.h"
#include "utils/JumpgateSourceFingerprint.h"
#include "utils/JumpgateThreadRegistry.h"
#include "utils/StringUtils.h"
#include "utils/TimeUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "video/VideoFileItemClassify.h"
#include "video/VideoInfoTag.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinEvents.h"
#include "windowing/android/VideoSyncAndroid.h"
#include "windowing/android/WinSystemAndroid.h"

#include "platform/android/activity/IInputDeviceCallbacks.h"
#include "platform/android/activity/IInputDeviceEventHandler.h"
#include "platform/android/powermanagement/AndroidPowerSyscall.h"
#include "platform/android/storage/AndroidStorageProvider.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>

#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <androidjni/ApplicationInfo.h>
#include <androidjni/BitmapFactory.h>
#include <androidjni/BroadcastReceiver.h>
#include <androidjni/Build.h>
#include <androidjni/CharSequence.h>
#include <androidjni/ConnectivityManager.h>
#include <androidjni/ContentResolver.h>
#include <androidjni/Context.h>
#include <androidjni/Cursor.h>
#include <androidjni/Display.h>
#include <androidjni/DisplayManager.h>
#include <androidjni/File.h>
#include <androidjni/FileProvider.h>
#include <androidjni/Intent.h>
#include <androidjni/IntentFilter.h>
#include <androidjni/JNIThreading.h>
#include <androidjni/KeyEvent.h>
#include <androidjni/MediaStore.h>
#include <androidjni/NetworkInfo.h>
#include <androidjni/PackageManager.h>
#include <androidjni/Resources.h>
#include <androidjni/System.h>
#include <androidjni/SystemClock.h>
#include <androidjni/SystemProperties.h>
#include <androidjni/URI.h>
#include <androidjni/View.h>
#include <androidjni/Window.h>
#include <androidjni/WindowManager.h>
#include <androidjni/jutils-details.hpp>
#include <crossguid/guid.hpp>
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>

#define ACTION_XBMC_RESUME "android.intent.XBMC_RESUME"

#define PLAYBACK_STATE_STOPPED 0x0000
#define PLAYBACK_STATE_PLAYING 0x0001
#define PLAYBACK_STATE_VIDEO 0x0100
#define PLAYBACK_STATE_AUDIO 0x0200
#define PLAYBACK_STATE_CANNOT_PAUSE 0x0400

using namespace ANNOUNCEMENT;
using namespace jni;
using namespace KODI::GUILIB;
using namespace KODI::VIDEO;
using namespace std::chrono_literals;

// Forward declaration for the unpaired onDestroy safety net.
static void SaveLegacyResumeForContentLocal(
    const std::string& imdbId, int season, int episode, int64_t posMs, int64_t durMs);

constexpr int64_t JUMPGATE_AUTHENTICATED_RESUME_CORRECTION_WINDOW_MS = 60000;

static int64_t SteadyClockNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::string NoRedirectUrl(const std::string& url)
{
  CURL requestUrl(url);
  requestUrl.SetProtocolOption("redirect-limit", "0");
  return requestUrl.Get();
}

static void ClearSensitiveString(std::string& value)
{
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
}

static int ParseHttpStatus(const std::string& protocolLine);

namespace
{
int ParsePositiveHeaderInt(const std::string& value)
{
  int parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0
             ? parsed
             : 0;
}

class CJumpgateCurlDeadline final
{
public:
  CJumpgateCurlDeadline(std::shared_ptr<XFILE::CCurlFile> curl,
                        std::chrono::steady_clock::time_point deadline)
    : m_curl(std::move(curl)),
      m_deadline(deadline),
      m_watcher(&CJumpgateCurlDeadline::Run, this)
  {
  }

  ~CJumpgateCurlDeadline() { Finish(); }

  void Finish()
  {
    {
      std::lock_guard lock(m_mutex);
      m_finished = true;
    }
    m_condition.notify_all();
    if (m_watcher.joinable() && m_watcher.get_id() != std::this_thread::get_id())
      m_watcher.join();
  }

private:
  void Run()
  {
    std::unique_lock lock(m_mutex);
    const bool timedOut = !m_condition.wait_until(lock, m_deadline, [this] { return m_finished; });
    lock.unlock();
    if (timedOut)
      m_curl->Cancel();
  }

  std::shared_ptr<XFILE::CCurlFile> m_curl;
  std::chrono::steady_clock::time_point m_deadline;
  std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_finished{false};
  std::thread m_watcher;
};

class CJumpgateCurlPairingTransport final : public KODI::JUMPGATE::IJumpgatePairingTransport
{
public:
  KODI::JUMPGATE::JumpgatePairingHttpResponse Post(const std::string& url,
                                                   const std::string& body,
                                                   std::chrono::steady_clock::time_point deadline,
                                                   const std::function<bool()>& cancelled) override
  {
    auto curl = std::make_shared<XFILE::CCurlFile>();
    curl->SetRequestHeader("Content-Type", "application/json");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto boundedRemaining =
        std::clamp(remaining, std::chrono::milliseconds(1), std::chrono::milliseconds(10000));
    const int timeoutSeconds = static_cast<int>((boundedRemaining.count() + 999) / 1000);
    curl->SetTimeout(std::min(8, timeoutSeconds));
    curl->SetTotalTimeout(timeoutSeconds);
    {
      std::lock_guard lock(m_mutex);
      m_active = curl;
    }
    if (cancelled && cancelled())
      curl->Cancel();

    CJumpgateCurlDeadline deadlineWatcher(curl, deadline);

    KODI::JUMPGATE::JumpgatePairingHttpResponse response;
    response.completed = curl->Post(NoRedirectUrl(url), body, response.body);
    deadlineWatcher.Finish();
    response.statusCode = ParseHttpStatus(curl->GetHttpHeader().GetProtoLine());
    response.retryAfterSeconds =
        ParsePositiveHeaderInt(curl->GetHttpHeader().GetValue("Retry-After"));
    {
      std::lock_guard lock(m_mutex);
      if (m_active == curl)
        m_active.reset();
    }
    return response;
  }

  void Cancel() override
  {
    std::shared_ptr<XFILE::CCurlFile> active;
    {
      std::lock_guard lock(m_mutex);
      active = m_active;
    }
    if (active)
      active->Cancel();
  }

private:
  std::mutex m_mutex;
  std::shared_ptr<XFILE::CCurlFile> m_active;
};
} // namespace

static std::vector<std::string> ParseJumpgateSubtitleLanguages(const std::string& configured)
{
  std::vector<std::string> languages;
  std::size_t begin = 0;
  while (begin <= configured.size() && languages.size() < 16)
  {
    const std::size_t separator = configured.find(',', begin);
    const std::size_t end = separator == std::string::npos ? configured.size() : separator;
    std::string language = configured.substr(begin, end - begin);
    const std::size_t first = language.find_first_not_of(" \t");
    const std::size_t last = language.find_last_not_of(" \t");
    if (first != std::string::npos)
    {
      language = language.substr(first, last - first + 1);
      std::transform(language.begin(), language.end(), language.begin(),
                     [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
      if (KODI::JUMPGATE::CJumpgateSubtitleClient::IsCanonicalLanguage(language) &&
          std::find(languages.begin(), languages.end(), language) == languages.end())
      {
        languages.emplace_back(std::move(language));
      }
    }
    if (separator == std::string::npos)
      break;
    begin = separator + 1;
  }
  return languages;
}

static int ParseHttpStatus(const std::string& protocolLine)
{
  const size_t separator = protocolLine.find(' ');
  if (separator == std::string::npos || separator + 4 > protocolLine.size())
    return 0;
  int status = 0;
  for (size_t index = separator + 1; index < separator + 4; ++index)
  {
    if (protocolLine[index] < '0' || protocolLine[index] > '9')
      return 0;
    status = status * 10 + protocolLine[index] - '0';
  }
  return status;
}

class CAndroidPlaybackClaimTransport final : public KODI::JUMPGATE::IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const KODI::JUMPGATE::JumpgatePlaybackHttpRequest& request,
            KODI::JUMPGATE::JumpgatePlaybackHttpResponse& response) override
  {
    if (request.followRedirects || request.url.empty() || request.authorization.empty())
      return false;

    XFILE::CCurlFile curl;
    curl.SetRequestHeader("Content-Type", request.contentType);
    curl.SetRequestHeader("Authorization", request.authorization);
    for (const auto& header : request.headers)
    {
      if (header.name.empty() || header.value.empty() ||
          header.name.find_first_of("\r\n:") != std::string::npos ||
          header.value.find_first_of("\r\n") != std::string::npos)
      {
        return false;
      }
      curl.SetRequestHeader(header.name, header.value);
    }
    curl.SetTimeout(3);
    curl.SetTotalTimeout(3);

    CURL requestUrl{request.url};
    requestUrl.SetProtocolOption("redirect-limit", "0");
    requestUrl.SetProtocolOption("failonerror", "false");
    const bool posted = curl.Post(requestUrl.Get(), request.body, response.body);
    response.statusCode = ParseHttpStatus(curl.GetProperty(XFILE::FileProperty::RESPONSE_PROTOCOL));
    return posted || response.statusCode != 0;
  }
};

static const char* PlaybackClaimStatusName(KODI::JUMPGATE::PlaybackClaimStatus status)
{
  using KODI::JUMPGATE::PlaybackClaimStatus;
  switch (status)
  {
    case PlaybackClaimStatus::Claimed:
      return "claimed";
    case PlaybackClaimStatus::Ambiguous:
      return "ambiguous";
    case PlaybackClaimStatus::Expired:
      return "expired";
    case PlaybackClaimStatus::NotFound:
      return "not_found";
    case PlaybackClaimStatus::InvalidRequest:
      return "invalid_request";
    case PlaybackClaimStatus::TransportFailure:
      return "transport_failure";
    case PlaybackClaimStatus::AuthenticationFailure:
      return "authentication_failure";
    case PlaybackClaimStatus::HttpFailure:
      return "http_failure";
    case PlaybackClaimStatus::InvalidResponse:
      return "invalid_response";
  }
  return "unknown";
}

static bool IsAndroidEmulatorDevice()
{
  if (CJNISystemProperties::get("ro.kernel.qemu", "") == "1")
    return true;

  const std::string hardware = StringUtils::ToLower(CJNISystemProperties::get("ro.hardware", ""));
  const std::string model = StringUtils::ToLower(CJNISystemProperties::get("ro.product.model", ""));
  const std::string brand = StringUtils::ToLower(CJNISystemProperties::get("ro.product.brand", ""));

  return hardware.find("ranchu") != std::string::npos ||
         hardware.find("goldfish") != std::string::npos ||
         model.find("sdk_gphone") != std::string::npos ||
         model.find("emulator") != std::string::npos || brand.find("generic") != std::string::npos;
}

static void ApplyEmulatorPlaybackSafetyOverrides()
{
  if (!IsAndroidEmulatorDevice())
    return;

  auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return;

  auto settings = settingsComponent->GetSettings();
  if (!settings)
    return;

  bool changed = false;
  if (settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODEC))
  {
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODEC, false);
    changed = true;
  }

  if (settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE))
  {
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE, false);
    changed = true;
  }

  if (settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH))
  {
    settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH, false);
    changed = true;
  }

  if (changed)
  {
    CLog::Log(
        LOGWARNING,
        "CXBMCApp: Emulator detected (hardware={}, model={}) - forced software decode and disabled "
        "audio passthrough for playback stability",
        CJNISystemProperties::get("ro.hardware", ""),
        CJNISystemProperties::get("ro.product.model", ""));
  }
}

std::shared_ptr<CNativeWindow> CNativeWindow::CreateFromSurface(CJNISurfaceHolder holder)
{
  ANativeWindow* window = ANativeWindow_fromSurface(xbmc_jnienv(), holder.getSurface().get_raw());
  if (window)
    return std::shared_ptr<CNativeWindow>(new CNativeWindow(window));

  return {};
}

CNativeWindow::CNativeWindow(ANativeWindow* window) : m_window(window)
{
}

CNativeWindow::~CNativeWindow()
{
  if (m_window)
    ANativeWindow_release(m_window);
}

bool CNativeWindow::SetBuffersGeometry(int width, int height, int format)
{
  if (m_window)
    return (ANativeWindow_setBuffersGeometry(m_window, width, height, format) == 0);

  return false;
}

int32_t CNativeWindow::GetWidth() const
{
  if (m_window)
    return ANativeWindow_getWidth(m_window);

  return -1;
}

int32_t CNativeWindow::GetHeight() const
{
  if (m_window)
    return ANativeWindow_getHeight(m_window);

  return -1;
}

std::shared_ptr<CXBMCApp> CXBMCApp::m_appinstance;

struct CXBMCApp::QueuedBackCommand final : KODI::MESSAGING::IApplicationCallback
{
  void Execute() override
  {
    if (!app)
      return;
    if (context)
      context->Execute([this] { return app->ExecuteQueuedBackCommand(*this); });
    else
      app->ExecuteQueuedBackCommand(*this);
  }

  void Cancel() noexcept override
  {
    if (context)
      context->Reject();
    if (app)
      app->CancelQueuedBackCommand(*this);
  }

  std::shared_ptr<CXBMCApp> app;
  std::optional<KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext> context;
  BackCommand command{BackCommand::EXTERNAL_BACK};
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken{0};
  uint64_t playbackGeneration{0};
  uint64_t playbackToken{0};
};

struct CXBMCApp::QueuedExternalPlayback final : KODI::MESSAGING::IApplicationCallback
{
  void Execute() override
  {
    if (app)
      app->ExecuteQueuedExternalPlayback(*this);
  }

  void Cancel() noexcept override
  {
    if (app)
      app->CancelQueuedExternalPlayback(*this);
  }

  std::shared_ptr<CXBMCApp> app;
  std::unique_ptr<CFileItem> item;
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken{0};
  uint64_t admissionGeneration{0};
  uint64_t admissionToken{0};
  std::string resultRequestId;
};

struct CXBMCApp::QueuedExternalPlayerResult final : KODI::MESSAGING::IApplicationCallback
{
  void Execute() override
  {
    if (app)
      app->ExecuteQueuedExternalPlayerResult(*this);
  }

  void Cancel() noexcept override
  {
    if (app)
      app->CancelQueuedExternalPlayerResult(*this);
  }

  std::shared_ptr<CXBMCApp> app;
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken{0};
  uint64_t generation{0};
  std::string requestId;
  bool wasStandalone{false};
};

CXBMCApp::CXBMCApp(ANativeActivity* nativeActivity, IInputHandler& inputHandler)
  : CJNIMainActivity(nativeActivity),
    CJNIBroadcastReceiver(CJNIContext::getPackageName() + ".XBMCBroadcastReceiver"),
    m_inputHandler(inputHandler)
{
  m_activity = nativeActivity;
  if (m_activity == nullptr)
  {
    android_printf("CXBMCApp: invalid ANativeActivity instance");
    exit(1);
    return;
  }

  m_jumpgateBackLifecycleToken =
      jni::CJNIMainActivity::GetJumpgateBackLifecycleToken(nativeActivity);
  if (m_jumpgateBackLifecycleToken ==
      KODI::JUMPGATE::CJumpgateBackDispatcher::INVALID_LIFECYCLE_TOKEN)
  {
    throw std::runtime_error("CXBMCApp has no native Back lifecycle token");
  }

  // Android lifecycle callbacks and Kodi's main thread can overlap. Keep the
  // object address stable for the app lifetime and toggle only its internally
  // synchronized initialized state when entering/leaving external-player mode.
  m_traktScrobbler = std::make_unique<TraktScrobbler>();
  m_mainView = std::make_unique<CJNIXBMCMainView>(this);
  m_hdmiSource = CJNISystemProperties::get("ro.hdmi.device_type", "") == "4";
  android_printf("CXBMCApp: Created");

  // crossguid requires init on android only once on process start
  JNIEnv* env = xbmc_jnienv();
  xg::initJni(env);
}

CXBMCApp::~CXBMCApp()
{
  CJNIMainActivity::RetireAppInstance(m_jumpgateBackLifecycleToken, m_jumpgateAppPublicationToken,
                                      this);
  CJNIMainActivity::GetJumpgateBackDispatcher().UnpublishSink(m_jumpgateBackLifecycleToken,
                                                              m_jumpgateBackPublicationToken);
  StopBridgePairingWorker(true);
  StopPlaybackClaimCoordinator(false);
}

void CXBMCApp::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                        const std::string& sender,
                        const std::string& message,
                        const CVariant& data)
{
  if (sender != CAnnouncementManager::ANNOUNCEMENT_SENDER)
    return;

  if (flag & Input)
  {
    if (message == "OnInputRequested")
      CAndroidKey::SetHandleSearchKeys(true);
    else if (message == "OnInputFinished")
      CAndroidKey::SetHandleSearchKeys(false);
  }
  else if (flag & Player)
  {
    if (message == "OnPlay")
    {
      uint64_t token = 0;
      const CVariant& tokenValue = data["jumpgate"]["playbackToken"];
      if (tokenValue.isUnsignedInteger())
        token = tokenValue.asUnsignedInteger();
      else if (tokenValue.isSignedInteger() && tokenValue.asInteger() > 0)
        token = static_cast<uint64_t>(tokenValue.asInteger());
      OnPlayBackStarted(false, token);
    }
    else if (message == "OnResume")
      OnPlayBackStarted(true);
    else if (message == "OnPause")
      OnPlayBackPaused();
    else if (message == "OnStop")
      OnPlayBackStopped(data["end"].asBoolean(false));
    else if (message == "OnSeek")
    {
      m_mediaSessionUpdated = false;
      UpdateSessionState();
    }
    else if (message == "OnSpeedChanged")
    {
      m_mediaSessionUpdated = false;
      UpdateSessionState();
    }
    else if (message == "OnAVStart")
    {
      m_mediaSessionUpdated = false;
      UpdateSessionState();
    }
  }
  else if (flag & Info)
  {
    if (message == "OnChanged")
    {
      m_mediaSessionUpdated = false;
      UpdateSessionMetadata();
    }
  }
}

void CXBMCApp::onStart()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  if (m_firstrun)
  {
    // Check if Java-side detected external player mode (set in Main.onCreate)
    jboolean extMode = call_method<jboolean>(m_context, "isExternalPlayerMode", "()Z");
    if (extMode)
    {
      SetExternalPlayerMode(true);
      android_printf("CXBMCApp: External player mode detected at startup");
    }

    // Register sink
    AE::CAESinkFactory::ClearSinks();
    CAESinkAUDIOTRACK::Register();

    // Create thread to run Kodi main event loop
    m_thread = std::thread(&CXBMCApp::run, this);

    // Some intent filters MUST be registered in code rather than through the manifest
    CJNIIntentFilter intentFilter;
    intentFilter.addAction(CJNIIntent::ACTION_BATTERY_CHANGED);
    intentFilter.addAction(CJNIIntent::ACTION_SCREEN_ON);
    intentFilter.addAction(CJNIIntent::ACTION_HEADSET_PLUG);
    // We currently use HDMI_AUDIO_PLUG for mode switch, don't use it on TV's (device_type = "0"
    if (m_hdmiSource)
      intentFilter.addAction(CJNIAudioManager::ACTION_HDMI_AUDIO_PLUG);

    intentFilter.addAction(CJNIIntent::ACTION_SCREEN_OFF);
    intentFilter.addAction(CJNIConnectivityManager::CONNECTIVITY_ACTION);
    registerReceiver(*this, intentFilter);
    m_mediaSession = std::make_unique<CJNIXBMCMediaSession>();
    m_inputHandler.setDPI(GetDPI());
    runNativeOnUiThread(RegisterDisplayListenerCallback, nullptr);
  }
}

namespace
{
bool isHeadsetPlugged()
{
  CJNIAudioManager audioManager(CXBMCApp::getSystemService(CJNIContext::AUDIO_SERVICE));

  if (CJNIBuild::SDK_INT >= 26)
  {
    const CJNIAudioDeviceInfos devices =
        audioManager.getDevices(CJNIAudioManager::GET_DEVICES_OUTPUTS);

    for (const CJNIAudioDeviceInfo& device : devices)
    {
      const int type = device.getType();
      if (type == CJNIAudioDeviceInfo::TYPE_WIRED_HEADSET ||
          type == CJNIAudioDeviceInfo::TYPE_WIRED_HEADPHONES ||
          type == CJNIAudioDeviceInfo::TYPE_BLUETOOTH_A2DP ||
          type == CJNIAudioDeviceInfo::TYPE_BLUETOOTH_SCO)
      {
        return true;
      }
    }
    return false;
  }
  else
  {
    return audioManager.isWiredHeadsetOn() || audioManager.isBluetoothA2dpOn();
  }
}
} // namespace

void CXBMCApp::onResume()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  if (g_application.IsInitialized() &&
      CServiceBroker::GetWinSystem()->GetOSScreenSaver()->IsInhibited())
    KeepScreenOn(true);

  // Reset shutdown timer on wake up
  if (g_application.IsInitialized() &&
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_POWERMANAGEMENT_SHUTDOWNTIME))
  {
    auto& components = CServiceBroker::GetAppComponents();
    const auto appPower = components.GetComponent<CApplicationPowerHandling>();
    appPower->ResetShutdownTimers();
  }

  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger)
    messenger->PostMsg(TMSG_RESUMEAPP);

  m_headsetPlugged = isHeadsetPlugged();

  // Clear the applications cache. We could have installed/deinstalled apps
  {
    std::unique_lock lock(m_applicationsMutex);
    m_applications.clear();
  }

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler)
    m_traktScrobbler->SetBackgrounded(false);
  if (m_bResumePlayback && appPlayer->IsPlaying())
  {
    if (appPlayer->HasVideo())
    {
      if (appPlayer->IsPaused())
        CServiceBroker::GetAppMessenger()->SendMsg(
            TMSG_GUI_ACTION, WINDOW_INVALID, -1,
            static_cast<void*>(new CAction(ACTION_PLAYER_PLAY)));
    }
  }

  // Re-request Visible Behind
  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && (m_playback_state & PLAYBACK_STATE_VIDEO))
    RequestVisibleBehind(true);
}

void CXBMCApp::onPause()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_bResumePlayback = false;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler)
    m_traktScrobbler->SetBackgrounded(true);
  if (appPlayer->IsPlaying())
  {
    if (appPlayer->HasVideo())
    {
      if (!appPlayer->IsPaused() && !m_hasReqVisible)
      {
        CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                   static_cast<void*>(new CAction(ACTION_PAUSE)));
        m_bResumePlayback = true;
      }
    }
  }

  if (m_hasReqVisible)
  {
    CGUIComponent* gui = CServiceBroker::GetGUI();
    if (gui)
    {
      gui->GetWindowManager().SwitchToFullScreen(true);
    }
  }

  KeepScreenOn(false);
  m_hasReqVisible = false;
}

void CXBMCApp::onStop()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && !m_hasReqVisible)
  {
    if (m_playback_state & PLAYBACK_STATE_CANNOT_PAUSE)
      CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                 static_cast<void*>(new CAction(ACTION_STOP)));
    else if (m_playback_state & PLAYBACK_STATE_VIDEO)
      CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                 static_cast<void*>(new CAction(ACTION_PAUSE)));
  }
}

void CXBMCApp::onDestroy()
{
  android_printf("%s", __PRETTY_FUNCTION__);
  CancelExternalPlaybackForLifecycleTeardown();
  CJNIMainActivity::GetJumpgateBackDispatcher().UnpublishSink(m_jumpgateBackLifecycleToken,
                                                              m_jumpgateBackPublicationToken);

  StopBridgePairingWorker(true, false);

  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();

    // The exact source namespace remains available even when no claim or profile
    // exists. Legacy metadata history is retained only for the compatibility path.
    if (m_externalPlayerMode.load(std::memory_order_relaxed))
    {
      SavePairedPlaybackHistory(false);
      if (m_traktScrobbler && !m_traktScrobbler->IsBridgeProfileBacked())
      {
        std::string imdb = m_traktScrobbler->GetImdbId();
        if (!imdb.empty())
        {
          int season = m_traktScrobbler->GetSeason();
          int episode = m_traktScrobbler->GetEpisode();
          int64_t posMs = m_lastPlaybackTimeMs.load(std::memory_order_relaxed);
          int64_t durMs = m_lastPlaybackDurationMs.load(std::memory_order_relaxed);
          if (posMs > 0)
          {
            SaveLegacyResumeForContentLocal(imdb, season, episode, posMs, durMs);
            android_printf("Jumpgate: onDestroy safety-net resume saved for %s pos=%lld dur=%lld",
                           imdb.c_str(), (long long)posMs, (long long)durMs);
          }
        }
      }
    }

    // Enqueue cleanup and hand unfinished workers to the final service-teardown
    // registry. The Android destroy callback must not wait for network I/O.
    if (m_traktScrobbler)
      m_traktScrobbler->StopForReplacement();
    ReleasePlaybackSourceClaim();
    StopPlaybackClaimCoordinator(false);
  }

  // Cancel immediately, then let the final service drain join any Curl worker.
  // Staged files stay in place until the player can no longer be reading them.
  StopJumpgateSubtitleController(true, false);

  if (m_subtitleDownloader)
  {
    m_subtitleDownloader->Deinitialize();
    m_subtitleDownloader.reset();
  }

  if (m_traktScrobbler)
    m_traktScrobbler->Deinitialize(false);

  unregisterReceiver(*this);

  UnregisterDisplayListener();
  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);

  m_mediaSession.release();
}

void CXBMCApp::onSaveState(void** data, size_t* size)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // no need to save anything as XBMC is running in its own thread
}

void CXBMCApp::onConfigurationChanged()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // ignore any configuration changes like screen rotation etc
}

void CXBMCApp::onLowMemory()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // can't do much as we don't want to close completely
}

void CXBMCApp::onCreateWindow(ANativeWindow* window)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::onResizeWindow()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_window.reset();
  // no need to do anything because we are fixed in fullscreen landscape mode
}

void CXBMCApp::onDestroyWindow()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  SetJumpgateBackInputReady(false);
}

void CXBMCApp::onGainFocus()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_hasFocus = true;
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPower = components.GetComponent<CApplicationPowerHandling>();
  appPower->WakeUpScreenSaverAndDPMS();
}

void CXBMCApp::onLostFocus()
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  m_hasFocus = false;
}

void CXBMCApp::RegisterDisplayListenerCallback(void*)
{
  CJNIDisplayManager displayManager(getSystemService(CJNIContext::DISPLAY_SERVICE));
  if (displayManager)
  {
    android_printf("CXBMCApp: installing DisplayManager::DisplayListener");
    displayManager.registerDisplayListener(CXBMCApp::Get().getDisplayListener());
  }
}

void CXBMCApp::UnregisterDisplayListener()
{
  CJNIDisplayManager displayManager(getSystemService(CJNIContext::DISPLAY_SERVICE));
  if (displayManager)
  {
    android_printf("CXBMCApp: removing DisplayManager::DisplayListener");
    displayManager.unregisterDisplayListener(m_displayListener.get_raw());
  }
}

void CXBMCApp::Initialize()
{
  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(
      this, ANNOUNCEMENT::Input | ANNOUNCEMENT::Player | ANNOUNCEMENT::Info);

  InitializeJumpgateProfileRuntime();

  if (m_externalPlayerMode.load(std::memory_order_relaxed))
  {
    ApplyEmulatorPlaybackSafetyOverrides();

    ApplyActiveJumpgateProfile();
    m_traktScrobbler->Initialize();

    if (!m_jumpgateSubtitleController)
      m_jumpgateSubtitleController =
          std::make_unique<KODI::JUMPGATE::CAndroidJumpgateSubtitleController>();
    m_jumpgateSubtitleController->SweepStartupOrphans();

    KODI::JUMPGATE::ActiveProfile active;
    if (m_jumpgateProfileRuntime)
      active = m_jumpgateProfileRuntime->GetActive();
    const auto provider = KODI::JUMPGATE::SelectAndroidJumpgateSubtitleProvider(
        true, active.selected && active.sourceBacked, active.credentialsValid,
        active.subtitlesEnabled, false);
    if (provider == KODI::JUMPGATE::AndroidJumpgateSubtitleProvider::OpenSubtitles)
    {
      m_subtitleDownloader = std::make_unique<SubtitleDownloader>();
      m_subtitleDownloader->Initialize();
      m_subtitleDownloader->SetLanguages(active.subtitleLanguages);
    }
    active.ClearSecrets();
  }
}

void CXBMCApp::Deinitialize()
{
  m_shutdownCoordinator.RunOnceAndWait(
      [this]
      {
        StopBridgePairingWorker(true);
        {
          auto authorityTransaction = m_playbackAuthority.BeginTransaction();
          SavePairedPlaybackHistory(false);
          ReleasePlaybackSourceClaim();
          StopPlaybackClaimCoordinator(false);
        }
        StopJumpgateSubtitleController(false);
        if (m_traktScrobbler)
          m_traktScrobbler->Deinitialize(true);
        if (!KODI::JUMPGATE::CJumpgateThreadRegistry::Global()->JoinAll(std::chrono::seconds{12}))
        {
          CLog::Log(LOGERROR, "CXBMCApp: bounded Jumpgate worker drain expired");
        }
      });
}

bool CXBMCApp::Stop(int exitCode)
{
  if (m_exiting)
    return true; // stage two: android activity has finished

  // enter stage one: tell android to finish the activity
  CLog::Log(LOGINFO, "XBMCApp: Finishing the activity");

  m_exitCode = exitCode;

  // Notify Android its finish routine.
  // This will cause Android to run through its teardown events, it calls:
  // onPause(), onLostFocus(), onDestroyWindow(), onStop(), onDestroy().
  ANativeActivity_finish(m_activity);

  return false; // stage one: let android finish the activity
}

void CXBMCApp::Quit()
{
  CLog::Log(LOGINFO, "XBMCApp: Stopping the application...");

  uint32_t msgId;
  switch (m_exitCode)
  {
    case EXITCODE_QUIT:
      msgId = TMSG_QUIT;
      break;
    case EXITCODE_POWERDOWN:
      msgId = TMSG_POWERDOWN;
      break;
    case EXITCODE_REBOOT:
      msgId = TMSG_RESTART;
      break;
    case EXITCODE_RESTARTAPP:
      msgId = TMSG_RESTARTAPP;
      break;
    default:
      CLog::Log(LOGWARNING, "CXBMCApp::Stop : Unexpected exit code. Defaulting to QUIT.");
      msgId = TMSG_QUIT;
      break;
  }

  m_exiting = true; // enter stage two: android activity has finished. go on with stopping Kodi
  CServiceBroker::GetAppMessenger()->PostMsg(msgId);

  // wait for the run thread to finish
  m_thread.join();

  // Note: CLog no longer available here.
  android_printf("%s: Application stopped!", __PRETTY_FUNCTION__);
}

void CXBMCApp::KeepScreenOnCallback(void* onVariant)
{
  CVariant* onV = static_cast<CVariant*>(onVariant);
  bool on = onV->asBoolean();
  delete onV;

  CJNIWindow window = getWindow();
  if (window)
  {
    on ? window.addFlags(CJNIWindowManagerLayoutParams::FLAG_KEEP_SCREEN_ON)
       : window.clearFlags(CJNIWindowManagerLayoutParams::FLAG_KEEP_SCREEN_ON);
  }
}

void CXBMCApp::KeepScreenOn(bool on)
{
  android_printf("%s: %s", __PRETTY_FUNCTION__, on ? "true" : "false");
  // this object is deallocated in the callback
  CVariant* variant = new CVariant(on);
  runNativeOnUiThread(KeepScreenOnCallback, variant);
}

bool CXBMCApp::AcquireAudioFocus()
{
  CJNIAudioManager audioManager(getSystemService(CJNIContext::AUDIO_SERVICE));

  int result;

  if (CJNIBuild::SDK_INT >= 26)
  {
    CJNIAudioFocusRequestClassBuilder audioFocusBuilder(CJNIAudioManager::AUDIOFOCUS_GAIN);
    CJNIAudioAttributesBuilder audioAttrBuilder;

    audioAttrBuilder.setUsage(CJNIAudioAttributes::USAGE_MEDIA);
    audioAttrBuilder.setContentType(CJNIAudioAttributes::CONTENT_TYPE_MUSIC);

    audioFocusBuilder.setAudioAttributes(audioAttrBuilder.build());
    audioFocusBuilder.setAcceptsDelayedFocusGain(true);
    audioFocusBuilder.setWillPauseWhenDucked(true);
    audioFocusBuilder.setOnAudioFocusChangeListener(m_audioFocusListener);

    // Request audio focus for playback
    result = audioManager.requestAudioFocus(audioFocusBuilder.build());
  }
  else
  {
    // Request audio focus for playback
    result = audioManager.requestAudioFocus(m_audioFocusListener,
                                            // Use the music stream.
                                            CJNIAudioManager::STREAM_MUSIC,
                                            // Request permanent focus.
                                            CJNIAudioManager::AUDIOFOCUS_GAIN);
  }
  if (result != CJNIAudioManager::AUDIOFOCUS_REQUEST_GRANTED)
  {
    android_printf("Audio Focus request failed");
    return false;
  }
  return true;
}

bool CXBMCApp::ReleaseAudioFocus()
{
  CJNIAudioManager audioManager(getSystemService(CJNIContext::AUDIO_SERVICE));
  int result;

  if (CJNIBuild::SDK_INT >= 26)
  {
    // Abandon requires the same AudioFocusRequest as the request
    CJNIAudioFocusRequestClassBuilder audioFocusBuilder(CJNIAudioManager::AUDIOFOCUS_GAIN);
    CJNIAudioAttributesBuilder audioAttrBuilder;

    audioAttrBuilder.setUsage(CJNIAudioAttributes::USAGE_MEDIA);
    audioAttrBuilder.setContentType(CJNIAudioAttributes::CONTENT_TYPE_MUSIC);

    audioFocusBuilder.setAudioAttributes(audioAttrBuilder.build());
    audioFocusBuilder.setAcceptsDelayedFocusGain(true);
    audioFocusBuilder.setWillPauseWhenDucked(true);
    audioFocusBuilder.setOnAudioFocusChangeListener(m_audioFocusListener);

    // Release audio focus after playback
    result = audioManager.abandonAudioFocusRequest(audioFocusBuilder.build());
  }
  else
  {
    // Release audio focus after playback
    result = audioManager.abandonAudioFocus(m_audioFocusListener);
  }

  if (result != CJNIAudioManager::AUDIOFOCUS_REQUEST_GRANTED)
  {
    android_printf("Audio Focus abandon failed");
    return false;
  }
  return true;
}

void CXBMCApp::RequestVisibleBehind(bool requested)
{
  if (requested == m_hasReqVisible)
    return;

  if (CJNIBuild::SDK_INT < 26)
    m_hasReqVisible = requestVisibleBehind(requested);

  CLog::Log(LOGDEBUG, "Visible Behind request: {}", m_hasReqVisible ? "true" : "false");
}

void CXBMCApp::run()
{
  int status = 0;

  SetupEnv();

  // Wait for main window
  if (!GetNativeWindow(30000))
    return;

  m_firstrun = false;
  android_printf(" => running XBMC_Run...");

  auto appParams = std::make_shared<CAppParams>();
  if (m_externalPlayerMode.load(std::memory_order_relaxed))
    appParams->SetExternalPlayerMode(true);
  CAppEnvironment::SetUp(appParams);
  status = XBMC_Run(true);
  CAppEnvironment::TearDown();

  android_printf(" => XBMC_Run finished with %d", status);
}

bool CXBMCApp::XBMC_SetupDisplay()
{
  android_printf("XBMC_SetupDisplay()");
  bool result;
  CServiceBroker::GetAppMessenger()->SendMsg(TMSG_DISPLAY_SETUP, -1, -1,
                                             static_cast<void*>(&result));
  return result;
}

bool CXBMCApp::XBMC_DestroyDisplay()
{
  android_printf("XBMC_DestroyDisplay()");
  bool result;
  CServiceBroker::GetAppMessenger()->SendMsg(TMSG_DISPLAY_DESTROY, -1, -1,
                                             static_cast<void*>(&result));
  return result;
}

bool CXBMCApp::SetBuffersGeometry(int width, int height, int format)
{
  if (m_window)
    return m_window->SetBuffersGeometry(width, height, format);

  return false;
}

void CXBMCApp::SetDisplayModeCallback(void* modeVariant)
{
  CVariant* modeV = static_cast<CVariant*>(modeVariant);
  int mode = (*modeV)["mode"].asInteger();
  delete modeV;

  CJNIWindow window = getWindow();
  if (window)
  {
    CJNIWindowManagerLayoutParams params = window.getAttributes();
    if (params.getpreferredDisplayModeId() != mode)
    {
      params.setpreferredDisplayModeId(mode);
      window.setAttributes(params);
      return;
    }
  }
  CXBMCApp::Get().m_displayChangeEvent.Set();
}

void CXBMCApp::SetDisplayMode(int mode, float rate)
{
  if (mode < 1.0)
    return;

  CJNIWindow window = getWindow();
  if (window)
  {
    CJNIWindowManagerLayoutParams params = window.getAttributes();
    if (params.getpreferredDisplayModeId() == mode)
      return;
  }

  m_displayChangeEvent.Reset();

  if (m_hdmiSource)
    dynamic_cast<CWinSystemAndroid*>(CServiceBroker::GetWinSystem())->InitiateModeChange();

  std::map<std::string, CVariant> vmap;
  vmap["mode"] = mode;
  m_refreshRate = rate;
  CVariant* variant = new CVariant(vmap);
  runNativeOnUiThread(SetDisplayModeCallback, variant);
  if (g_application.IsInitialized())
    m_displayChangeEvent.Wait(5000ms);
}

int CXBMCApp::android_printf(const char* format, ...)
{
  // For use before CLog is setup by XBMC_Run()
  va_list args, args_copy;
  va_start(args, format);
  va_copy(args_copy, args);
  int result;

  if (CServiceBroker::IsLoggingUp())
  {
    std::string message;
    int len = vsnprintf(0, 0, format, args_copy);
    message.resize(len);
    result = vsnprintf(message.data(), len + 1, format, args);
    CLog::Log(LOGDEBUG, "{}", message);
  }
  else
  {
    result = __android_log_vprint(ANDROID_LOG_VERBOSE, "Kodi", format, args);
  }

  va_end(args_copy);
  va_end(args);

  return result;
}

int CXBMCApp::GetDPI() const
{
  if (m_activity->assetManager == nullptr)
    return 0;

  // grab DPI from the current configuration - this is approximate
  // but should be close enough for what we need
  AConfiguration* config = AConfiguration_new();
  AConfiguration_fromAssetManager(config, m_activity->assetManager);
  int dpi = AConfiguration_getDensity(config);
  AConfiguration_delete(config);

  return dpi;
}

CRect CXBMCApp::MapRenderToDroid(const CRect& srcRect)
{
  float scaleX = 1.0;
  float scaleY = 1.0;

  CJNIRect r = getDisplayRect();
  if (r.width() && r.height())
  {
    RESOLUTION_INFO renderRes = CDisplaySettings::GetInstance().GetResolutionInfo(
        CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution());
    scaleX = (double)r.width() / renderRes.iWidth;
    scaleY = (double)r.height() / renderRes.iHeight;
  }

  return CRect(srcRect.x1 * scaleX, srcRect.y1 * scaleY, srcRect.x2 * scaleX, srcRect.y2 * scaleY);
}

void CXBMCApp::UpdateSessionMetadata()
{
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  CGUIInfoManager& infoMgr = CServiceBroker::GetGUI()->GetInfoManager();
  CJNIMediaMetadataBuilder builder;
  builder
      .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_TITLE,
                 infoMgr.GetLabel(PLAYER_TITLE, INFO::DEFAULT_CONTEXT))
      .putString(CJNIMediaMetadata::METADATA_KEY_TITLE,
                 infoMgr.GetLabel(PLAYER_TITLE, INFO::DEFAULT_CONTEXT))
      .putLong(CJNIMediaMetadata::METADATA_KEY_DURATION, appPlayer->GetTotalTime())
      //      .putString(CJNIMediaMetadata::METADATA_KEY_ART_URI, thumb)
      //      .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_ICON_URI, thumb)
      //      .putString(CJNIMediaMetadata::METADATA_KEY_ALBUM_ART_URI, thumb)
      ;

  std::string thumb;
  if (m_playback_state & PLAYBACK_STATE_VIDEO)
  {
    builder
        .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_SUBTITLE,
                   infoMgr.GetLabel(VIDEOPLAYER_TAGLINE, INFO::DEFAULT_CONTEXT))
        .putString(CJNIMediaMetadata::METADATA_KEY_ARTIST,
                   infoMgr.GetLabel(VIDEOPLAYER_DIRECTOR, INFO::DEFAULT_CONTEXT));
    thumb = infoMgr.GetImage(VIDEOPLAYER_COVER, -1);
  }
  else if (m_playback_state & PLAYBACK_STATE_AUDIO)
  {
    builder
        .putString(CJNIMediaMetadata::METADATA_KEY_DISPLAY_SUBTITLE,
                   infoMgr.GetLabel(MUSICPLAYER_ARTIST, INFO::DEFAULT_CONTEXT))
        .putString(CJNIMediaMetadata::METADATA_KEY_ARTIST,
                   infoMgr.GetLabel(MUSICPLAYER_ARTIST, INFO::DEFAULT_CONTEXT));
    thumb = infoMgr.GetImage(MUSICPLAYER_COVER, -1);
  }
  bool needrecaching = false;
  std::string cachefile = CServiceBroker::GetTextureCache()->CheckCachedImage(thumb, needrecaching);
  if (!cachefile.empty())
  {
    std::string actualfile = CSpecialProtocol::TranslatePath(cachefile);
    CJNIBitmap bmp = CJNIBitmapFactory::decodeFile(actualfile);
    if (bmp)
      builder.putBitmap(CJNIMediaMetadata::METADATA_KEY_ART, bmp);
  }
  m_mediaSession->updateMetadata(builder.build());
}

void CXBMCApp::UpdateSessionState()
{
  CJNIPlaybackStateBuilder builder;
  int state = CJNIPlaybackState::STATE_NONE;
  int64_t pos = 0;
  float speed = 0.0;
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  uint32_t oldPlayState = m_playback_state;
  if (m_playback_state != PLAYBACK_STATE_STOPPED)
  {
    if (appPlayer->HasVideo())
      m_playback_state |= PLAYBACK_STATE_VIDEO;
    else
      m_playback_state &= ~PLAYBACK_STATE_VIDEO;

    if (appPlayer->HasAudio())
      m_playback_state |= PLAYBACK_STATE_AUDIO;
    else
      m_playback_state &= ~PLAYBACK_STATE_AUDIO;

    pos = appPlayer->GetTime();
    speed = appPlayer->GetPlaySpeed();

    if (m_playback_state & PLAYBACK_STATE_PLAYING)
      state = CJNIPlaybackState::STATE_PLAYING;
    else
      state = CJNIPlaybackState::STATE_PAUSED;
  }
  else
    state = CJNIPlaybackState::STATE_STOPPED;

  if ((oldPlayState != m_playback_state) || !m_mediaSessionUpdated)
  {
    builder.setState(state, pos, speed, CJNISystemClock::elapsedRealtime())
        .setActions(CJNIPlaybackState::PLAYBACK_POSITION_UNKNOWN);
    m_mediaSession->updatePlaybackState(builder.build());
    m_mediaSessionUpdated = true;
  }
}

void CXBMCApp::OnPlayBackStarted(bool resumed, uint64_t token)
{
  CLog::Log(LOGDEBUG, "{}", __PRETTY_FUNCTION__);
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();

  m_playback_state = PLAYBACK_STATE_PLAYING;
  if (appPlayer->HasVideo())
    m_playback_state |= PLAYBACK_STATE_VIDEO;
  if (appPlayer->HasAudio())
    m_playback_state |= PLAYBACK_STATE_AUDIO;
  if (!appPlayer->CanPause())
    m_playback_state |= PLAYBACK_STATE_CANNOT_PAUSE;

  m_mediaSession->activate(true);
  m_mediaSessionUpdated = false;
  UpdateSessionState();

  CJNIIntent intent(ACTION_XBMC_RESUME, CJNIURI::EMPTY, *this, get_class(CJNIContext::get_raw()));
  m_mediaSession->updateIntent(intent);

  AcquireAudioFocus();
  CAndroidKey::SetHandleMediaKeys(false);

  RequestVisibleBehind(true);

  // Reset overlay flag so ProcessSlow hides the overlay at the right time —
  // when currentTime > 0 (actual frame decoding started, not just file opened).
  // Do NOT call hideLoadingOverlay here: this callback fires when the player
  // opens the file, well before any frames are decoded/displayed on screen.
  if (m_externalPlayerMode.load(std::memory_order_relaxed))
  {
    m_overlayHidden = false;
    CLog::Log(LOGINFO, "CXBMCApp: Playback started — overlay will hide when frames render");
  }
  uint64_t subtitleGeneration = 0;
  uint64_t playbackStartedGeneration = 0;
  bool playbackAuthorityAccepted = true;
  if (resumed)
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    const auto resumedEvent = authorityTransaction.CommitPlaybackResumed();
    if (!resumedEvent)
    {
      playbackAuthorityAccepted = false;
      CLog::Log(LOGDEBUG, "CXBMCApp: Ignoring resume without an active playback token");
    }
    else
    {
      playbackStartedGeneration = resumedEvent->generation;
    }
  }
  else
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    const uint64_t continuationGeneration = m_externalPlayerMode.load(std::memory_order_relaxed)
                                                ? m_playbackResultState.CurrentGeneration()
                                                : 0;
    const auto started = authorityTransaction.CommitPlaybackStarted(continuationGeneration, token);
    if (!started)
    {
      playbackAuthorityAccepted = false;
      CLog::Log(LOGWARNING,
                "CXBMCApp: Ignoring playback start during a profile authority transition");
    }
    else if (token == 0 && started->generation == 0)
    {
      m_ordinaryPlaybackAuthorityToken.store(started->token, std::memory_order_relaxed);
    }
    else if (m_externalPlayerMode.load(std::memory_order_relaxed))
    {
      subtitleGeneration = started->generation;
      playbackStartedGeneration = started->generation;
    }
  }
  if (playbackAuthorityAccepted && !resumed && playbackStartedGeneration != 0 &&
      m_externalPlayerMode.load(std::memory_order_relaxed))
  {
    m_externalPlaybackStartedGeneration.store(playbackStartedGeneration, std::memory_order_relaxed);
    m_externalPlaybackStartedAtSteadyMs.store(SteadyClockNowMs(), std::memory_order_relaxed);
  }
  bool stopCanceledDispatch = false;
  if (token != 0)
  {
    std::lock_guard lock(m_externalPlaybackQueueMutex);
    if (m_externalPlaybackDispatchToken == token)
    {
      stopCanceledDispatch =
          m_pendingExternalPlaybackStopGeneration == m_externalPlaybackDispatchGeneration &&
          m_pendingExternalPlaybackStopToken == token;
      if (stopCanceledDispatch)
      {
        m_pendingExternalPlaybackStopGeneration = 0;
        m_pendingExternalPlaybackStopToken = 0;
      }
      m_externalPlaybackDispatchGeneration = 0;
      m_externalPlaybackDispatchToken = 0;
      m_externalPlaybackDispatchRequestId.clear();
      m_externalPlaybackDispatchPayload.reset();
    }
  }
  if (stopCanceledDispatch)
  {
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                               static_cast<void*>(new CAction(ACTION_STOP)));
  }
  if (subtitleGeneration != 0 && m_jumpgateSubtitleController)
    m_jumpgateSubtitleController->MarkPlaybackReady(subtitleGeneration);
  if (playbackAuthorityAccepted && m_externalPlayerMode.load(std::memory_order_relaxed) &&
      m_traktScrobbler)
  {
    m_traktScrobbler->OnPlaybackStarted(resumed);
  }
}

void CXBMCApp::OnPlayBackPaused()
{
  CLog::Log(LOGDEBUG, "{}", __PRETTY_FUNCTION__);

  m_playback_state &= ~PLAYBACK_STATE_PLAYING;
  m_mediaSessionUpdated = false;
  UpdateSessionState();

  RequestVisibleBehind(false);
  ReleaseAudioFocus();
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler)
    m_traktScrobbler->OnPlaybackPaused();
}

void CXBMCApp::OnPlayBackStopped(bool completed)
{
  static_cast<void>(completed);
  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  CLog::Log(LOGDEBUG, "{}", __PRETTY_FUNCTION__);

  const uint64_t ordinaryToken =
      m_ordinaryPlaybackAuthorityToken.exchange(0, std::memory_order_relaxed);
  if (ordinaryToken != 0)
    authorityTransaction.CommitPlaybackStopped(ordinaryToken);

  // ApplicationMessageHandling commits the generation-bound terminal state
  // before publishing OnStop. This callback only updates Android UI state.
  if (authorityTransaction.GetActiveToken() != 0)
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: Ignored delayed OnStop while replacement playback is active");
    return;
  }

  m_playback_state = PLAYBACK_STATE_STOPPED;
  m_mediaSessionUpdated = false;
  UpdateSessionState();
  m_mediaSession->activate(false);

  RequestVisibleBehind(false);
  CAndroidKey::SetHandleMediaKeys(true);
  ReleaseAudioFocus();
}

void CXBMCApp::CommitExternalPlaybackTerminal(bool completed, uint64_t token, bool started)
{
  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> terminal;
  bool superseded = false;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    terminal = authorityTransaction.CommitPlaybackTerminal(token, started);
    superseded = terminal && authorityTransaction.HasNewerPlayback(terminal->token);
  }
  if (!terminal || terminal->generation == 0)
    return;
  if (m_jumpgateSubtitleController)
    m_jumpgateSubtitleController->OnPlaybackTerminal(terminal->generation);
  if (superseded)
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: Ignored delayed terminal for superseded playback token {}",
              terminal->token);
    return;
  }

  int64_t positionMs = m_lastPlaybackTimeMs.load(std::memory_order_relaxed);
  int64_t durationMs = m_lastPlaybackDurationMs.load(std::memory_order_relaxed);
  const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  const int64_t currentPositionMs = appPlayer->GetTime();
  const int64_t currentDurationMs = appPlayer->GetTotalTime();
  if (currentPositionMs > 0 || currentDurationMs > 0)
  {
    positionMs = std::max<int64_t>(0, currentPositionMs);
    durationMs = std::max<int64_t>(0, currentDurationMs);
    m_lastPlaybackTimeMs.store(positionMs, std::memory_order_relaxed);
    m_lastPlaybackDurationMs.store(durationMs, std::memory_order_relaxed);
  }

  if (m_traktScrobbler)
  {
    const auto terminal = m_traktScrobbler->StopForReplacement(completed);
    if (terminal.status == KODI::JUMPGATE::JumpgateHistoryTerminalStatus::Rejected)
      CLog::Log(LOGWARNING, "CXBMCApp: Bridge history terminal event was rejected");
  }

  const int64_t observedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
  m_playbackHistoryState.UpdateProgress(terminal->generation, positionMs, durationMs, observedAtMs);
  SavePairedPlaybackHistory(completed, terminal->generation);
  m_playbackResultState.Capture(terminal->generation, positionMs, durationMs);
  m_playbackResultState.Finish(terminal->generation, completed);
}

void CXBMCApp::QueuePendingExternalPlayerResult()
{
  std::optional<KODI::JUMPGATE::JumpgatePlaybackResultOwner> owner;
  {
    auto lifecycleOperation = m_playbackResultState.BeginLifecycleOperation();
    owner = m_playbackResultState.CurrentOwner(lifecycleOperation);
  }
  if (!owner)
    return;

  QueueExternalPlayerResult(owner->generation, owner->requestId,
                            m_wasStandalone.load(std::memory_order_relaxed));
}

void CXBMCApp::DeliverPendingExternalPlayerResult()
{
  QueuePendingExternalPlayerResult();
}

void CXBMCApp::DeliverPendingExternalPlayerResult(
    KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation)
{
  const auto result = m_playbackResultState.TakeFinished(lifecycleOperation);
  if (!result)
    return;

  const bool wasStandalone = m_wasStandalone.load(std::memory_order_relaxed);
  const bool exited = ExitExternalPlayerMode(*result, lifecycleOperation);
  const bool reset = m_playbackResultState.Reset(lifecycleOperation, result->generation);
  if (exited && reset && wasStandalone)
    HandoffWarmExternalPlayerTask(result->generation, result->requestId);
}

bool CXBMCApp::ExitExternalPlayerMode(
    const KODI::JUMPGATE::JumpgatePlaybackResult& result,
    KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation)
{
  // The terminal announcement captured this generation's last stable position.
  // Do not query ApplicationPlayer here because it may already be shutting down.

  if (!lifecycleOperation.OwnsGeneration(result.generation) ||
      !m_playbackResultState.IsCurrent(result.generation) ||
      !m_externalPlayerMode.load(std::memory_order_relaxed))
    return false;

  const int64_t posMs = result.positionMs;
  const int64_t durMs = result.durationMs;

  CLog::Log(LOGINFO, "CXBMCApp: Exiting external player mode (completed={}, pos={}, dur={})",
            result.completed, posMs, durMs);

  // Save resume position before exiting
  SaveResumePosition(result.completed);
  StopJumpgateSubtitleController(false, false);
  ReleasePlaybackSourceClaim(result.completed);
  StopPlaybackClaimCoordinator(false);

  bool wasStandalone = m_wasStandalone.load(std::memory_order_relaxed);

  if (wasStandalone)
  {
    CLog::Log(LOGINFO, "CXBMCApp: Returning to standalone mode (warm transition)");

    // Call Java-side with wasStandalone=true so it does NOT kill the process
    call_method<void>(m_context, "exitExternalPlayerMode", "(JLjava/lang/String;JJZZ)V",
                      static_cast<jlong>(result.generation), jcast<jhstring>(result.requestId),
                      static_cast<jlong>(posMs), static_cast<jlong>(durMs),
                      static_cast<jboolean>(result.completed), static_cast<jboolean>(true));

    // Clean up C++ side external player state
    ReturnToStandaloneMode();

  }
  else
  {
    // Java schedules a delayed process kill for cold exits. Seal this native
    // instance before returning from the lifecycle operation so a waiting
    // onNewIntent cannot admit work that the old generation will kill.
    m_playbackResultState.CloseAdmissions();
    SetExternalPlayerMode(false); // prevent re-entry

    // Call Java-side with wasStandalone=false (cold launch: finish + killProcess)
    call_method<void>(m_context, "exitExternalPlayerMode", "(JLjava/lang/String;JJZZ)V",
                      static_cast<jlong>(result.generation), jcast<jhstring>(result.requestId),
                      static_cast<jlong>(posMs), static_cast<jlong>(durMs),
                      static_cast<jboolean>(result.completed), static_cast<jboolean>(false));
  }
  return true;
}

void CXBMCApp::SetExternalPlayerMode(bool mode)
{
  m_externalPlayerMode.store(mode, std::memory_order_relaxed);
  CJNIMainActivity::GetJumpgateBackDispatcher().SetExternalMode(m_jumpgateBackLifecycleToken, mode);
}

void CXBMCApp::ReturnToStandaloneMode()
{
  CLog::Log(LOGINFO, "CXBMCApp: Returning to standalone mode");

  ReleasePlaybackSourceClaim();
  StopJumpgateSubtitleController(false);
  StopPlaybackClaimCoordinator(true);

  // Keep the object alive so concurrent lifecycle readers cannot observe a
  // freed pointer; Deinitialize removes its announcer and disables all work.
  if (m_traktScrobbler)
    m_traktScrobbler->Deinitialize();

  // Deinitialize and destroy SubtitleDownloader
  if (m_subtitleDownloader)
  {
    m_subtitleDownloader->Deinitialize();
    m_subtitleDownloader.reset();
  }
  m_jumpgateSubtitleController.reset();

  // Reset external player state
  SetExternalPlayerMode(false);
  m_resumePositionMs.store(0, std::memory_order_relaxed);
  m_resumeApplied.store(false, std::memory_order_relaxed);
  m_externalPlaybackStartedGeneration.store(0, std::memory_order_relaxed);
  m_externalPlaybackStartedAtSteadyMs.store(0, std::memory_order_relaxed);
  m_lastPlaybackTimeMs.store(0, std::memory_order_relaxed);
  m_lastPlaybackDurationMs.store(0, std::memory_order_relaxed);
  m_wasStandalone.store(false, std::memory_order_relaxed);

  CLog::Log(
      LOGINFO,
      "CXBMCApp: Returned to standalone mode, TraktScrobbler and SubtitleDownloader deinitialized");
}

void CXBMCApp::HandoffWarmExternalPlayerTask(uint64_t generation,
                                             const std::string& requestId)
{
  call_method<void>(m_context, "handoffWarmExternalPlayerTask", "(JJLjava/lang/String;)V",
                    static_cast<jlong>(m_jumpgateBackLifecycleToken),
                    static_cast<jlong>(generation), jcast<jhstring>(requestId));
}

bool CXBMCApp::SavePairedPlaybackHistory(bool explicitEnd, uint64_t generation)
{
  if (generation == 0)
  {
    std::unique_lock lock(m_playbackClaimMutex);
    generation = m_playbackClaimGeneration;
  }
  if (!m_jumpgatePlaybackHistoryStore)
    return false;

  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  std::optional<KODI::JUMPGATE::JumpgatePlaybackHistoryEntry> entry =
      m_playbackHistoryState.Finalize(generation, explicitEnd, nowMs);
  if (!entry)
    return false;

  std::string error;
  const KODI::JUMPGATE::JumpgatePlaybackHistoryKey key =
      KODI::JUMPGATE::GetJumpgatePlaybackHistoryKey(*entry);
  if (!m_jumpgatePlaybackHistoryStore->Save(key, std::move(*entry), error))
  {
    CLog::Log(LOGERROR, "CXBMCApp: Playback history save failed: {}", error);
    return false;
  }
  CLog::Log(LOGINFO, "CXBMCApp: Playback history saved locally");
  return true;
}

void CXBMCApp::LoadAndApplyPairedPlaybackResume(uint64_t generation, bool allowPlayerSeek)
{
  const std::optional<KODI::JUMPGATE::JumpgatePlaybackResumeToken> token =
      m_playbackHistoryState.BeginResume(generation);
  if (!token)
    return;
  if (!m_jumpgatePlaybackHistoryStore)
    return;

  std::optional<KODI::JUMPGATE::JumpgatePlaybackHistoryEntry> entry;
  std::string error;
  const KODI::JUMPGATE::JumpgatePlaybackHistoryKey key{token->historyNamespace, token->profileId,
                                                       token->contentKey};
  if (!m_jumpgatePlaybackHistoryStore->Get(key, entry, error))
  {
    CLog::Log(LOGERROR, "CXBMCApp: Playback history load failed: {}", error);
    return;
  }

  const bool historyEntryFound = entry.has_value();
  const int64_t resumePosition =
      entry ? KODI::JUMPGATE::GetJumpgatePlaybackResumePosition(*entry) : 0;
  const std::optional<int64_t> previouslyAppliedPositionMs = token->previouslyAppliedPositionMs;
  m_playbackHistoryState.ApplyResume(
      *token, resumePosition,
      [this, allowPlayerSeek, historyEntryFound, generation,
       previouslyAppliedPositionMs](int64_t positionMs)
      {
        if (!historyEntryFound)
          return;
        m_resumeApplied.store(true, std::memory_order_relaxed);
        m_resumePositionMs.store(positionMs, std::memory_order_relaxed);
        if (!allowPlayerSeek ||
            (previouslyAppliedPositionMs && *previouslyAppliedPositionMs == positionMs) ||
            (positionMs == 0 &&
             (!previouslyAppliedPositionMs || *previouslyAppliedPositionMs == 0)))
          return;
        const uint64_t startedGeneration =
            m_externalPlaybackStartedGeneration.load(std::memory_order_relaxed);
        if (startedGeneration != 0 && startedGeneration != generation)
          return;
        const int64_t startedAtMs =
            m_externalPlaybackStartedAtSteadyMs.load(std::memory_order_relaxed);
        if (!KODI::JUMPGATE::IsJumpgateResumeCorrectionWithinWindow(
                startedAtMs, SteadyClockNowMs(),
                JUMPGATE_AUTHENTICATED_RESUME_CORRECTION_WINDOW_MS))
        {
          CLog::Log(LOGINFO,
                    "CXBMCApp: Ignored authenticated resume correction outside the claim window");
          return;
        }
        const auto appPlayer =
            CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
        appPlayer->SeekTime(positionMs);
        CLog::Log(LOGINFO, "CXBMCApp: Applied authenticated playback resume at {} ms", positionMs);
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                              "Resuming playback", 3000, true);
      });
}

void CXBMCApp::SaveResumePosition(bool explicitEnd)
{
  SavePairedPlaybackHistory(explicitEnd);
  if (!m_traktScrobbler)
    return;

  if (m_traktScrobbler->IsBridgeProfileBacked())
    return;

  std::string imdb = m_traktScrobbler->GetImdbId();
  if (imdb.empty())
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: SaveResumePosition - no IMDB ID, skipping");
    return;
  }

  int season = m_traktScrobbler->GetSeason();
  int episode = m_traktScrobbler->GetEpisode();

  // Build content key
  std::string key = imdb;
  if (season >= 0 && episode >= 0)
    key += ":" + std::to_string(season) + ":" + std::to_string(episode);

  int64_t posMs = m_lastPlaybackTimeMs.load(std::memory_order_relaxed);
  int64_t durMs = m_lastPlaybackDurationMs.load(std::memory_order_relaxed);

  // Determine if playback completed (>90%)
  bool completed =
      (durMs > 0 && posMs > 0 &&
       static_cast<float>(posMs) / static_cast<float>(durMs) > Jumpgate::RESUME_CLEAR_RATIO);

  // Load existing resume store
  std::string storePath = CSpecialProtocol::TranslatePath(RESUME_STORE_FILE);
  CVariant store(CVariant::VariantTypeObject);

  XFILE::CFile file;
  std::vector<uint8_t> buffer;
  if (file.LoadFile(storePath, buffer) > 0)
  {
    std::string json(buffer.begin(), buffer.end());
    CJSONVariantParser::Parse(json, store);
  }

  if (completed)
  {
    // Remove entry — content is fully watched
    if (store.isMember(key))
      store.erase(key);
    CLog::Log(LOGINFO, "CXBMCApp: Resume cleared for {} (completed)", key);
  }
  else if (posMs > 0)
  {
    // Save position
    CVariant entry(CVariant::VariantTypeObject);
    entry["position"] = posMs;
    entry["duration"] = durMs;
    entry["timestamp"] =
        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
    store[key] = entry;
    CLog::Log(LOGINFO, "CXBMCApp: Resume saved for {} - pos={} dur={}", key, posMs, durMs);
  }

  // Cleanup entries older than 30 days
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  std::vector<std::string> expiredKeys;
  for (auto it = store.begin_map(); it != store.end_map(); ++it)
  {
    int64_t ts = it->second["timestamp"].asInteger(0);
    if (ts > 0 && (now - ts) > 30 * 24 * 3600)
      expiredKeys.push_back(it->first);
  }
  for (const auto& k : expiredKeys)
    store.erase(k);

  // Write back
  std::string json;
  if (CJSONVariantWriter::Write(store, json, true))
  {
    XFILE::CFile outFile;
    if (outFile.OpenForWrite(storePath, true))
    {
      outFile.Write(json.c_str(), json.size());
      outFile.Close();
    }
  }
}

int64_t CXBMCApp::LoadResumePosition(const std::string& imdbId, int season, int episode)
{
  if (imdbId.empty())
    return 0;

  std::string key = imdbId;
  if (season >= 0 && episode >= 0)
    key += ":" + std::to_string(season) + ":" + std::to_string(episode);

  std::string storePath = CSpecialProtocol::TranslatePath(RESUME_STORE_FILE);

  XFILE::CFile file;
  std::vector<uint8_t> buffer;
  if (file.LoadFile(storePath, buffer) <= 0)
    return 0;

  std::string json(buffer.begin(), buffer.end());
  CVariant store;
  if (!CJSONVariantParser::Parse(json, store) || !store.isMember(key))
    return 0;

  const CVariant& entry = store[key];
  int64_t pos = entry["position"].asInteger(0);
  int64_t dur = entry["duration"].asInteger(0);
  int64_t ts = entry["timestamp"].asInteger(0);

  // Check expiry (30 days)
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  if (ts > 0 && (now - ts) > 30 * 24 * 3600)
    return 0;

  // If position is > 95% of duration, content was watched — start over
  if (dur > 0 && static_cast<float>(pos) / static_cast<float>(dur) > Jumpgate::RESUME_DISCARD_RATIO)
    return 0;

  return pos;
}

void CXBMCApp::OnContentIdentified()
{
  if (!m_traktScrobbler || !m_traktScrobbler->IsContentIdentified())
    return;

  uint64_t resumeGeneration = 0;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    resumeGeneration = m_playbackClaimGeneration;
  }

  std::string imdb = m_traktScrobbler->GetImdbId();
  const std::string canonicalProvider = m_traktScrobbler->GetCanonicalProvider();
  const std::string canonicalId = m_traktScrobbler->GetCanonicalId();

  std::string title = m_traktScrobbler->GetTitle();
  int year = m_traktScrobbler->GetYear();
  int season = m_traktScrobbler->GetSeason();
  int episode = m_traktScrobbler->GetEpisode();

  // Update Java-side Jumpgate overlay with identified content info (title + meta).
  UpdateLoadingOverlayContentInfo(true);

  // Show content identification toast
  std::string toastMsg;
  if (!title.empty())
  {
    toastMsg = title;
    if (year > 0)
      toastMsg += " (" + std::to_string(year) + ")";
    if (season >= 0 && episode >= 0)
      toastMsg += " S" + std::to_string(season) + "E" + std::to_string(episode);
  }
  else
  {
    toastMsg = !canonicalId.empty() ? canonicalProvider + ":" + canonicalId : imdb;
    if (season >= 0 && episode >= 0)
      toastMsg += " S" + std::to_string(season) + "E" + std::to_string(episode);
  }
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                        "Identified: " + toastMsg, 5000, true);

  // Paired resume is loaded only from the authenticated content-keyed history
  // immediately after claim application. These fallbacks are legacy-only.
  if (!m_traktScrobbler->IsBridgeProfileBacked())
  {
    int64_t savedPos = 0;
    if (!imdb.empty())
      savedPos = LoadResumePosition(imdb, season, episode);

    std::unique_lock resumeLock(m_playbackClaimMutex);
    if (resumeGeneration != m_playbackClaimGeneration)
      return;
    if (savedPos > 0 && m_resumePositionMs.load(std::memory_order_relaxed) <= 0)
    {
      auto& components = CServiceBroker::GetAppComponents();
      const auto appPlayer = components.GetComponent<CApplicationPlayer>();
      if (appPlayer->GetTime() < 60000)
      {
        appPlayer->SeekTime(savedPos);
        CLog::Log(LOGINFO,
                  "CXBMCApp: Late resume to {} ms (content identified after playback start)",
                  savedPos);
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                              "Resuming playback", 3000, true);
      }
    }
    m_resumeApplied.store(true, std::memory_order_relaxed);
  }
  else
  {
    std::unique_lock resumeLock(m_playbackClaimMutex);
    if (resumeGeneration != m_playbackClaimGeneration)
      return;
    m_resumeApplied.store(true, std::memory_order_relaxed);
  }
  // Propagate to SubtitleDownloader for late subtitle download (F-011)
  // Always auto-download regardless of playback position (per user decision)
  // Silent download -- no toast notification (per user decision)
  if (m_subtitleDownloader)
  {
    m_subtitleDownloader->SetContentInfo(imdb, title, year, season, episode);
    m_subtitleDownloader->TriggerSearch();
  }
}

void CXBMCApp::UpdateLoadingOverlayContentInfo(bool force)
{
  if (!m_traktScrobbler)
    return;

  std::string imdb = m_traktScrobbler->GetImdbId();
  std::string showOrMovieTitle = m_traktScrobbler->GetTitle();
  std::string episodeTitle = m_traktScrobbler->GetEpisodeTitle();
  std::string logoUrl = m_traktScrobbler->GetLogoUrl();
  int year = m_traktScrobbler->GetYear();
  int season = m_traktScrobbler->GetSeason();
  int episode = m_traktScrobbler->GetEpisode();

  const bool isEpisode = (season >= 0 && episode >= 0);

  std::string title;
  std::string meta;

  if (isEpisode)
  {
    // Title: episode title, Meta: show title • SxxEyy
    title = !episodeTitle.empty() ? episodeTitle
                                  : (!showOrMovieTitle.empty() ? showOrMovieTitle : imdb);

    char seBuf[16];
    std::snprintf(seBuf, sizeof(seBuf), "S%02dE%02d", season, episode);

    meta = showOrMovieTitle;
    if (!meta.empty())
      meta += " - ";
    meta += seBuf;
  }
  else
  {
    title = !showOrMovieTitle.empty() ? showOrMovieTitle : imdb;
    meta = "MOVIE";
    if (year > 0)
      meta += " - " + std::to_string(year);
  }

  if (title.empty())
    title = "Identifying...";

  if (!force && title == m_lastOverlayTitle && meta == m_lastOverlayMeta &&
      logoUrl == m_lastOverlayLogoUrl)
    return;

  m_lastOverlayTitle = title;
  m_lastOverlayMeta = meta;
  m_lastOverlayLogoUrl = logoUrl;

  call_method<void>(m_context, "updateLoadingOverlayContentInfo",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                    jcast<jhstring>(title), jcast<jhstring>(meta), jcast<jhstring>(logoUrl));
}

// --- Secure profile runtime and settings ---

uint64_t CXBMCApp::QueuePlaybackSourceClaim(const std::string& rawLaunchUri, int64_t launchedAtMs)
{
  // Replacement must snapshot the old generation before the generation and
  // playback counters are advanced/reset for the incoming source.
  SavePairedPlaybackHistory(false);
  ReleasePlaybackSourceClaim();

  uint64_t generation = 0;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    generation = ++m_playbackClaimGeneration;
    m_pendingPlaybackClaim.reset();
  }
  m_externalPlaybackStartedGeneration.store(0, std::memory_order_relaxed);
  m_externalPlaybackStartedAtSteadyMs.store(0, std::memory_order_relaxed);
  m_playbackHistoryState.AdvanceGeneration(generation);

  std::vector<std::string> fingerprints;
  const bool fingerprinted =
      !rawLaunchUri.empty() &&
      KODI::UTILITY::CJumpgateSourceFingerprint::FingerprintPlaybackUrl(rawLaunchUri, fingerprints);
  if (!m_playbackHistoryState.ActivateLocalSource(generation, fingerprints, rawLaunchUri,
                                                  std::max<int64_t>(0, launchedAtMs)))
  {
    CLog::Log(LOGERROR, "CXBMCApp: Device-local playback history could not be activated");
  }

  if (!fingerprinted || launchedAtMs <= 0)
  {
    CLog::Log(LOGWARNING,
              "CXBMCApp: Playback source cannot be fingerprinted; paired Trakt is fail-closed");
    return generation;
  }

  PendingPlaybackClaim pending;
  pending.fingerprints = std::move(fingerprints);
  pending.intentUrlHash =
      KODI::UTILITY::CDigest::Calculate(KODI::UTILITY::CDigest::Type::SHA256, rawLaunchUri);
  pending.launchedAtMs = launchedAtMs;

  std::unique_lock lock(m_playbackClaimMutex);
  pending.generation = generation;
  if (generation == m_playbackClaimGeneration)
    m_pendingPlaybackClaim = std::move(pending);
  return generation;
}

std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> CXBMCApp::
    BeginExternalPlaybackAdmission(
        const std::string& rawLaunchUri,
        int64_t launchedAtMs,
        std::string resultRequestId,
        KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation)
{
  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  if (m_playbackResultState.AdmissionsClosed() || !authorityTransaction.CanAdmitPlayback())
    return std::nullopt;

  const uint64_t generation = QueuePlaybackSourceClaim(rawLaunchUri, launchedAtMs);
  const auto admission = authorityTransaction.CommitAdmission(generation);
  if (admission &&
      !m_playbackResultState.Begin(lifecycleOperation, generation, std::move(resultRequestId)))
  {
    authorityTransaction.CancelPendingAdmissionByToken(admission->token);
    return std::nullopt;
  }
  return admission;
}

void CXBMCApp::CommitExternalPlaybackOpenFailure(uint64_t token)
{
  if (token == 0)
    return;

  uint64_t generation = 0;
  std::string requestId;
  {
    std::lock_guard lock(m_externalPlaybackQueueMutex);
    if (m_externalPlaybackDispatchToken == token)
    {
      generation = m_externalPlaybackDispatchGeneration;
      requestId = m_externalPlaybackDispatchRequestId;
      m_externalPlaybackDispatchGeneration = 0;
      m_externalPlaybackDispatchToken = 0;
      m_externalPlaybackDispatchRequestId.clear();
      m_externalPlaybackDispatchPayload.reset();
      if (m_pendingExternalPlaybackStopToken == token)
      {
        m_pendingExternalPlaybackStopGeneration = 0;
        m_pendingExternalPlaybackStopToken = 0;
      }
    }
  }
  const bool failed = CommitExternalPlaybackAdmissionFailure(token);
  if (failed && generation != 0 && !requestId.empty())
  {
    QueueExternalPlayerResult(generation, std::move(requestId),
                              m_wasStandalone.load(std::memory_order_relaxed));
  }
}

std::optional<uint64_t> CXBMCApp::BeginExternalPlaybackContinuation()
{
  auto lifecycleOperation = m_playbackResultState.TryBeginLifecycleOperation();
  if (!lifecycleOperation)
    return std::nullopt;
  if (!m_externalPlayerMode.load(std::memory_order_relaxed) ||
      m_playbackResultState.AdmissionsClosed())
  {
    return std::nullopt;
  }

  const uint64_t generation = m_playbackResultState.CurrentGeneration();
  if (generation == 0)
    return std::nullopt;

  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> admission;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    admission = authorityTransaction.CommitAdmission(generation);
    if (admission && !m_playbackResultState.Begin(*lifecycleOperation, generation))
    {
      authorityTransaction.CancelPendingAdmissionByToken(admission->token);
      admission.reset();
    }
  }
  if (!admission)
    return std::nullopt;

  if (m_traktScrobbler)
    m_traktScrobbler->SetPlaybackGeneration(generation, admission->token);
  return admission->token;
}

bool CXBMCApp::IsLatestExternalPlaybackAdmission(uint64_t token)
{
  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  return authorityTransaction.IsLatestPendingAdmission(token);
}

bool CXBMCApp::CommitExternalPlaybackAdmissionFailure(uint64_t token)
{
  if (token == 0)
    return false;

  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> canceled;
  bool superseded = false;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    canceled = authorityTransaction.CancelPendingAdmissionByToken(token);
    superseded = canceled && authorityTransaction.HasNewerPlayback(canceled->token);
    if (canceled && !superseded)
    {
      m_playbackResultState.Capture(canceled->generation, 0, 0);
      m_playbackResultState.Finish(canceled->generation, false);
    }
  }

  if (canceled && !superseded && m_traktScrobbler)
    m_traktScrobbler->CancelPlaybackGeneration(canceled->generation, canceled->token);
  if (canceled && !superseded && m_jumpgateSubtitleController)
    m_jumpgateSubtitleController->OnPlaybackTerminal(canceled->generation);
  return canceled.has_value() && !superseded;
}

void CXBMCApp::DeliverRejectedExternalPlaybackResult(
    std::string resultRequestId,
    KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation)
{
  const uint64_t generation =
      m_rejectedPlaybackResultGeneration.fetch_add(1, std::memory_order_relaxed);
  if (m_playbackResultState.AdmissionsClosed() ||
      m_playbackResultState.CurrentOwner(lifecycleOperation))
  {
    call_method<void>(m_context, "rejectExternalPlayerResult", "(JLjava/lang/String;)V",
                      static_cast<jlong>(generation), jcast<jhstring>(resultRequestId));
    return;
  }

  if (!m_playbackResultState.Begin(lifecycleOperation, generation, std::move(resultRequestId)))
    return;
  m_playbackResultState.Finish(generation, 0, 0, false);
  const auto result = m_playbackResultState.TakeFinished(lifecycleOperation);
  if (!result)
    return;

  const bool wasColdLaunch = m_externalPlayerMode.load(std::memory_order_relaxed);
  const bool returnToStandalone = m_wasStandalone.load(std::memory_order_relaxed) || !wasColdLaunch;
  const auto owner = m_playbackResultState.CurrentOwner(lifecycleOperation);
  if (!owner)
    return;
  static_cast<void>(call_method<jboolean>(m_context, "beginExternalPlayerMode",
                                          "(JLjava/lang/String;Z)Z", static_cast<jlong>(generation),
                                          jcast<jhstring>(owner->requestId),
                                          static_cast<jboolean>(returnToStandalone)));
  if (!returnToStandalone)
  {
    m_playbackResultState.CloseAdmissions();
    SetExternalPlayerMode(false);
  }
  call_method<void>(m_context, "exitExternalPlayerMode", "(JLjava/lang/String;JJZZ)V",
                    static_cast<jlong>(generation), jcast<jhstring>(owner->requestId),
                    static_cast<jlong>(0), static_cast<jlong>(0), static_cast<jboolean>(false),
                    static_cast<jboolean>(returnToStandalone));
  const bool reset = m_playbackResultState.Reset(lifecycleOperation, generation);
  if (reset && returnToStandalone)
    HandoffWarmExternalPlayerTask(generation, owner->requestId);
}

void CXBMCApp::ProcessPlaybackSourceClaim()
{
  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  std::optional<PendingPlaybackClaim> pending;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (m_pendingPlaybackClaim)
    {
      pending = std::move(m_pendingPlaybackClaim);
      m_pendingPlaybackClaim.reset();
    }
    if (!m_playbackClaimCoordinator)
    {
      m_playbackClaimCoordinator =
          std::make_unique<KODI::JUMPGATE::CJumpgatePlaybackClaimCoordinator>(
              std::make_shared<CAndroidPlaybackClaimTransport>());
    }
  }

  if (pending)
  {
    KODI::JUMPGATE::ActiveProfile active;
    if (m_jumpgateProfileRuntime)
      active = m_jumpgateProfileRuntime->GetActive();

    if (active.selected && active.sourceBacked && active.credentialsValid &&
        !active.bridgeOrigin.empty() && !active.deviceToken.empty())
    {
      KODI::JUMPGATE::PlaybackClaimRequest request;
      request.bridgeOrigin = active.bridgeOrigin;
      request.deviceToken = active.deviceToken;
      request.attemptId = StringUtils::CreateUUID();
      request.fingerprints = std::move(pending->fingerprints);
      request.intentUrlHash = std::move(pending->intentUrlHash);
      request.launchedAt = pending->launchedAtMs;
      request.client = KODI::JUMPGATE::PlaybackClaimClientInfo{"android", JUMPGATE_VERSION};

      std::unique_lock lock(m_playbackClaimMutex);
      if (pending->generation == m_playbackClaimGeneration && m_playbackClaimCoordinator &&
          m_playbackClaimCoordinator->QueueClaim(pending->generation, std::move(request)))
      {
        m_submittedPlaybackClaimGeneration = pending->generation;
        m_submittedPlaybackClaimProfileId = active.profileId;
        m_submittedPlaybackClaimDeviceId = active.deviceId;
        m_submittedPlaybackClaimOrigin = active.bridgeOrigin;
      }
    }
    else
    {
      CLog::Log(LOGWARNING,
                "CXBMCApp: No valid source-backed profile; authenticated claim was not sent");
    }
    active.ClearSecrets();
  }

  std::optional<KODI::JUMPGATE::PlaybackClaimCompletion> completion;
  std::string expectedProfileId;
  std::string expectedDeviceId;
  std::string expectedOrigin;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (m_playbackClaimCoordinator)
      completion = m_playbackClaimCoordinator->TakeCompletion();
    if (completion && completion->generation == m_submittedPlaybackClaimGeneration)
    {
      expectedProfileId = m_submittedPlaybackClaimProfileId;
      expectedDeviceId = m_submittedPlaybackClaimDeviceId;
      expectedOrigin = m_submittedPlaybackClaimOrigin;
    }
  }
  if (!completion)
    return;

  const uint64_t generation = completion->generation;
  const bool generationCurrent = generation != 0 && generation == m_playbackClaimGeneration &&
                                 generation == m_submittedPlaybackClaimGeneration;
  KODI::JUMPGATE::ActiveProfile active;
  if (m_jumpgateProfileRuntime)
    active = m_jumpgateProfileRuntime->GetActive();
  const bool profileCurrent = generationCurrent && active.selected && active.sourceBacked &&
                              active.credentialsValid && active.profileId == expectedProfileId &&
                              active.deviceId == expectedDeviceId &&
                              active.bridgeOrigin == expectedOrigin;

  if (!profileCurrent)
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (m_playbackClaimCoordinator)
      m_playbackClaimCoordinator->RejectCompletion(generation);
    completion->result.ClearSensitive();
    active.ClearSecrets();
    CLog::Log(LOGINFO, "CXBMCApp: Discarded stale playback claim after authority change");
    return;
  }

  if (!completion->result.IsClaimed())
  {
    const auto status = completion->result.status;
    {
      std::unique_lock lock(m_playbackClaimMutex);
      if (m_playbackClaimCoordinator)
        m_playbackClaimCoordinator->AcceptCompletion(generation);
    }
    completion->result.ClearSensitive();
    active.ClearSecrets();
    CLog::Log(LOGWARNING, "CXBMCApp: Playback source claim failed closed ({})",
              PlaybackClaimStatusName(status));
    if (status == KODI::JUMPGATE::PlaybackClaimStatus::AuthenticationFailure)
    {
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Warning, "Jumpgate",
          "Profile authorization expired; pair this profile again", 5000, true);
    }
    return;
  }

  auto context =
      KODI::JUMPGATE::CJumpgatePlaybackContextParser::Parse(completion->result.claim.context);
  if (!context || context->profileId != expectedProfileId ||
      context->profileId != active.profileId || !context->contentKey)
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (m_playbackClaimCoordinator)
      m_playbackClaimCoordinator->RejectCompletion(generation);
    completion->result.ClearSensitive();
    active.ClearSecrets();
    CLog::Log(LOGERROR,
              "CXBMCApp: Bridge returned a playback context outside the active profile authority");
    return;
  }

  std::string provider;
  std::string canonicalId;
  std::string mediaType{"movie"};
  int season = context->display.season.value_or(-1);
  int episode = context->display.episode.value_or(-1);
  if (context->canonicalIdentity)
  {
    provider = KODI::JUMPGATE::ToString(context->canonicalIdentity->provider);
    canonicalId = context->canonicalIdentity->id;
    mediaType = KODI::JUMPGATE::ToString(context->canonicalIdentity->mediaType);
    season = context->canonicalIdentity->season.value_or(-1);
    episode = context->canonicalIdentity->episode.value_or(-1);
  }
  const std::string title = context->display.title.value_or("");
  const std::string logoUrl = context->display.logo.value_or("");
  const int year = context->display.year.value_or(0);

  const bool applied =
      m_traktScrobbler &&
      m_traktScrobbler->SetClaimedContentInfo(
          generation, active.profileId, active.deviceId, active.bridgeOrigin, active.deviceToken,
          completion->result.claim.sessionId, completion->result.claim.historyGrant,
          completion->result.claim.historyGrantKind, completion->result.claim.sessionRevision,
          provider, canonicalId, mediaType, title, logoUrl, year, season, episode,
          context->traktEligible);
  if (!applied)
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (m_playbackClaimCoordinator)
      m_playbackClaimCoordinator->RejectCompletion(generation);
    completion->result.ClearSensitive();
    active.ClearSecrets();
    CLog::Log(LOGINFO, "CXBMCApp: Discarded claim that lost the playback generation race");
    return;
  }

  bool accepted = false;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    const bool transferred =
        m_playbackClaimCoordinator && m_playbackClaimCoordinator->AcceptCompletion(generation);
    if (transferred && generation == m_playbackClaimGeneration)
    {
      m_activePlaybackClaimSessionId = completion->result.claim.sessionId;
      m_activePlaybackClaimProfileId = active.profileId;
      m_activePlaybackClaimDeviceId = active.deviceId;
      m_activePlaybackClaimOrigin = active.bridgeOrigin;
      accepted = true;
    }
  }
  completion->result.ClearSensitive();
  active.ClearSecrets();
  if (!accepted)
  {
    m_traktScrobbler->StopForReplacement(false);
    CLog::Log(LOGINFO, "CXBMCApp: Bound claim was terminated after losing its generation race");
    return;
  }

  KODI::JUMPGATE::JumpgatePlaybackHistoryIdentity historyIdentity;
  historyIdentity.generation = generation;
  historyIdentity.historyNamespace =
      KODI::JUMPGATE::JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
  historyIdentity.profileId = context->profileId;
  historyIdentity.contentKey = *context->contentKey;
  historyIdentity.canonicalIdentity = context->canonicalIdentity;
  historyIdentity.display.title = context->display.title;
  historyIdentity.display.year = context->display.year;
  historyIdentity.display.season = context->display.season;
  historyIdentity.display.episode = context->display.episode;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    if (generation != m_playbackClaimGeneration)
    {
      lock.unlock();
      ReleasePlaybackSourceClaim();
      CLog::Log(LOGINFO, "CXBMCApp: Discarded history identity after generation change");
      return;
    }
  }
  if (!m_playbackHistoryState.Promote(std::move(historyIdentity)))
  {
    ReleasePlaybackSourceClaim();
    CLog::Log(LOGERROR, "CXBMCApp: Authenticated history identity could not be promoted");
    return;
  }

  LoadAndApplyPairedPlaybackResume(generation);
  QueueJumpgateSubtitles(generation);
  UpdateLoadingOverlayContentInfo(true);
  OnContentIdentified();
  CLog::Log(LOGINFO, "CXBMCApp: Authenticated playback context applied");
}

void CXBMCApp::QueueJumpgateSubtitles(uint64_t generation)
{
  if (!m_externalPlayerMode.load(std::memory_order_relaxed) || !m_jumpgateProfileRuntime ||
      !m_jumpgateSubtitleController || generation == 0)
  {
    return;
  }

  KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
  std::string sessionId;
  bool claimCurrent = false;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    claimCurrent = generation == m_playbackClaimGeneration &&
                   generation == m_submittedPlaybackClaimGeneration && active.selected &&
                   active.sourceBacked && active.credentialsValid &&
                   active.profileId == m_activePlaybackClaimProfileId &&
                   active.deviceId == m_activePlaybackClaimDeviceId &&
                   active.bridgeOrigin == m_activePlaybackClaimOrigin &&
                   !m_activePlaybackClaimSessionId.empty();
    if (claimCurrent)
      sessionId = m_activePlaybackClaimSessionId;
  }

  const auto provider = KODI::JUMPGATE::SelectAndroidJumpgateSubtitleProvider(
      true, active.selected && active.sourceBacked, active.credentialsValid,
      active.subtitlesEnabled, claimCurrent);
  if (provider != KODI::JUMPGATE::AndroidJumpgateSubtitleProvider::Bridge)
  {
    active.ClearSecrets();
    ClearSensitiveString(sessionId);
    return;
  }

  KODI::JUMPGATE::JumpgateSubtitleRequest request{
      {generation, active.profileId, active.deviceId, active.bridgeOrigin, sessionId},
      KODI::JUMPGATE::CJumpgateSubtitleBearerAuthority{std::move(active.deviceToken)},
      ParseJumpgateSubtitleLanguages(active.subtitleLanguages)};
  active.ClearSecrets();
  ClearSensitiveString(sessionId);
  if (!m_jumpgateSubtitleController->Queue(std::move(request)))
    CLog::Log(LOGWARNING, "CXBMCApp: Bridge subtitle discovery was not queued");
}

void CXBMCApp::ProcessJumpgateSubtitles()
{
  if (!m_externalPlayerMode.load(std::memory_order_relaxed) || !m_jumpgateProfileRuntime ||
      !m_jumpgateSubtitleController)
  {
    return;
  }

  KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
  KODI::JUMPGATE::JumpgateSubtitleBinding binding;
  bool current = false;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    current = m_playbackClaimGeneration != 0 && active.selected && active.sourceBacked &&
              active.credentialsValid && active.subtitlesEnabled &&
              active.profileId == m_activePlaybackClaimProfileId &&
              active.deviceId == m_activePlaybackClaimDeviceId &&
              active.bridgeOrigin == m_activePlaybackClaimOrigin &&
              !m_activePlaybackClaimSessionId.empty();
    if (current)
    {
      binding = {m_playbackClaimGeneration, active.profileId, active.deviceId, active.bridgeOrigin,
                 m_activePlaybackClaimSessionId};
    }
  }
  active.ClearSecrets();
  if (current)
    m_jumpgateSubtitleController->Process(binding);
  ClearSensitiveString(binding.sessionId);
}

void CXBMCApp::StopJumpgateSubtitleController(bool playerMayRead, bool waitForCompletion)
{
  if (m_jumpgateSubtitleController)
    m_jumpgateSubtitleController->Stop(playerMayRead, waitForCompletion);
}

bool CXBMCApp::ReleasePlaybackSourceClaim(bool completed)
{
  if (m_traktScrobbler)
  {
    const auto terminal = m_traktScrobbler->StopForReplacement(completed);
    if (terminal.status == KODI::JUMPGATE::JumpgateHistoryTerminalStatus::Rejected)
      CLog::Log(LOGWARNING, "CXBMCApp: Playback claim terminal history event was rejected");
  }

  {
    std::unique_lock lock(m_playbackClaimMutex);
    ClearSensitiveString(m_activePlaybackClaimSessionId);
    m_activePlaybackClaimProfileId.clear();
    m_activePlaybackClaimDeviceId.clear();
    m_activePlaybackClaimOrigin.clear();
  }
  return true;
}

void CXBMCApp::StopPlaybackClaimCoordinator(bool drainRelease)
{
  std::unique_ptr<KODI::JUMPGATE::CJumpgatePlaybackClaimCoordinator> coordinator;
  uint64_t generation = 0;
  {
    std::unique_lock lock(m_playbackClaimMutex);
    generation = ++m_playbackClaimGeneration;
    m_pendingPlaybackClaim.reset();
    coordinator = std::move(m_playbackClaimCoordinator);
    ClearSensitiveString(m_activePlaybackClaimSessionId);
    m_activePlaybackClaimProfileId.clear();
    m_activePlaybackClaimDeviceId.clear();
    m_activePlaybackClaimOrigin.clear();
    m_submittedPlaybackClaimGeneration = 0;
    m_submittedPlaybackClaimProfileId.clear();
    m_submittedPlaybackClaimDeviceId.clear();
    m_submittedPlaybackClaimOrigin.clear();
  }
  m_playbackHistoryState.AdvanceGeneration(generation);
  if (coordinator)
    coordinator->Stop(drainRelease);
}

bool CXBMCApp::InitializeJumpgateProfileRuntime()
{
  if (!m_jumpgateProfileStorage)
    m_jumpgateProfileStorage = std::make_unique<KODI::JUMPGATE::CJumpgateProfileStorage>(
        "special://profile/jumpgate_settings.json");
  if (!m_jumpgatePlaybackHistoryStorage)
  {
    m_jumpgatePlaybackHistoryStorage = std::make_unique<KODI::JUMPGATE::CJumpgateProfileStorage>(
        "special://profile/jumpgate_playback_history.json");
  }
  if (!m_jumpgatePlaybackHistoryStore)
  {
    m_jumpgatePlaybackHistoryStore =
        std::make_unique<KODI::JUMPGATE::CJumpgatePlaybackHistoryStore>(
            *m_jumpgatePlaybackHistoryStorage);
  }
  if (!m_jumpgateCredentialStore)
  {
    m_jumpgateCredentialStore =
        std::make_unique<KODI::JUMPGATE::CAndroidJumpgateCredentialStore>(*this);
  }
  if (!m_jumpgateProfileRuntime)
  {
    m_jumpgateProfileRuntime = std::make_unique<KODI::JUMPGATE::CJumpgateProfileRuntime>(
        *m_jumpgateProfileStorage, *m_jumpgateCredentialStore);
  }

  std::string error;
  if (!m_jumpgateProfileRuntime->Initialize(error))
  {
    CLog::Log(LOGERROR, "CXBMCApp: Jumpgate profile runtime failed closed: {}", error);
    return false;
  }
  return true;
}

void CXBMCApp::ApplyActiveJumpgateProfile()
{
  if (!m_jumpgateProfileRuntime || !m_traktScrobbler)
    return;

  KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
  if (active.selected)
  {
    m_traktScrobbler->SetBridgeProfile(active.profileId, active.deviceId, active.bridgeOrigin,
                                       active.sourceBacked, active.credentialsValid,
                                       active.traktEnabled);
  }
  else
  {
    m_traktScrobbler->ClearBridgeProfile();
  }

  if (m_subtitleDownloader)
    m_subtitleDownloader->SetLanguages(active.subtitleLanguages);
  active.ClearSecrets();
}

std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Token> CXBMCApp::
    BeginJumpgateProfileAuthorityTransition(std::string& error)
{
  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  const auto token = authorityTransaction.BeginProfileMutation();
  if (!token)
  {
    error = "Stop playback before changing Jumpgate profile authority";
    return std::nullopt;
  }
  error.clear();
  return token;
}

void CXBMCApp::PrepareJumpgateProfileAuthorityTransition()
{
  SavePairedPlaybackHistory(false);
  StopJumpgateSubtitleController(false);
  const bool releaseReady = ReleasePlaybackSourceClaim();
  if (!releaseReady)
  {
    CLog::Log(LOGERROR,
              "CXBMCApp: Profile transition withheld playback release after terminal timeout");
  }
  if (m_traktScrobbler)
    m_traktScrobbler->Deinitialize(false);
  StopPlaybackClaimCoordinator(true);
  m_resumeApplied.store(false, std::memory_order_relaxed);
  m_resumePositionMs.store(0, std::memory_order_relaxed);
  m_externalPlaybackStartedGeneration.store(0, std::memory_order_relaxed);
  m_externalPlaybackStartedAtSteadyMs.store(0, std::memory_order_relaxed);
}

bool CXBMCApp::FinishJumpgateProfileAuthorityTransition(
    KODI::JUMPGATE::CJumpgatePlaybackAuthority::Token token, bool committed)
{
  // Reconfigure the scrobbler before unblocking intent admission. This call is
  // deliberately outside the authority transaction and may acquire its own locks.
  ApplyActiveJumpgateProfile();
  if (m_traktScrobbler && m_externalPlayerMode.load(std::memory_order_relaxed))
    m_traktScrobbler->Initialize();
  if (m_externalPlayerMode.load(std::memory_order_relaxed))
  {
    if (!m_jumpgateSubtitleController)
      m_jumpgateSubtitleController =
          std::make_unique<KODI::JUMPGATE::CAndroidJumpgateSubtitleController>();
    else
      m_jumpgateSubtitleController->Restart();
    m_jumpgateSubtitleController->SweepStartupOrphans();
  }

  auto authorityTransaction = m_playbackAuthority.BeginTransaction();
  const bool finished = committed ? authorityTransaction.CommitProfileMutation(token)
                                  : authorityTransaction.RollbackProfileMutation(token);
  if (!finished)
    CLog::Log(LOGERROR, "CXBMCApp: Stale Jumpgate profile authority transition token");
  return finished;
}

std::string CXBMCApp::GetSettingString(const std::string& key, const std::string& defaultVal) const
{
  if (!m_jumpgateProfileRuntime)
    return defaultVal;
  KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
  std::string value = defaultVal;
  if (key == "subtitle_languages")
    value = active.subtitleLanguages;
  active.ClearSecrets();
  return value;
}

bool CXBMCApp::GetSettingBool(const std::string& key, bool defaultVal) const
{
  if (!m_jumpgateProfileRuntime)
    return defaultVal;
  KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
  bool value = defaultVal;
  if (key == "trakt_enabled")
    value = active.traktEnabled;
  else if (key == "subtitles_enabled")
    value = active.subtitlesEnabled;
  else if (key == "auto_update_check")
    value = active.autoUpdateCheck;
  active.ClearSecrets();
  return value;
}

void CXBMCApp::SetSetting(const std::string& key, const std::string& value)
{
  if (!m_jumpgateProfileRuntime)
    return;
  std::string error;
  const auto transitionToken = BeginJumpgateProfileAuthorityTransition(error);
  if (!transitionToken)
  {
    CLog::Log(LOGWARNING, "CXBMCApp: Jumpgate setting update blocked: {}", error);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate", error, 5000,
                                          true);
    return;
  }
  PrepareJumpgateProfileAuthorityTransition();
  const auto result = m_jumpgateProfileRuntime->SetActiveSetting(key, CVariant{value}, error);
  if (!result.IsCommitted())
  {
    FinishJumpgateProfileAuthorityTransition(*transitionToken, false);
    CLog::Log(LOGERROR, "CXBMCApp: Jumpgate setting update rejected: {}", error);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate", error, 5000, true);
    return;
  }
  std::string outcome;
  if (!result.IsFullyApplied())
  {
    std::string reloadError;
    if (m_jumpgateProfileRuntime->Reload(reloadError))
      outcome = "Setting committed and local authority was reloaded";
    else
    {
      outcome =
          "Setting committed; authority failed closed and reload remains pending: " + reloadError;
      CLog::Log(LOGERROR, "CXBMCApp: Setting committed but runtime reload failed closed: {}",
                reloadError);
    }
  }
  FinishJumpgateProfileAuthorityTransition(*transitionToken, true);
  if (outcome.empty())
    outcome = key == "subtitle_languages" ? "Subtitle languages: " + value : "Setting updated";
  CGUIDialogKaiToast::QueueNotification(result.IsFullyApplied() ? CGUIDialogKaiToast::Info
                                                                : CGUIDialogKaiToast::Warning,
                                        "Jumpgate", outcome, 4000, true);
}

void CXBMCApp::SetSetting(const std::string& key, bool value)
{
  if (!m_jumpgateProfileRuntime)
    return;
  std::string error;
  const auto transitionToken = BeginJumpgateProfileAuthorityTransition(error);
  if (!transitionToken)
  {
    CLog::Log(LOGWARNING, "CXBMCApp: Jumpgate setting update blocked: {}", error);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate", error, 5000,
                                          true);
    return;
  }
  PrepareJumpgateProfileAuthorityTransition();
  const auto result = m_jumpgateProfileRuntime->SetActiveSetting(key, CVariant{value}, error);
  if (!result.IsCommitted())
  {
    FinishJumpgateProfileAuthorityTransition(*transitionToken, false);
    CLog::Log(LOGERROR, "CXBMCApp: Jumpgate setting update rejected: {}", error);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate", error, 5000, true);
    return;
  }
  std::string outcome;
  if (!result.IsFullyApplied())
  {
    std::string reloadError;
    if (m_jumpgateProfileRuntime->Reload(reloadError))
      outcome = "Setting committed and local authority was reloaded";
    else
    {
      outcome =
          "Setting committed; authority failed closed and reload remains pending: " + reloadError;
      CLog::Log(LOGERROR, "CXBMCApp: Setting committed but runtime reload failed closed: {}",
                reloadError);
    }
  }
  FinishJumpgateProfileAuthorityTransition(*transitionToken, true);
  if (outcome.empty())
    outcome = "Setting updated";
  CGUIDialogKaiToast::QueueNotification(result.IsFullyApplied() ? CGUIDialogKaiToast::Info
                                                                : CGUIDialogKaiToast::Warning,
                                        "Jumpgate", outcome, 4000, true);
}

std::string CXBMCApp::GetBridgeOriginFromUrl(const std::string& currentUrl)
{
  std::string normalized = currentUrl;
  StringUtils::Trim(normalized);
  if (normalized.empty())
    return "";

  size_t schemePos = normalized.find("://");
  if (schemePos == std::string::npos)
  {
    normalized = "https://" + normalized;
    schemePos = normalized.find("://");
  }

  if (schemePos == std::string::npos)
    return "";

  std::string scheme = StringUtils::ToLower(normalized.substr(0, schemePos));
  if (scheme == "stremio")
    scheme = "https";

  const size_t authorityStart = schemePos + 3;
  if (authorityStart >= normalized.size())
    return "";

  const size_t authorityEnd = normalized.find_first_of("/?#", authorityStart);
  std::string authority = normalized.substr(authorityStart, authorityEnd == std::string::npos
                                                                ? std::string::npos
                                                                : authorityEnd - authorityStart);
  if (authority.empty())
    return "";

  return scheme + "://" + authority;
}

void CXBMCApp::QueuePairingRedemption(std::string responseJson,
                                      const std::string& origin,
                                      const std::string& profileName)
{
  std::unique_lock lock(m_pairingMutex);
  ClearSensitiveString(m_pairingRedemptionJson);
  m_pairingRedemptionJson = std::move(responseJson);
  m_pairingRedemptionOrigin = origin;
  m_pairingApplyProfileName = profileName;
  m_pairingRedemptionPending = true;
}

void CXBMCApp::StopBridgePairingWorker(bool clearPendingState, bool waitForCompletion)
{
  std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> coordinator;
  {
    std::unique_lock lock(m_pairingMutex);
    coordinator = m_pairingCoordinator;
  }
  if (coordinator)
    coordinator->Stop(waitForCompletion);

  if (auto* gui = CServiceBroker::GetGUI())
  {
    if (auto* dialog = gui->GetWindowManager().GetWindow<CGUIDialogJumpgatePairing>(
            WINDOW_DIALOG_JUMPGATE_PAIRING);
        dialog && dialog->IsActive())
      dialog->Close(true, 0, false, waitForCompletion);
  }

  if (clearPendingState)
  {
    std::unique_lock lock(m_pairingMutex);
    m_pairingRedemptionPending = false;
    ClearSensitiveString(m_pairingRedemptionJson);
    m_pairingRedemptionOrigin.clear();
    m_pairingApplyProfileName.clear();
    if (waitForCompletion && m_pairingCoordinator == coordinator)
      m_pairingCoordinator.reset();
  }
}

void CXBMCApp::StartBridgePairing()
{
  bool canMutateProfile = false;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    canMutateProfile = authorityTransaction.CanMutateProfile();
  }
  if (!canMutateProfile)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate",
                                          "Stop playback before pairing a profile", 4000, true);
    return;
  }

  {
    std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> coordinator;
    {
      std::unique_lock lock(m_pairingMutex);
      coordinator = m_pairingCoordinator;
    }
    if (coordinator &&
        (coordinator->GetSnapshot().stage == KODI::JUMPGATE::JumpgatePairingStage::Issuing ||
         coordinator->GetSnapshot().stage ==
             KODI::JUMPGATE::JumpgatePairingStage::AwaitingActivation ||
         coordinator->GetSnapshot().stage == KODI::JUMPGATE::JumpgatePairingStage::Applying))
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                            "Pairing already in progress", 3000, true);
      return;
    }
  }

  StopBridgePairingWorker(true);

  if (!InitializeJumpgateProfileRuntime())
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          "Secure profile runtime is unavailable", 5000, true);
    return;
  }

  // Capture one immutable origin. Code issuance, polling, response validation,
  // and credential commit all remain bound to this exact origin.
  const std::string origin = m_jumpgateProfileRuntime->GetPairingOrigin();
  if (!KODI::JUMPGATE::IsValidPairingOrigin(origin, true))
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          "Pairing Bridge origin is invalid", 5000, true);
    return;
  }

  auto transport = std::make_shared<CJumpgateCurlPairingTransport>();
  auto coordinator = std::make_shared<KODI::JUMPGATE::CJumpgatePairingCoordinator>(transport);
  {
    std::unique_lock lock(m_pairingMutex);
    m_pairingCoordinator = coordinator;
  }

  KODI::JUMPGATE::JumpgatePairingRequest request;
  request.bridgeOrigin = origin;
  request.deviceName = "Jumpgate";
  const bool started = coordinator->Start(
      std::move(request),
      [this](std::string responseJson, const std::string& responseOrigin,
             const std::string& profileName)
      {
        // AndroidKeyStore and profile activation remain on Kodi's main thread.
        QueuePairingRedemption(std::move(responseJson), responseOrigin, profileName);
      },
      [](const std::string& verificationUrl) { return RenderJumpgatePairingQr(verificationUrl); },
      [](const std::string& path) { XFILE::CFile::Delete(path); });
  if (!started)
  {
    StopBridgePairingWorker(true);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          "Pairing could not be started", 5000, true);
    return;
  }

  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogJumpgatePairing>(
      WINDOW_DIALOG_JUMPGATE_PAIRING);
  if (!dialog)
  {
    StopBridgePairingWorker(true);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          "Pairing dialog is unavailable", 5000, true);
    return;
  }
  dialog->SetCoordinator(coordinator);
  dialog->Open();
  StopBridgePairingWorker(false);
}

void CXBMCApp::CheckForUpdate()
{
  if (!m_traktScrobbler)
    return;

  const std::string bridgeOrigin = m_traktScrobbler->GetBridgeOrigin();
  if (bridgeOrigin.empty())
    return;

  XFILE::CCurlFile curl;
  curl.SetTimeout(3);
  curl.SetTotalTimeout(5);
  std::string response;

  if (!curl.Get(bridgeOrigin + "/version", response))
    return;

  CVariant data;
  if (!CJSONVariantParser::Parse(response, data))
    return;

  std::string remoteVersion = data["version"].asString();
  if (remoteVersion.empty())
    return;

  // Semver comparison using major/minor/patch fields from Bridge
  int remoteMajor = static_cast<int>(data["major"].asInteger());
  int remoteMinor = static_cast<int>(data["minor"].asInteger());
  int remotePatch = static_cast<int>(data["patch"].asInteger());

  // Parse local version
  int localMajor = 0, localMinor = 0, localPatch = 0;
  sscanf(JUMPGATE_VERSION, "%d.%d.%d", &localMajor, &localMinor, &localPatch);

  bool isNewer =
      (remoteMajor > localMajor) || (remoteMajor == localMajor && remoteMinor > localMinor) ||
      (remoteMajor == localMajor && remoteMinor == localMinor && remotePatch > localPatch);

  if (isNewer)
  {
    CLog::Log(LOGINFO, "CXBMCApp: Update available: {} -> {}", JUMPGATE_VERSION, remoteVersion);
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                          "Update available: v" + remoteVersion, 7000, true);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: Version up to date ({})", JUMPGATE_VERSION);
  }
}

void CXBMCApp::ShowJumpgateProfilePicker(bool removeProfile)
{
  bool canMutateProfile = false;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    canMutateProfile = authorityTransaction.CanMutateProfile();
  }
  if (!canMutateProfile)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate",
                                          "Stop playback before changing the active profile", 4000,
                                          true);
    return;
  }

  if (!InitializeJumpgateProfileRuntime())
    return;

  const std::vector<KODI::JUMPGATE::ProfileMetadata> profiles =
      m_jumpgateProfileRuntime->GetProfiles();
  if (profiles.empty())
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                          "No paired profiles", 3000, true);
    return;
  }

  CGUIDialogSelect* dialog =
      CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
          WINDOW_DIALOG_SELECT);
  if (!dialog)
    return;
  dialog->Reset();
  dialog->SetHeading(
      CVariant{removeProfile ? "Forget Jumpgate Profile" : "Select Jumpgate Profile"});
  for (const auto& profile : profiles)
  {
    std::string label = profile.name.empty() ? "Jumpgate Profile" : profile.name;
    if (profile.active)
      label += " [active]";
    if (profile.state != "paired")
      label += " [" + profile.state + "]";
    dialog->Add(label);
  }
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  const int selected = dialog->GetSelectedItem();
  if (selected < 0 || static_cast<size_t>(selected) >= profiles.size())
    return;
  const auto& profile = profiles[static_cast<size_t>(selected)];

  std::string error;
  bool changed = false;
  KODI::JUMPGATE::ProfileMutationResult mutationResult;
  if (removeProfile)
  {
    if (!CGUIDialogYesNo::ShowAndGetInput(
            CVariant{"Forget Jumpgate Profile"},
            CVariant{"Remove " + profile.name + " and its encrypted local credential?"}))
      return;
  }
  const auto transitionToken = BeginJumpgateProfileAuthorityTransition(error);
  if (!transitionToken)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate", error, 5000,
                                          true);
    return;
  }
  PrepareJumpgateProfileAuthorityTransition();

  if (removeProfile)
  {
    bool historyBlocked = false;
    if (m_jumpgatePlaybackHistoryStore)
    {
      std::string historyError;
      historyBlocked =
          m_jumpgatePlaybackHistoryStore->BlockProfile(profile.profileId, historyError);
      if (!historyBlocked)
      {
        error = "Profile was not forgotten because local history could not be protected: " +
                historyError;
      }
    }
    if (error.empty())
    {
      mutationResult =
          m_jumpgateProfileRuntime->ForgetLocal(profile.profileId, profile.deviceId, error);
      changed = mutationResult.IsCommitted();
    }
    const auto historyAction =
        KODI::JUMPGATE::GetJumpgateForgetHistoryAction(historyBlocked, mutationResult);
    if (historyAction == KODI::JUMPGATE::JumpgateForgetHistoryAction::UnblockPreserving)
    {
      std::string recoveryError;
      if (!m_jumpgatePlaybackHistoryStore->UnblockProfile(profile.profileId, recoveryError))
        error += "; history remains recoverably blocked: " + recoveryError;
    }
    if (historyAction == KODI::JUMPGATE::JumpgateForgetHistoryAction::PurgeKeepingBlocked)
    {
      std::string purgeError;
      if (!m_jumpgatePlaybackHistoryStore->PurgeBlockedProfile(profile.profileId, purgeError))
        error = "Profile forgotten; protected history cleanup remains pending: " + purgeError;
    }
    if (mutationResult.status == KODI::JUMPGATE::ProfileMutationStatus::CommittedRefreshFailed)
    {
      std::string reloadError;
      const std::string refreshMessage =
          m_jumpgateProfileRuntime->Reload(reloadError)
              ? "local authority was reloaded after a transient refresh failure"
              : "local authority failed closed and reload remains pending: " + reloadError;
      if (!error.empty())
        error += "; ";
      error += "Profile forgotten; " + refreshMessage;
    }
  }
  else
  {
    mutationResult = m_jumpgateProfileRuntime->SelectActive(profile.profileId, error);
    changed = mutationResult.IsCommitted();
    if (mutationResult.status == KODI::JUMPGATE::ProfileMutationStatus::CommittedRefreshFailed)
    {
      std::string reloadError;
      const std::string refreshMessage =
          m_jumpgateProfileRuntime->Reload(reloadError)
              ? "Active profile changed and local authority was reloaded"
              : "Active profile changed; authority failed closed and reload remains pending: " +
                    reloadError;
      if (!error.empty())
        error += "; ";
      error += refreshMessage;
    }
  }

  FinishJumpgateProfileAuthorityTransition(*transitionToken, changed);
  if (!changed)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          error.empty() ? "Profile update failed" : error, 5000,
                                          true);
    return;
  }

  CGUIDialogKaiToast::QueueNotification(
      error.empty() ? CGUIDialogKaiToast::Info : CGUIDialogKaiToast::Warning, "Jumpgate",
      error.empty() ? (removeProfile ? "Profile forgotten" : "Active profile updated") : error,
      4000, true);
}

void CXBMCApp::ShowJumpgateProfileManager()
{
  CGUIDialogSelect* dialog =
      CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
          WINDOW_DIALOG_SELECT);
  if (!dialog)
    return;
  dialog->Reset();
  dialog->SetHeading(CVariant{"Jumpgate Profiles"});
  dialog->Add("Pair New Profile");
  dialog->Add("Switch Active Profile");
  dialog->Add("Forget Profile");
  dialog->Add("Use Unpaired Local Mode");
  dialog->Add("Show Saved Profiles");
  dialog->Open();
  if (!dialog->IsConfirmed())
    return;

  switch (dialog->GetSelectedItem())
  {
    case 0:
      StartBridgePairing();
      break;
    case 1:
      ShowJumpgateProfilePicker(false);
      break;
    case 2:
      ShowJumpgateProfilePicker(true);
      break;
    case 3:
      HandleJumpgateManagerCommand("clear");
      break;
    case 4:
      HandleJumpgateManagerCommand("show");
      break;
    default:
      break;
  }
}

void CXBMCApp::HandleJumpgateManagerCommand(const std::string& command)
{
  if (command == "pair")
  {
    StartBridgePairing();
    return;
  }
  if (command == "switch")
  {
    ShowJumpgateProfilePicker(false);
    return;
  }
  if (command == "remove")
  {
    ShowJumpgateProfilePicker(true);
    return;
  }
  if (command == "menu")
  {
    ShowJumpgateProfileManager();
    return;
  }
  if (!InitializeJumpgateProfileRuntime())
    return;

  if (command == "clear")
  {
    auto active = m_jumpgateProfileRuntime->GetActive();
    if (!active.selected)
    {
      active.ClearSecrets();
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                            "Unpaired local mode is already active", 3000, true);
      return;
    }
    const std::string profileName = active.name.empty() ? "the active profile" : active.name;
    active.ClearSecrets();
    if (!CGUIDialogYesNo::ShowAndGetInput(
            CVariant{"Use Unpaired Local Mode"},
            CVariant{"Stop using " + profileName + " for this Kodi profile?"}))
      return;
    std::string error;
    const auto transitionToken = BeginJumpgateProfileAuthorityTransition(error);
    if (!transitionToken)
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate", error, 5000,
                                            true);
      return;
    }
    PrepareJumpgateProfileAuthorityTransition();
    const auto clearResult = m_jumpgateProfileRuntime->ClearActive(error);
    if (!clearResult.IsCommitted())
    {
      FinishJumpgateProfileAuthorityTransition(*transitionToken, false);
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate", error, 5000,
                                            true);
      return;
    }
    if (!clearResult.IsFullyApplied())
    {
      std::string reloadError;
      if (m_jumpgateProfileRuntime->Reload(reloadError))
        error = "Unpaired mode committed and local authority was reloaded";
      else
        error = "Unpaired mode committed; authority failed closed and reload remains pending: " +
                reloadError;
    }
    FinishJumpgateProfileAuthorityTransition(*transitionToken, true);
    CGUIDialogKaiToast::QueueNotification(
        error.empty() ? CGUIDialogKaiToast::Info : CGUIDialogKaiToast::Warning, "Jumpgate",
        error.empty() ? "Using unpaired local mode" : error, 4000, true);
    return;
  }

  if (command == "show")
  {
    const auto profiles = m_jumpgateProfileRuntime->GetProfiles();
    CGUIDialogSelect* dialog =
        CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
            WINDOW_DIALOG_SELECT);
    if (!dialog)
      return;
    dialog->Reset();
    dialog->SetHeading(CVariant{"Saved Jumpgate Profiles"});
    if (profiles.empty())
      dialog->Add("No paired profiles");
    for (const auto& profile : profiles)
    {
      std::string label = profile.name.empty() ? "Jumpgate Profile" : profile.name;
      if (profile.active)
        label += " [active]";
      dialog->Add(label);
    }
    dialog->Open();
    return;
  }

  if (command == "help")
  {
    CGUIDialogOK::ShowAndGetInput(
        CVariant{"Jumpgate Pairing"},
        CVariant{"Pair once from Kodi, then open the shown URL on your phone or laptop."},
        CVariant{"Finish Trakt and addon setup in the browser. The approved profile is applied "
                 "automatically."},
        CVariant{"Each saved profile keeps a separate encrypted device credential."});
    return;
  }

  CLog::Log(LOGWARNING, "CXBMCApp: Ignored unknown Jumpgate manager command");
}

void CXBMCApp::ShowSettingsDialog()
{
  CGUIDialogSelect* dialog =
      CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(
          WINDOW_DIALOG_SELECT);

  if (!dialog)
    return;

  dialog->Reset();
  dialog->SetHeading(CVariant{"Jumpgate Settings"});

  KODI::JUMPGATE::ActiveProfile active;
  if (m_jumpgateProfileRuntime)
    active = m_jumpgateProfileRuntime->GetActive();
  dialog->Add("Profiles (" + (active.selected ? active.name : "unpaired local mode") + ")");
  dialog->Add("Subtitle Languages (" + GetSettingString("subtitle_languages", "en") + ")");
  dialog->Add(active.selected ? "Trakt (managed by active profile)"
                              : "Trakt (pair a Jumpgate profile)");
  dialog->Add("OpenSubtitles Account");
  dialog->Add("About Jumpgate");
  active.ClearSecrets();

  dialog->Open();

  if (!dialog->IsConfirmed())
    return;

  int sel = dialog->GetSelectedItem();

  switch (sel)
  {
    case 0: // Profiles
    {
      ShowJumpgateProfileManager();
      break;
    }
    case 1: // Subtitle Languages
    {
      std::string langs = GetSettingString("subtitle_languages", "en");
      if (CGUIKeyboardFactory::ShowAndGetInput(
              langs, CVariant{"Subtitle languages (comma-separated: en,es,fr)"}, true))
      {
        if (langs.empty())
          langs = "en";
        SetSetting("subtitle_languages", langs);
      }
      break;
    }
    case 2: // Trakt Account
    {
      KODI::JUMPGATE::ActiveProfile current;
      if (m_jumpgateProfileRuntime)
        current = m_jumpgateProfileRuntime->GetActive();
      if (current.selected)
      {
        current.ClearSecrets();
        CGUIDialogKaiToast::QueueNotification(
            CGUIDialogKaiToast::Info, "Jumpgate",
            "Trakt is managed from the paired Jumpgate configuration page", 5000, true);
        break;
      }
      current.ClearSecrets();
      if (CGUIDialogYesNo::ShowAndGetInput(
              CVariant{"Trakt via Jumpgate"},
              CVariant{"Pair a Jumpgate profile to connect a separate Trakt account securely?"}))
        StartBridgePairing();
      break;
    }
    case 3: // OpenSubtitles Account
    {
      if (m_subtitleDownloader)
      {
        // Reinitialize will re-prompt for credentials
        m_subtitleDownloader->Deinitialize();
        m_subtitleDownloader->Initialize();
        CGUIDialogKaiToast::QueueNotification(
            CGUIDialogKaiToast::Info, "Jumpgate",
            "OpenSubtitles will prompt for credentials on next play", 3000, true);
      }
      break;
    }
    case 4: // About
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                            std::string("Jumpgate v") + JUMPGATE_VERSION +
                                                " - External Player for Stremio",
                                            5000, true);
      break;
    }
  }
}

const CJNIViewInputDevice CXBMCApp::GetInputDevice(int deviceId)
{
  CJNIInputManager inputManager(getSystemService(CJNIContext::INPUT_SERVICE));
  return inputManager.getInputDevice(deviceId);
}

std::vector<int> CXBMCApp::GetInputDeviceIds()
{
  CJNIInputManager inputManager(getSystemService(CJNIContext::INPUT_SERVICE));
  return inputManager.getInputDeviceIds();
}

void CXBMCApp::ProcessSlow()
{
  if ((m_playback_state & PLAYBACK_STATE_PLAYING) && !m_mediaSessionUpdated &&
      m_mediaSession->isActive())
    UpdateSessionState();

  {
    std::string pairingRedemptionJson;
    std::string pairingRedemptionOrigin;
    std::string pairingProfileName;
    std::string pairingErrorMessage;
    std::string pairingWarningMessage;
    std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> pairingCoordinator;
    bool pairingProfilePreviouslyKnown = false;
    bool pairingRedemptionPending = false;
    {
      std::unique_lock lock(m_pairingMutex);
      pairingRedemptionPending = m_pairingRedemptionPending;
      pairingCoordinator = m_pairingCoordinator;
    }

    std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Token> transitionToken;
    if (pairingRedemptionPending)
    {
      std::string transitionError;
      transitionToken = BeginJumpgateProfileAuthorityTransition(transitionError);
      if (transitionToken)
      {
        std::unique_lock lock(m_pairingMutex);
        if (m_pairingRedemptionPending)
        {
          pairingRedemptionJson.swap(m_pairingRedemptionJson);
          pairingRedemptionOrigin.swap(m_pairingRedemptionOrigin);
          pairingProfileName.swap(m_pairingApplyProfileName);
          m_pairingRedemptionPending = false;
          m_pairingApplyProfileName.clear();
        }
      }
      else
      {
        bool rejectedPendingRedemption = false;
        {
          std::unique_lock lock(m_pairingMutex);
          if (m_pairingRedemptionPending)
          {
            pairingRedemptionJson.swap(m_pairingRedemptionJson);
            m_pairingRedemptionOrigin.clear();
            m_pairingApplyProfileName.clear();
            m_pairingRedemptionPending = false;
            rejectedPendingRedemption = true;
          }
        }
        ClearSensitiveString(pairingRedemptionJson);
        if (rejectedPendingRedemption)
        {
          pairingErrorMessage = transitionError.empty()
                                    ? "Pairing could not acquire local profile authority"
                                    : transitionError;
          if (pairingCoordinator)
            pairingCoordinator->CompleteApply(false, pairingErrorMessage);
        }
      }
    }

    if (transitionToken && pairingRedemptionJson.empty())
      FinishJumpgateProfileAuthorityTransition(*transitionToken, false);

    if (transitionToken && !pairingRedemptionJson.empty())
    {
      PrepareJumpgateProfileAuthorityTransition();
      CVariant redemption;
      if (!CJSONVariantParser::Parse(pairingRedemptionJson, redemption) || !redemption.isObject())
      {
        pairingErrorMessage = "Pairing failed: invalid redemption response";
      }
      ClearSensitiveString(pairingRedemptionJson);

      std::string storeError;
      std::string pairingProfileId;
      auto historyAction = KODI::JUMPGATE::JumpgatePairingHistoryAction::None;
      KODI::JUMPGATE::ProfileMutationResult pairingResult;
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      if (pairingErrorMessage.empty() && !InitializeJumpgateProfileRuntime())
        pairingErrorMessage = "Pairing profile runtime initialization failed";
      if (pairingErrorMessage.empty() && redemption.isMember("profileId") &&
          redemption["profileId"].isString())
      {
        pairingProfileId = redemption["profileId"].asString();
        const auto profiles = m_jumpgateProfileRuntime->GetProfiles();
        pairingProfilePreviouslyKnown =
            std::any_of(profiles.begin(), profiles.end(), [&pairingProfileId](const auto& profile)
                        { return profile.profileId == pairingProfileId; });
      }
      if (pairingErrorMessage.empty() && m_jumpgatePlaybackHistoryStore &&
          !pairingProfileId.empty())
      {
        bool historyBlocked = false;
        bool profileForgotten = false;
        std::string historyError;
        if (!m_jumpgatePlaybackHistoryStore->GetProfileProtection(pairingProfileId, historyBlocked,
                                                                  profileForgotten, historyError))
        {
          pairingErrorMessage = "Pairing not applied because local history protection could not "
                                "be inspected: " +
                                historyError;
        }
        else
        {
          historyAction = KODI::JUMPGATE::GetJumpgatePairingHistoryAction(
              historyBlocked, profileForgotten, pairingProfilePreviouslyKnown);
          if (historyAction == KODI::JUMPGATE::JumpgatePairingHistoryAction::PurgeThenUnblock &&
              !m_jumpgatePlaybackHistoryStore->PurgeBlockedProfile(pairingProfileId, historyError))
          {
            pairingErrorMessage =
                "Pairing not applied; forgotten history purge remains pending: " + historyError;
          }
        }
      }
      if (pairingErrorMessage.empty())
      {
        pairingResult = m_jumpgateProfileRuntime->StorePairingResponse(
            redemption, pairingRedemptionOrigin, true, now, storeError);
        if (!pairingResult.IsCommitted())
        {
          pairingErrorMessage =
              storeError.empty() ? "Pairing credential commit failed" : storeError;
        }
        else if (!pairingResult.IsFullyApplied())
        {
          std::string reloadError;
          if (m_jumpgateProfileRuntime->Reload(reloadError))
          {
            pairingWarningMessage =
                "Pairing committed and local authority recovered after a transient refresh failure";
          }
          else
          {
            pairingWarningMessage =
                "Pairing committed; authority failed closed and reload remains pending: " +
                reloadError;
          }
        }
      }
      redemption = CVariant{};

      const bool pairingCommitted = pairingResult.IsCommitted();
      if (pairingCommitted)
      {
        if (m_jumpgatePlaybackHistoryStore && !pairingProfileId.empty() &&
            historyAction != KODI::JUMPGATE::JumpgatePairingHistoryAction::None)
        {
          std::string historyError;
          if (!m_jumpgatePlaybackHistoryStore->CompleteProfileRepair(pairingProfileId,
                                                                     historyError))
          {
            if (!pairingWarningMessage.empty())
              pairingWarningMessage += "; ";
            pairingWarningMessage +=
                "paired history remains protected until repair completes: " + historyError;
          }
        }
        KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
        if (pairingProfileName.empty())
        {
          pairingProfileName = active.name.empty() ? (pairingProfileId.empty() ? "Jumpgate Profile"
                                                                               : pairingProfileId)
                                                   : active.name;
        }
        active.ClearSecrets();
      }

      FinishJumpgateProfileAuthorityTransition(*transitionToken, pairingCommitted);
      if (pairingCoordinator)
        pairingCoordinator->CompleteApply(pairingCommitted, pairingErrorMessage);
      if (pairingCommitted)
      {
        const std::string toast = "Paired and applied (" + pairingProfileName + ")";
        CGUIDialogKaiToast::QueueNotification(
            pairingWarningMessage.empty() ? CGUIDialogKaiToast::Info : CGUIDialogKaiToast::Warning,
            "Jumpgate", pairingWarningMessage.empty() ? toast : pairingWarningMessage, 5000, true);
      }
    }
    if (!pairingErrorMessage.empty())
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                            pairingErrorMessage, 5000, true);
    }
  }

  // Track playback position for external player mode result
  // Track during both PLAYING and PAUSED states (video/audio flag stays set when paused)
  if (m_externalPlayerMode.load(std::memory_order_relaxed) &&
      (m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO)))
  {
    const auto& components = CServiceBroker::GetAppComponents();
    const auto appPlayer = components.GetComponent<CApplicationPlayer>();
    int64_t currentTime = appPlayer->GetTime();
    int64_t totalTime = appPlayer->GetTotalTime();
    // Only update if we got valid values (player may return 0 briefly during transitions)
    if (currentTime > 0 || totalTime > 0)
    {
      m_lastPlaybackTimeMs.store(currentTime, std::memory_order_relaxed);
      m_lastPlaybackDurationMs.store(totalTime, std::memory_order_relaxed);
      uint64_t generation = 0;
      {
        std::unique_lock lock(m_playbackClaimMutex);
        generation = m_playbackClaimGeneration;
      }
      const int64_t observedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
      m_playbackHistoryState.UpdateProgress(generation, std::max<int64_t>(0, currentTime),
                                            std::max<int64_t>(0, totalTime), observedAtMs);
      m_playbackResultState.Capture(generation, std::max<int64_t>(0, currentTime),
                                    std::max<int64_t>(0, totalTime));
    }

    // Feed Java-side Jumpgate overlay with real parser/clock signals so the
    // loading progress/status can reflect actual stream readiness.
    if (!m_overlayHidden)
      call_method<void>(m_context, "updatePlaybackPosition", "(JJ)V", currentTime, totalTime);

    // Hide loading overlay once the player clock is actually advancing.
    // Separate from position tracking: totalTime > 0 fires too early (file header parsed,
    // no frames decoded yet). currentTime > 0 means actual decoding/playback is happening
    // and video frames are being rendered on the SurfaceView.
    if (!m_overlayHidden && currentTime > 0)
    {
      call_method<void>(m_context, "hideLoadingOverlay", "()V");
      m_overlayHidden = true;
      CLog::Log(LOGINFO, "CXBMCApp: Loading overlay hidden (currentTime={}ms)", currentTime);
    }
  }

  // Apply source claims on the Kodi main thread before Trakt evaluates playback state.
  if (m_externalPlayerMode.load(std::memory_order_relaxed))
  {
    ProcessPlaybackSourceClaim();
    ProcessJumpgateSubtitles();
  }

  // Process deferred claim metadata and renew the paired Bridge token when needed.
  if (m_traktScrobbler && m_externalPlayerMode.load(std::memory_order_relaxed))
    m_traktScrobbler->ProcessSlow();

  // Update Jumpgate overlay content info as soon as identification/hydration becomes available.
  // Safe even if the overlay isn't currently shown (Java side will no-op).
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && !m_overlayHidden &&
      m_traktScrobbler && m_traktScrobbler->IsContentIdentified())
  {
    UpdateLoadingOverlayContentInfo(false);
  }

  // Content-ID based late resume: when content is identified after playback starts
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler &&
      !m_resumeApplied.load(std::memory_order_relaxed))
    OnContentIdentified();

  // Settings dialog (triggered by Menu key from input thread)
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_settingsRequested.exchange(false))
    ShowSettingsDialog();

  // One-time update check
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && !m_updateChecked &&
      GetSettingBool("auto_update_check", true))
  {
    m_updateChecked = true;
    CheckForUpdate();
  }
}

std::vector<androidPackage> CXBMCApp::GetApplications() const
{
  std::unique_lock lock(m_applicationsMutex);
  if (m_applications.empty())
  {
    CJNIList<CJNIApplicationInfo> packageList =
        GetPackageManager().getInstalledApplications(CJNIPackageManager::GET_ACTIVITIES);
    int numPackages = packageList.size();
    for (int i = 0; i < numPackages; i++)
    {
      CJNIIntent intent =
          GetPackageManager().getLaunchIntentForPackage(packageList.get(i).packageName);
      if (!intent)
        intent =
            GetPackageManager().getLeanbackLaunchIntentForPackage(packageList.get(i).packageName);
      if (!intent)
        continue;

      androidPackage newPackage;
      newPackage.packageName = packageList.get(i).packageName;
      newPackage.packageLabel =
          GetPackageManager().getApplicationLabel(packageList.get(i)).toString();
      newPackage.icon = packageList.get(i).icon;
      m_applications.emplace_back(newPackage);
    }
  }

  return m_applications;
}

// Note intent, dataType, dataURI, action, category, flags, extras, className all default to ""
bool CXBMCApp::StartActivity(const std::string& package,
                             const std::string& intent,
                             const std::string& dataType,
                             const std::string& dataURI,
                             const std::string& flags,
                             const std::string& extras,
                             const std::string& action,
                             const std::string& category,
                             const std::string& className)
{
  CLog::LogF(LOGDEBUG, "Starting Android activity");

  CJNIIntent newIntent =
      intent.empty() ? GetPackageManager().getLaunchIntentForPackage(package) : CJNIIntent(intent);

  if (intent.empty() && GetPackageManager().hasSystemFeature(CJNIPackageManager::FEATURE_LEANBACK))
  {
    CJNIIntent leanbackIntent = GetPackageManager().getLeanbackLaunchIntentForPackage(package);
    if (leanbackIntent)
      newIntent = leanbackIntent;
  }

  if (!newIntent)
    return false;

  if (!dataURI.empty())
  {
    CJNIURI jniURI = CJNIURI::parse(dataURI);

    if (!jniURI)
      return false;

    // decoded path or null if this is not a hierarchical URI
    const std::string pathname = jniURI.getPath();

    if (!pathname.empty() && StringUtils::StartsWith(pathname, "/storage/"))
    {
      // generate a content URI
      jniURI = CJNIFileProvider::getUriForFile(
          CXBMCApp::Get(), std::string(CCompileInfo::GetPackage()) + ".fileprovider",
          CJNIFile(pathname));

      CLog::LogF(LOGINFO, "Sharing content through FileProvider");

      // grant temporary permission to external app
      CJNIContext::grantUriPermission(package, jniURI, CJNIIntent::FLAG_GRANT_READ_URI_PERMISSION);
    }

    newIntent.setDataAndType(jniURI, dataType);
  }

  if (!action.empty())
    newIntent.setAction(action);

  if (!category.empty())
    newIntent.addCategory(category);

  if (!flags.empty())
  {
    try
    {
      newIntent.setFlags(std::stoi(flags));
    }
    catch (const std::exception&)
    {
      CLog::LogF(LOGDEBUG, "Invalid flags given, ignore them");
    }
  }

  if (!extras.empty())
  {
    CVariant doc;
    CJSONVariantParser::Parse(extras, doc);
    if (!doc.isArray())
    {
      CLog::LogF(LOGDEBUG, "Invalid intent extras format: Needs to be an array");
      return false;
    }

    for (auto it = doc.begin_array(); it != doc.end_array(); ++it)
    {
      CVariant& e = *it;
      if (!e.isObject() || !e.isMember("type") || !e.isMember("key") || !e.isMember("value"))
      {
        CLog::LogF(LOGDEBUG, "Invalid intent extras value format");
        continue;
      }

      if (e["type"] == "string")
      {
        newIntent.putExtra(e["key"].asString(), e["value"].asString());
        CLog::LogF(LOGDEBUG, "Adding Android intent extra");
      }
      else
        CLog::LogF(LOGDEBUG, "Ignoring unsupported Android intent extra type");
    }
  }

  newIntent.setPackage(package);
  if (!className.empty())
    newIntent.setClassName(package, className);

  startActivity(newIntent);
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::LogF(LOGERROR, "Exception occurred launching Android activity");
    xbmc_jnienv()->ExceptionClear();
    return false;
  }

  return true;
}

int CXBMCApp::GetBatteryLevel() const
{
  return m_batteryLevel;
}

// Used in Application.cpp to figure out volume steps
int CXBMCApp::GetMaxSystemVolume()
{
  JNIEnv* env = xbmc_jnienv();
  static int maxVolume = -1;
  if (maxVolume == -1)
  {
    maxVolume = GetMaxSystemVolume(env);
  }
  //android_printf("CXBMCApp::GetMaxSystemVolume: %i",maxVolume);
  return maxVolume;
}

int CXBMCApp::GetMaxSystemVolume(JNIEnv* env)
{
  CJNIAudioManager audioManager(getSystemService(CJNIContext::AUDIO_SERVICE));
  if (audioManager)
    return audioManager.getStreamMaxVolume();
  android_printf("CXBMCApp::SetSystemVolume: Could not get Audio Manager");
  return 0;
}

float CXBMCApp::GetSystemVolume()
{
  CJNIAudioManager audioManager(getSystemService(CJNIContext::AUDIO_SERVICE));
  if (audioManager)
    return (float)audioManager.getStreamVolume() / GetMaxSystemVolume();
  else
  {
    android_printf("CXBMCApp::GetSystemVolume: Could not get Audio Manager");
    return 0;
  }
}

void CXBMCApp::SetSystemVolume(float percent)
{
  CJNIAudioManager audioManager(getSystemService(CJNIContext::AUDIO_SERVICE));
  int maxVolume = (int)(GetMaxSystemVolume() * percent);
  if (audioManager)
    audioManager.setStreamVolume(maxVolume);
  else
    android_printf("CXBMCApp::SetSystemVolume: Could not get Audio Manager");
}

void CXBMCApp::onReceive(CJNIIntent intent)
{
  std::string action = intent.getAction();
  android_printf("CXBMCApp::onReceive - Got intent. Action: %s", action.c_str());

  // Most actions can be processed only after the app is fully initialized,
  // but some actions should be processed even during initialization phase.
  if (!g_application.IsInitialized() && action != CJNIAudioManager::ACTION_HDMI_AUDIO_PLUG)
  {
    android_printf("CXBMCApp::onReceive - ignoring action %s during app initialization phase",
                   action.c_str());
    return;
  }

  if (action == CJNIIntent::ACTION_BATTERY_CHANGED)
    m_batteryLevel = intent.getIntExtra("level", -1);
  else if (action == CJNIIntent::ACTION_DREAMING_STOPPED)
  {
    if (HasFocus())
    {
      auto& components = CServiceBroker::GetAppComponents();
      const auto appPower = components.GetComponent<CApplicationPowerHandling>();
      appPower->WakeUpScreenSaverAndDPMS();
    }
  }
  else if (action == CJNIIntent::ACTION_HEADSET_PLUG ||
           action == "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED")
  {
    bool newstate = m_headsetPlugged;
    if (action == CJNIIntent::ACTION_HEADSET_PLUG)
    {
      newstate = (intent.getIntExtra("state", 0) != 0);

      // If unplugged headset and playing content then pause or stop playback
      if (!newstate && (m_playback_state & PLAYBACK_STATE_PLAYING))
      {
        const auto& components = CServiceBroker::GetAppComponents();
        const auto appPlayer = components.GetComponent<CApplicationPlayer>();
        if (appPlayer->CanPause())
        {
          CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                     static_cast<void*>(new CAction(ACTION_PAUSE)));
        }
        else
        {
          CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                     static_cast<void*>(new CAction(ACTION_STOP)));
        }
      }
    }
    else if (action == "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED")
      newstate = (intent.getIntExtra("android.bluetooth.profile.extra.STATE", 0) ==
                  2 /* STATE_CONNECTED */);

    if (newstate != m_headsetPlugged)
    {
      m_headsetPlugged = newstate;
      IAE* iae = CServiceBroker::GetActiveAE();
      if (iae)
        iae->DeviceChange();
    }
  }
  else if (action == CJNIAudioManager::ACTION_HDMI_AUDIO_PLUG)
  {
    m_hdmiPlugged = (intent.getIntExtra(CJNIAudioManager::EXTRA_AUDIO_PLUG_STATE, 0) != 0);
    android_printf("-- HDMI is plugged in: %s", m_hdmiPlugged ? "yes" : "no");
    if (g_application.IsInitialized())
    {
      CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
      if (winSystem && dynamic_cast<CWinSystemAndroid*>(winSystem))
        dynamic_cast<CWinSystemAndroid*>(winSystem)->SetHdmiState(m_hdmiPlugged);
    }
    if (m_hdmiPlugged && m_aeReset)
    {
      android_printf("CXBMCApp::onReceive: Reset audio engine");
      CServiceBroker::GetActiveAE()->DeviceChange();
      m_aeReset = false;
    }
    if (m_hdmiPlugged && m_wakeUp)
    {
      OnWakeup();
      m_wakeUp = false;
    }
  }
  else if (action == CJNIIntent::ACTION_SCREEN_ON)
  {
    // Sent when the device wakes up and becomes interactive.
    //
    // For historical reasons, the name of this broadcast action refers to the power state of the
    // screen but it is actually sent in response to changes in the overall interactive state of
    // the device.
    CLog::Log(LOGINFO, "Got device wakeup intent");
    if (m_hdmiPlugged)
      OnWakeup();
    else
      // wake-up sequence continues in ACTION_HDMI_AUDIO_PLUG intent
      m_wakeUp = true;
  }
  else if (action == CJNIIntent::ACTION_SCREEN_OFF)
  {
    // Sent when the device goes to sleep and becomes non-interactive.
    //
    // For historical reasons, the name of this broadcast action refers to the power state of the
    // screen but it is actually sent in response to changes in the overall interactive state of
    // the device.
    CLog::Log(LOGINFO, "Got device sleep intent");
    OnSleep();
  }
  else if (action == CJNIIntent::ACTION_MEDIA_BUTTON)
  {
    if (m_playback_state == PLAYBACK_STATE_STOPPED)
    {
      CLog::Log(LOGINFO, "Ignore MEDIA_BUTTON intent: no media playing");
      return;
    }
    CJNIKeyEvent keyevt = (CJNIKeyEvent)intent.getParcelableExtra(CJNIIntent::EXTRA_KEY_EVENT);

    int keycode = keyevt.getKeyCode();
    bool up = (keyevt.getAction() == CJNIKeyEvent::ACTION_UP);

    CLog::Log(LOGINFO, "Got MEDIA_BUTTON intent: {}, up:{}", keycode, up ? "true" : "false");
    if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_RECORD)
      CAndroidKey::XBMC_Key(keycode, XBMCK_RECORD, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_EJECT)
      CAndroidKey::XBMC_Key(keycode, XBMCK_EJECT, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_FAST_FORWARD)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_FASTFORWARD, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_NEXT)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_NEXT_TRACK, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PAUSE)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PLAY)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PLAY_PAUSE)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PLAY_PAUSE, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_PREVIOUS)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_PREV_TRACK, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_REWIND)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_REWIND, 0, 0, up);
    else if (keycode == CJNIKeyEvent::KEYCODE_MEDIA_STOP)
      CAndroidKey::XBMC_Key(keycode, XBMCK_MEDIA_STOP, 0, 0, up);
  }
}

void CXBMCApp::OnSleep()
{
  CLog::Log(LOGDEBUG, "CXBMCApp::OnSleep");
  IPowerSyscall* syscall = CServiceBroker::GetPowerManager().GetPowerSyscall();
  if (syscall)
    static_cast<CAndroidPowerSyscall*>(syscall)->SetSuspended();
}

void CXBMCApp::OnWakeup()
{
  CLog::Log(LOGDEBUG, "CXBMCApp::OnWakeup");
  IPowerSyscall* syscall = CServiceBroker::GetPowerManager().GetPowerSyscall();
  if (syscall)
    static_cast<CAndroidPowerSyscall*>(syscall)->SetResumed();

  if (HasFocus())
  {
    auto& components = CServiceBroker::GetAppComponents();
    const auto appPower = components.GetComponent<CApplicationPowerHandling>();
    appPower->WakeUpScreenSaverAndDPMS();
  }
}

// Legacy, unpaired destroy safety net. Paired playback never reaches this
// helper, and the helper deliberately has no remote transport capability.
static void SaveLegacyResumeForContentLocal(
    const std::string& imdbId, int season, int episode, int64_t posMs, int64_t durMs)
{
  if (imdbId.empty() || posMs <= 0)
    return;

  std::string key = imdbId;
  if (season >= 0 && episode >= 0)
    key += ":" + std::to_string(season) + ":" + std::to_string(episode);

  bool completed =
      (durMs > 0 && posMs > 0 &&
       static_cast<float>(posMs) / static_cast<float>(durMs) > Jumpgate::RESUME_CLEAR_RATIO);

  // Load existing resume store
  std::string storePath = CSpecialProtocol::TranslatePath("special://profile/jumpgate_resume.json");
  CVariant store(CVariant::VariantTypeObject);

  XFILE::CFile file;
  std::vector<uint8_t> buffer;
  if (file.LoadFile(storePath, buffer) > 0)
  {
    std::string json(buffer.begin(), buffer.end());
    CJSONVariantParser::Parse(json, store);
  }

  if (completed)
  {
    if (store.isMember(key))
      store.erase(key);
    CLog::Log(LOGINFO, "SaveLegacyResumeForContentLocal: Cleared {} (completed)", key);
  }
  else
  {
    CVariant entry(CVariant::VariantTypeObject);
    entry["position"] = posMs;
    entry["duration"] = durMs;
    entry["timestamp"] =
        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
    store[key] = entry;
    CLog::Log(LOGINFO, "SaveLegacyResumeForContentLocal: Saved {} - pos={} dur={}", key, posMs,
              durMs);
  }

  // Write back
  std::string json;
  if (CJSONVariantWriter::Write(store, json, true))
  {
    XFILE::CFile outFile;
    if (outFile.OpenForWrite(storePath, true))
    {
      outFile.Write(json.c_str(), json.size());
      outFile.Close();
    }
  }
}

void CXBMCApp::onNewIntent(CJNIIntent intent, std::string preparedRequestId)
{
  if (!intent)
  {
    CLog::Log(LOGINFO, "CXBMCApp::onNewIntent - Got invalid intent.");
    return;
  }

  std::string action = intent.getAction();
  CLog::Log(LOGDEBUG, "CXBMCApp::onNewIntent - Got intent. Action: {}", action);
  const CJNIURI rawData = intent.getData();
  const std::string rawLaunchUri = rawData ? rawData.toString() : "";
  const std::string intentResultRequestId =
      intent.hasExtra("jumpgate_external_result_request_id")
          ? intent.getStringExtra("jumpgate_external_result_request_id")
          : "";
  const std::string resultRequestId = std::move(preparedRequestId);
  std::optional<KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation>
      lifecycleOperation;
  if (action == CJNIIntent::ACTION_VIEW)
  {
    if (resultRequestId.empty() || resultRequestId != intentResultRequestId)
    {
      const std::string rejectedRequestId =
          !resultRequestId.empty() ? resultRequestId : intentResultRequestId;
      call_method<void>(m_context, "postDeferredExternalPlayerRejection", "(JLjava/lang/String;)V",
                        static_cast<jlong>(m_jumpgateBackLifecycleToken),
                        jcast<jhstring>(rejectedRequestId));
      return;
    }

    // Admission and result delivery own this same operation. Android callbacks
    // never wait for a terminal worker that still owns the previous generation.
    lifecycleOperation = m_playbackResultState.TryBeginLifecycleOperation();
    if (!lifecycleOperation)
    {
      call_method<void>(m_context, "postNativeExternalIntentRetry",
                        "(JLandroid/content/Intent;Ljava/lang/String;)V",
                        static_cast<jlong>(m_jumpgateBackLifecycleToken), intent.get_raw(),
                        jcast<jhstring>(resultRequestId));
      return;
    }
  }
  int64_t launchedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  if (intent.hasExtra("jumpgate_launched_at_ms"))
  {
    const std::string stampedAt = intent.getStringExtra("jumpgate_launched_at_ms");
    int64_t parsedAt = 0;
    const auto [end, error] =
        std::from_chars(stampedAt.data(), stampedAt.data() + stampedAt.size(), parsedAt);
    if (error == std::errc{} && end == stampedAt.data() + stampedAt.size() && parsedAt > 0 &&
        parsedAt <= launchedAtMs)
    {
      launchedAtMs = parsedAt;
    }
  }
  std::string targetFile = GetFilenameFromIntent(intent);
  if (action == CJNIIntent::ACTION_VIEW && targetFile.empty())
  {
    CLog::Log(LOGWARNING, "CXBMCApp: External playback rejected because the target is empty");
    DeliverRejectedExternalPlaybackResult(resultRequestId, *lifecycleOperation);
    return;
  }
  uint64_t admissionGeneration = 0;
  uint64_t admissionToken = 0;
  bool externalPlaybackPrepared = false;
  bool playbackAccepted = false;

  // Detect external player mode: ACTION_VIEW with a media file
  // (ACTION_GET_CONTENT is Kodi's internal leanback navigation, not external player)
  if (!targetFile.empty() && action == CJNIIntent::ACTION_VIEW)
  {
    const auto previousResultOwner = m_playbackResultState.CurrentOwner(*lifecycleOperation);
    const auto admission = BeginExternalPlaybackAdmission(rawLaunchUri, launchedAtMs,
                                                          resultRequestId, *lifecycleOperation);
    if (!admission)
    {
      CLog::Log(LOGWARNING,
                "CXBMCApp: External playback rejected during a profile authority transition");
      DeliverRejectedExternalPlaybackResult(resultRequestId, *lifecycleOperation);
      return;
    }
    admissionGeneration = admission->generation;
    admissionToken = admission->token;
    externalPlaybackPrepared = true;

    if (!m_jumpgateSubtitleController)
      m_jumpgateSubtitleController =
          std::make_unique<KODI::JUMPGATE::CAndroidJumpgateSubtitleController>();
    m_jumpgateSubtitleController->SweepStartupOrphans();
    m_jumpgateSubtitleController->PrepareGeneration(admissionGeneration);

    if (previousResultOwner && previousResultOwner->generation != admissionGeneration)
    {
      call_method<void>(m_context, "supersedeExternalPlayerResult", "(JLjava/lang/String;)V",
                        static_cast<jlong>(previousResultOwner->generation),
                        jcast<jhstring>(previousResultOwner->requestId));
    }

    // Only mark wasStandalone if we were NOT already in external player mode.
    // On cold launch from Stremio, m_externalPlayerMode is already true (set in onStart
    // via Java's isExternalPlayerMode()). On a genuine warm transition from standalone
    // Kodi, m_externalPlayerMode is false (reset by ReturnToStandaloneMode).
    bool wasColdLaunch = m_externalPlayerMode.load(std::memory_order_relaxed);
    if (!wasColdLaunch)
    {
      m_wasStandalone.store(true, std::memory_order_relaxed);
      CLog::Log(LOGINFO, "CXBMCApp: External player mode activated (warm transition)");
    }
    else
    {
      CLog::Log(LOGINFO, "CXBMCApp: External player mode activated (cold launch)");
    }
    SetExternalPlayerMode(true);
    const bool cancellationRequested =
        call_method<jboolean>(
            m_context, "beginExternalPlayerMode", "(JLjava/lang/String;Z)Z",
            static_cast<jlong>(admissionGeneration), jcast<jhstring>(resultRequestId),
            static_cast<jboolean>(m_wasStandalone.load(std::memory_order_relaxed))) == JNI_TRUE;
    if (cancellationRequested)
    {
      CommitExternalPlaybackAdmissionFailure(admissionToken);
      QueueExternalPlayerResult(admissionGeneration, resultRequestId,
                                m_wasStandalone.load(std::memory_order_relaxed));
      return;
    }

    ApplyActiveJumpgateProfile();
    m_traktScrobbler->Initialize();
    const bool pairedProfileBacked = m_traktScrobbler->IsBridgeProfileBacked();
    KODI::JUMPGATE::ActiveProfile subtitleProfile;
    if (m_jumpgateProfileRuntime)
      subtitleProfile = m_jumpgateProfileRuntime->GetActive();
    const auto subtitleProvider = KODI::JUMPGATE::SelectAndroidJumpgateSubtitleProvider(
        true, subtitleProfile.selected && subtitleProfile.sourceBacked,
        subtitleProfile.credentialsValid, subtitleProfile.subtitlesEnabled, false);

    m_traktScrobbler->StopForReplacement();
    m_traktScrobbler->SetPlaybackGeneration(admissionGeneration, admissionToken);

    // Extract content ID extras forwarded from Splash
    std::string imdbId;
    std::string title;
    int year = 0;
    int season = -1;
    int episode = -1;

    if (!pairedProfileBacked)
    {
      if (intent.hasExtra("imdb_id"))
        imdbId = intent.getStringExtra("imdb_id");
      if (intent.hasExtra("title"))
        title = intent.getStringExtra("title");
      if (intent.hasExtra("year"))
        year = intent.getIntExtra("year", 0);
      if (intent.hasExtra("season"))
        season = intent.getIntExtra("season", -1);
      if (intent.hasExtra("episode"))
        episode = intent.getIntExtra("episode", -1);
    }

    m_lastPlaybackTimeMs.store(0, std::memory_order_relaxed);
    m_lastPlaybackDurationMs.store(0, std::memory_order_relaxed);

    // Pass content info to TraktScrobbler
    if (m_traktScrobbler)
    {
      m_traktScrobbler->ClearContentInfo();
      m_traktScrobbler->SetContentInfo(imdbId, title, year, season, episode);
      m_traktScrobbler->SetMediaUrl(targetFile);
    }

    // A source-backed profile owns subtitle authority from admission onward. OpenSubtitles
    // remains available only for the unpaired compatibility path.
    if (subtitleProvider == KODI::JUMPGATE::AndroidJumpgateSubtitleProvider::OpenSubtitles &&
        !m_subtitleDownloader)
    {
      m_subtitleDownloader = std::make_unique<SubtitleDownloader>();
      m_subtitleDownloader->Initialize();
      m_subtitleDownloader->SetLanguages(subtitleProfile.subtitleLanguages);
    }
    if (subtitleProvider == KODI::JUMPGATE::AndroidJumpgateSubtitleProvider::OpenSubtitles &&
        m_subtitleDownloader)
    {
      m_subtitleDownloader->ClearContentInfo();
      m_subtitleDownloader->SetContentInfo(imdbId, title, year, season, episode);
    }
    else if (m_subtitleDownloader)
    {
      m_subtitleDownloader->Deinitialize();
      m_subtitleDownloader.reset();
    }
    subtitleProfile.ClearSecrets();

    // Java normalizes long extras to decimal strings because CJNIIntent has no
    // getLongExtra binding. The int fallback preserves older launchers.
    // Stremio uses "startfrom" (milliseconds)
    int64_t resumePositionMs = 0;
    if (!pairedProfileBacked)
    {
      const auto readPositionExtra = [&intent](const char* name) -> int64_t
      {
        if (!intent.hasExtra(name))
          return 0;
        const auto parsed = KODI::JUMPGATE::ParseJumpgatePositiveInt64(intent.getStringExtra(name));
        return parsed ? *parsed : static_cast<int64_t>(intent.getIntExtra(name, 0));
      };
      resumePositionMs = readPositionExtra("extra_position");
      if (resumePositionMs <= 0)
        resumePositionMs = readPositionExtra("position");
    }

    // Also check Stremio's "startfrom" extra
    if (!pairedProfileBacked && resumePositionMs <= 0 && intent.hasExtra("startfrom"))
    {
      const auto parsed =
          KODI::JUMPGATE::ParseJumpgatePositiveInt64(intent.getStringExtra("startfrom"));
      const int64_t startFrom =
          parsed ? *parsed : static_cast<int64_t>(intent.getIntExtra("startfrom", 0));
      if (startFrom > 0)
      {
        // Auto-detect seconds vs milliseconds: if value < 10000 treat as seconds
        resumePositionMs = (startFrom < 10000) ? startFrom * 1000 : startFrom;
        CLog::Log(LOGINFO, "CXBMCApp: startfrom={} interpreted as {} ms", startFrom,
                  resumePositionMs);
      }
    }

    if (resumePositionMs > 0)
      CLog::Log(LOGINFO, "CXBMCApp: Resume position from intent: {} ms", resumePositionMs);

    // Unpaired compatibility may use caller metadata. Paired playback waits for
    // the authenticated claim before applying content-keyed resume state.
    if (!pairedProfileBacked && resumePositionMs <= 0 && !imdbId.empty() && m_traktScrobbler)
    {
      resumePositionMs = LoadResumePosition(imdbId, season, episode);
      if (resumePositionMs > 0)
        CLog::Log(LOGINFO, "CXBMCApp: Resume from local store: {} ms", resumePositionMs);
    }

    m_resumePositionMs.store(resumePositionMs, std::memory_order_relaxed);
    m_resumeApplied.store((resumePositionMs > 0),
                          std::memory_order_relaxed); // Mark applied if we got it from intent/store
    LoadAndApplyPairedPlaybackResume(admissionGeneration, false);
  }

  if (!targetFile.empty() &&
      (action == CJNIIntent::ACTION_VIEW || action == CJNIIntent::ACTION_GET_CONTENT))
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: Dispatching external playback target");

    CURL targeturl(targetFile);
    std::string value;
    if (action == CJNIIntent::ACTION_GET_CONTENT ||
        (targeturl.GetOption("showinfo", value) && value == "true"))
    {
      if (targeturl.IsProtocol("videodb") ||
          (targeturl.IsProtocol("special") &&
           targetFile.find("playlists/video") != std::string::npos) ||
          (targeturl.IsProtocol("special") &&
           targetFile.find("playlists/mixed") != std::string::npos))
      {
        std::vector<std::string> params;
        params.push_back(targeturl.Get());
        params.emplace_back("return");
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTIVATE_WINDOW, WINDOW_VIDEO_NAV, 0,
                                                   nullptr, "", params);
      }
      else if (targeturl.IsProtocol("musicdb") ||
               (targeturl.IsProtocol("special") &&
                targetFile.find("playlists/music") != std::string::npos))
      {
        std::vector<std::string> params;
        params.push_back(targeturl.Get());
        params.emplace_back("return");
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTIVATE_WINDOW, WINDOW_MUSIC_NAV, 0,
                                                   nullptr, "", params);
      }
    }
    else
    {
      auto item = std::make_unique<CFileItem>(targetFile, false);
      if (action == CJNIIntent::ACTION_VIEW)
      {
        item->SetProperty("jumpgate.playback_token", CVariant{admissionToken});
        item->SetProperty("jumpgate.lifecycle_token", CVariant{m_jumpgateBackLifecycleToken});
      }
      if (IsVideoDb(*item))
      {
        *(item->GetVideoInfoTag()) = XFILE::CVideoDatabaseFile::GetVideoTag(item->GetURL());
        item->SetPath(item->GetVideoInfoTag()->m_strFileNameAndPath);
      }
      // Set resume position if provided by caller (in external player mode)
      int64_t resumeMs = m_resumePositionMs.load(std::memory_order_relaxed);
      if (m_externalPlayerMode.load(std::memory_order_relaxed) && resumeMs > 0)
      {
        item->SetStartOffset(static_cast<int64_t>(resumeMs));
        CLog::Log(LOGINFO, "CXBMCApp: Setting start offset to {} ms for resume", resumeMs);
      }
      if (action == CJNIIntent::ACTION_VIEW)
      {
        playbackAccepted = QueueExternalPlayback(std::move(item), admissionGeneration,
                                                 admissionToken, resultRequestId);
      }
      else
      {
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_PLAY, 0, 0,
                                                   static_cast<void*>(item.release()));
        playbackAccepted = true;
      }
    }
    if (action == CJNIIntent::ACTION_VIEW && externalPlaybackPrepared && !playbackAccepted)
    {
      CommitExternalPlaybackAdmissionFailure(admissionToken);
      QueueExternalPlayerResult(admissionGeneration, resultRequestId,
                                m_wasStandalone.load(std::memory_order_relaxed));
    }
  }
  else if (action == ACTION_XBMC_RESUME)
  {
    if (m_playback_state != PLAYBACK_STATE_STOPPED)
    {
      if (m_playback_state & PLAYBACK_STATE_VIDEO)
        RequestVisibleBehind(true);
      if (!(m_playback_state & PLAYBACK_STATE_PLAYING))
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                   static_cast<void*>(new CAction(ACTION_PAUSE)));
    }
  }
}

void CXBMCApp::onActivityResult(int requestCode, int resultCode, CJNIIntent resultData)
{
}

void CXBMCApp::onVisibleBehindCanceled()
{
  CLog::Log(LOGDEBUG, "Visible Behind Cancelled");
  m_hasReqVisible = false;

  // Pressing the pause button calls OnStop() (cf. https://code.google.com/p/android/issues/detail?id=186469)
  if ((m_playback_state & PLAYBACK_STATE_PLAYING))
  {
    if (m_playback_state & PLAYBACK_STATE_CANNOT_PAUSE)
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                 static_cast<void*>(new CAction(ACTION_STOP)));
    else if (m_playback_state & PLAYBACK_STATE_VIDEO)
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                 static_cast<void*>(new CAction(ACTION_PAUSE)));
  }
}

void CXBMCApp::onBackLifecycleRetiring(
    KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken)
{
  if (lifecycleToken == m_jumpgateBackLifecycleToken)
    CancelExternalPlaybackForLifecycleTeardown();
}

bool CXBMCApp::DispatchExternalBack(
    const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context)
{
  bool playbackStarted = false;
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    playbackStarted = authorityTransaction.GetActiveToken() != 0;
  }
  if (!playbackStarted)
  {
    return context.Execute(
        [this]
        {
          const bool requestCancellation =
              call_method<jboolean>(m_context, "recordEarlyExternalPlayerBack", "()Z") == JNI_TRUE;
          return CancelPendingExternalPlaybackFromBack() || requestCancellation;
        });
  }
  return DispatchBackCommand(context, BackCommand::EXTERNAL_BACK);
}

bool CXBMCApp::DispatchKodiBack(
    const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context, bool longPress)
{
  return DispatchBackCommand(context, longPress ? BackCommand::KODI_LONG_BACK
                                                : BackCommand::KODI_SHORT_BACK);
}

bool CXBMCApp::OpenExternalSettings(
    const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context)
{
  return DispatchBackCommand(context, BackCommand::OPEN_SETTINGS);
}

bool CXBMCApp::DispatchBackCommand(
    const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context, BackCommand command)
{
  return QueueBackCommand(command, 0, 0, context);
}

bool CXBMCApp::QueueBackCommand(
    BackCommand command,
    uint64_t playbackGeneration,
    uint64_t playbackToken,
    std::optional<KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext> context)
{
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger == nullptr)
    return false;

  auto payload = std::make_unique<QueuedBackCommand>();
  payload->app = shared_from_this();
  payload->context = std::move(context);
  payload->command = command;
  payload->lifecycleToken = m_jumpgateBackLifecycleToken;
  const uint64_t currentGeneration = m_playbackResultState.CurrentGeneration();
  payload->playbackGeneration = playbackGeneration != 0 ? playbackGeneration : currentGeneration;
  payload->playbackToken = playbackToken;

  if (command == BackCommand::EXTERNAL_BACK && payload->playbackToken == 0)
  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    payload->playbackToken = authorityTransaction.GetActiveToken();
  }
  if ((command == BackCommand::EXTERNAL_BACK || command == BackCommand::CANCEL_PENDING_PLAYBACK) &&
      (payload->playbackGeneration == 0 || payload->playbackToken == 0))
  {
    return false;
  }

  return messenger->PostCallback(
      std::shared_ptr<KODI::MESSAGING::IApplicationCallback>(std::move(payload)));
}

bool CXBMCApp::ExecuteQueuedBackCommand(QueuedBackCommand& payload)
{
  if (payload.command == BackCommand::CANCEL_PENDING_PLAYBACK)
  {
    {
      std::lock_guard lock(m_externalPlaybackQueueMutex);
      if (m_pendingExternalPlaybackStopGeneration != payload.playbackGeneration ||
          m_pendingExternalPlaybackStopToken != payload.playbackToken)
      {
        return false;
      }
      m_pendingExternalPlaybackStopGeneration = 0;
      m_pendingExternalPlaybackStopToken = 0;
    }

    const CFileItem& currentItem = g_application.CurrentFileItem();
    const CVariant& currentLifecycle = currentItem.GetProperty("jumpgate.lifecycle_token");
    const CVariant& currentPlayback = currentItem.GetProperty("jumpgate.playback_token");
    const uint64_t lifecycleToken =
        currentLifecycle.isUnsignedInteger() ? currentLifecycle.asUnsignedInteger() : 0;
    const uint64_t playbackToken =
        currentPlayback.isUnsignedInteger() ? currentPlayback.asUnsignedInteger() : 0;
    if (lifecycleToken != payload.lifecycleToken || playbackToken != payload.playbackToken)
      return false;

    g_application.OnAction(CAction(ACTION_STOP));
    return true;
  }

  auto& dispatcher = CJNIMainActivity::GetJumpgateBackDispatcher();
  if (!dispatcher.IsCurrentLifecycle(payload.lifecycleToken))
  {
    CancelQueuedBackCommand(payload);
    return false;
  }

  if (payload.command == BackCommand::KODI_SHORT_BACK ||
      payload.command == BackCommand::KODI_LONG_BACK)
  {
    return ExecuteKodiBackCommand(payload.command == BackCommand::KODI_LONG_BACK);
  }

  if (payload.command == BackCommand::OPEN_SETTINGS)
    return ExecuteOpenExternalSettingsCommand();

  if (payload.command != BackCommand::EXTERNAL_BACK ||
      !m_externalPlayerMode.load(std::memory_order_relaxed) ||
      !m_playbackResultState.IsCurrent(payload.playbackGeneration))
  {
    return false;
  }

  {
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    if (authorityTransaction.GetActiveToken() != payload.playbackToken)
      return false;
  }

  CGUIComponent* gui = CServiceBroker::GetGUI();
  if (gui == nullptr)
    return false;

  CGUIDialog* videoOsd = gui->GetWindowManager().GetDialog(WINDOW_DIALOG_VIDEO_OSD);
  const bool osdVisible = videoOsd != nullptr && videoOsd->IsDialogRunning();
  if (osdVisible)
  {
    videoOsd->Close(true);
    return true;
  }
  if (CancelPendingExternalPlaybackFromBack())
    return true;
  g_application.OnAction(CAction(ACTION_STOP));
  return true;
}

void CXBMCApp::CancelQueuedBackCommand(const QueuedBackCommand& payload) noexcept
{
  // A rejected late-stop callback must leave the exact stop marker for the
  // in-flight playback callback to consume after PlayFile returns.
  static_cast<void>(payload);
}

bool CXBMCApp::QueueExternalPlayback(std::unique_ptr<CFileItem> item,
                                     uint64_t admissionGeneration,
                                     uint64_t admissionToken,
                                     std::string resultRequestId)
{
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger == nullptr || !item || admissionGeneration == 0 || admissionToken == 0)
    return false;

  auto payload = std::make_unique<QueuedExternalPlayback>();
  payload->app = shared_from_this();
  payload->item = std::move(item);
  payload->lifecycleToken = m_jumpgateBackLifecycleToken;
  payload->admissionGeneration = admissionGeneration;
  payload->admissionToken = admissionToken;
  payload->resultRequestId = std::move(resultRequestId);
  return messenger->PostCallback(
      std::shared_ptr<KODI::MESSAGING::IApplicationCallback>(std::move(payload)));
}

void CXBMCApp::ExecuteQueuedExternalPlayback(QueuedExternalPlayback& payload)
{
  const auto failAdmission = [&]
  {
    if (CommitExternalPlaybackAdmissionFailure(payload.admissionToken))
    {
      QueueExternalPlayerResult(payload.admissionGeneration, payload.resultRequestId,
                                m_wasStandalone.load(std::memory_order_relaxed));
    }
  };

  auto& dispatcher = CJNIMainActivity::GetJumpgateBackDispatcher();
  if (!dispatcher.IsCurrentLifecycle(payload.lifecycleToken))
  {
    failAdmission();
    return;
  }
  if (!m_externalPlayerMode.load(std::memory_order_relaxed) ||
      !m_playbackResultState.IsCurrent(payload.admissionGeneration))
  {
    failAdmission();
    return;
  }

  if (call_method<jboolean>(m_context, "isExternalPlayerResultCanceled", "(JLjava/lang/String;)Z",
                            static_cast<jlong>(payload.admissionGeneration),
                            jcast<jhstring>(payload.resultRequestId)) == JNI_TRUE)
  {
    failAdmission();
    return;
  }

  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger == nullptr || !payload.item)
  {
    failAdmission();
    return;
  }

  bool latestAdmission = false;
  {
    std::lock_guard lock(m_externalPlaybackQueueMutex);
    latestAdmission = IsLatestExternalPlaybackAdmission(payload.admissionToken);
    if (latestAdmission)
    {
      m_externalPlaybackDispatchGeneration = payload.admissionGeneration;
      m_externalPlaybackDispatchToken = payload.admissionToken;
      m_externalPlaybackDispatchRequestId = payload.resultRequestId;
    }
  }
  if (!latestAdmission)
  {
    failAdmission();
    return;
  }

  const auto app = shared_from_this();
  auto mediaPayload = messenger->PostMsgOwned(
      TMSG_MEDIA_PLAY, 0, 0, std::move(payload.item),
      [app, lifecycleToken = payload.lifecycleToken,
       admissionGeneration = payload.admissionGeneration, admissionToken = payload.admissionToken,
       resultRequestId = payload.resultRequestId]
      {
        app->CancelQueuedMediaPlayback(lifecycleToken, admissionGeneration, admissionToken,
                                       resultRequestId);
      });
  if (!mediaPayload)
  {
    failAdmission();
    return;
  }

  bool cancelBeforeExecution = false;
  {
    std::lock_guard lock(m_externalPlaybackQueueMutex);
    if (m_externalPlaybackDispatchGeneration != payload.admissionGeneration ||
        m_externalPlaybackDispatchToken != payload.admissionToken)
    {
      cancelBeforeExecution = true;
    }
    else
    {
      m_externalPlaybackDispatchPayload = mediaPayload;
      cancelBeforeExecution =
          m_pendingExternalPlaybackStopGeneration == payload.admissionGeneration &&
          m_pendingExternalPlaybackStopToken == payload.admissionToken;
    }
  }

  if (cancelBeforeExecution)
    mediaPayload->Cancel();
}

void CXBMCApp::CancelQueuedExternalPlayback(const QueuedExternalPlayback& payload) noexcept
{
  try
  {
    if (CommitExternalPlaybackAdmissionFailure(payload.admissionToken))
    {
      QueueExternalPlayerResult(payload.admissionGeneration, payload.resultRequestId,
                                m_wasStandalone.load(std::memory_order_relaxed));
    }
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "CXBMCApp: Failed to terminalize rejected owned playback callback");
  }
}

void CXBMCApp::CancelQueuedMediaPlayback(
    KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken,
    uint64_t admissionGeneration,
    uint64_t admissionToken,
    const std::string& resultRequestId) noexcept
{
  try
  {
    {
      std::lock_guard lock(m_externalPlaybackQueueMutex);
      if (m_externalPlaybackDispatchGeneration == admissionGeneration &&
          m_externalPlaybackDispatchToken == admissionToken)
      {
        m_externalPlaybackDispatchGeneration = 0;
        m_externalPlaybackDispatchToken = 0;
        m_externalPlaybackDispatchRequestId.clear();
        m_externalPlaybackDispatchPayload.reset();
      }
      if (m_pendingExternalPlaybackStopGeneration == admissionGeneration &&
          m_pendingExternalPlaybackStopToken == admissionToken)
      {
        m_pendingExternalPlaybackStopGeneration = 0;
        m_pendingExternalPlaybackStopToken = 0;
      }
    }

    if (!CJNIMainActivity::GetJumpgateBackDispatcher().IsCurrentLifecycle(lifecycleToken) ||
        !CommitExternalPlaybackAdmissionFailure(admissionToken))
    {
      return;
    }
    QueueExternalPlayerResult(admissionGeneration, resultRequestId,
                              m_wasStandalone.load(std::memory_order_relaxed));
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "CXBMCApp: Failed to terminalize canceled owned media payload");
  }
}

void CXBMCApp::CancelExternalPlaybackForLifecycleTeardown() noexcept
{
  try
  {
    uint64_t generation = 0;
    uint64_t token = 0;
    std::shared_ptr<KODI::MESSAGING::COwnedThreadMessagePayload> mediaPayload;
    {
      std::lock_guard lock(m_externalPlaybackQueueMutex);
      generation = m_externalPlaybackDispatchGeneration;
      token = m_externalPlaybackDispatchToken;
      if (generation == 0 || token == 0)
        return;

      m_pendingExternalPlaybackStopGeneration = generation;
      m_pendingExternalPlaybackStopToken = token;
      mediaPayload = m_externalPlaybackDispatchPayload;
    }

    if (mediaPayload && mediaPayload->Cancel())
      return;
    QueueBackCommand(BackCommand::CANCEL_PENDING_PLAYBACK, generation, token);
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "CXBMCApp: Failed to fence external playback during lifecycle teardown");
  }
}

bool CXBMCApp::QueueExternalPlayerResult(uint64_t generation,
                                         std::string requestId,
                                         bool wasStandalone)
{
  if (generation == 0 || requestId.empty())
    return false;

  auto payload = std::make_shared<QueuedExternalPlayerResult>();
  payload->app = shared_from_this();
  payload->lifecycleToken = m_jumpgateBackLifecycleToken;
  payload->generation = generation;
  payload->requestId = std::move(requestId);
  payload->wasStandalone = wasStandalone;

  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger == nullptr)
  {
    payload->Cancel();
    return false;
  }
  return messenger->PostAsyncCallback(payload);
}

void CXBMCApp::ExecuteQueuedExternalPlayerResult(const QueuedExternalPlayerResult& payload)
{
  const auto appTarget = CJNIMainActivity::AcquireAppInstance(payload.lifecycleToken);
  if (!appTarget || appTarget.get() != static_cast<CJNIMainActivity*>(this))
  {
    return;
  }

  auto lifecycleOperation = m_playbackResultState.TryBeginLifecycleOperation();
  if (!lifecycleOperation)
  {
    PostExternalPlayerResultConvergence(payload.generation, payload.requestId,
                                        payload.wasStandalone);
    return;
  }

  const auto owner = m_playbackResultState.CurrentOwner(*lifecycleOperation);
  if (!owner || owner->generation != payload.generation || owner->requestId != payload.requestId)
    return;
  DeliverPendingExternalPlayerResult(*lifecycleOperation);
}

void CXBMCApp::CancelQueuedExternalPlayerResult(const QueuedExternalPlayerResult& payload) noexcept
{
  const auto appTarget = CJNIMainActivity::AcquireAppInstance(payload.lifecycleToken);
  if (!appTarget || appTarget.get() != static_cast<CJNIMainActivity*>(this))
  {
    return;
  }
  PostExternalPlayerResultConvergence(payload.generation, payload.requestId,
                                      payload.wasStandalone);
}

void CXBMCApp::PostExternalPlayerResultConvergence(uint64_t generation,
                                                    const std::string& requestId,
                                                    bool wasStandalone) noexcept
{
  try
  {
    auto payload = std::make_unique<CVariant>(CVariant::VariantTypeObject);
    (*payload)["lifecycleToken"] = static_cast<uint64_t>(m_jumpgateBackLifecycleToken);
    (*payload)["generation"] = generation;
    (*payload)["requestId"] = requestId;
    (*payload)["wasStandalone"] = wasStandalone;

    const jboolean posted = call_method<jboolean>(
        m_context, "postExternalPlayerResultConvergence", "(JJ)Z",
        reinterpret_cast<jlong>(&CXBMCApp::ConvergeExternalPlayerResultCallback),
        reinterpret_cast<jlong>(payload.get()));
    if (posted == JNI_TRUE)
      payload.release();
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "CXBMCApp: Failed to post exact external-player result convergence");
  }
}

void CXBMCApp::ConvergeExternalPlayerResultCallback(CVariant* rawPayload)
{
  std::unique_ptr<CVariant> payload(rawPayload);
  if (!payload || !payload->isObject())
    return;

  const uint64_t lifecycleToken = (*payload)["lifecycleToken"].asUnsignedInteger();
  const uint64_t generation = (*payload)["generation"].asUnsignedInteger();
  const std::string requestId = (*payload)["requestId"].asString();
  const bool wasStandalone = (*payload)["wasStandalone"].asBoolean();
  const auto appTarget = CJNIMainActivity::AcquireAppInstance(lifecycleToken);
  if (!appTarget)
    return;

  auto* app = static_cast<CXBMCApp*>(appTarget.get());
  try
  {
    app->ConvergeExternalPlayerResult(generation, requestId, wasStandalone);
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "CXBMCApp: Exact external-player result convergence failed");
  }
}

void CXBMCApp::ConvergeExternalPlayerResult(uint64_t generation,
                                            const std::string& requestId,
                                            bool wasStandalone)
{
  auto lifecycleOperation = m_playbackResultState.BeginLifecycleOperation();
  const auto owner = m_playbackResultState.CurrentOwner(lifecycleOperation);
  if (!owner || owner->generation != generation || owner->requestId != requestId ||
      m_wasStandalone.load(std::memory_order_relaxed) != wasStandalone)
  {
    return;
  }
  DeliverPendingExternalPlayerResult(lifecycleOperation);
}

bool CXBMCApp::CancelPendingExternalPlaybackFromBack()
{
  auto lifecycleOperation = m_playbackResultState.TryBeginLifecycleOperation();
  if (!lifecycleOperation)
    return false;
  const uint64_t generation = m_playbackResultState.CurrentGeneration();
  const auto owner = m_playbackResultState.CurrentOwner(*lifecycleOperation);
  if (generation == 0 || !owner || owner->generation != generation)
    return false;

  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> canceled;
  bool queueLateStop = false;
  std::shared_ptr<KODI::MESSAGING::COwnedThreadMessagePayload> queuedMediaPayload;
  {
    std::lock_guard queueLock(m_externalPlaybackQueueMutex);
    auto authorityTransaction = m_playbackAuthority.BeginTransaction();
    canceled = authorityTransaction.CancelPendingAdmission(generation);
    if (!canceled)
      return false;

    queueLateStop = m_externalPlaybackDispatchGeneration == canceled->generation &&
                    m_externalPlaybackDispatchToken == canceled->token;
    if (queueLateStop)
    {
      m_pendingExternalPlaybackStopGeneration = canceled->generation;
      m_pendingExternalPlaybackStopToken = canceled->token;
      queuedMediaPayload = m_externalPlaybackDispatchPayload;
    }
  }

  if (queuedMediaPayload && queuedMediaPayload->Cancel())
    queueLateStop = false;

  if (!m_playbackResultState.Finish(canceled->generation, false))
    return false;
  if (m_traktScrobbler)
    m_traktScrobbler->CancelPlaybackGeneration(canceled->generation, canceled->token);
  if (m_jumpgateSubtitleController)
    m_jumpgateSubtitleController->OnPlaybackTerminal(canceled->generation);

  if (queueLateStop)
  {
    QueueBackCommand(BackCommand::CANCEL_PENDING_PLAYBACK, canceled->generation, canceled->token);
  }

  QueueExternalPlayerResult(canceled->generation, owner->requestId,
                            m_wasStandalone.load(std::memory_order_relaxed));
  return true;
}

bool CXBMCApp::ExecuteExternalBackCommand()
{
  if (!m_externalPlayerMode.load(std::memory_order_relaxed))
    return false;
  if (CancelPendingExternalPlaybackFromBack())
    return true;
  return QueueBackCommand(BackCommand::EXTERNAL_BACK);
}

bool CXBMCApp::ExecuteKodiBackCommand(bool longPress)
{
  if (!g_application.IsInitialized())
    return false;

  const uint32_t modifiers = longPress ? CKey::MODIFIER_LONG : 0;
  const unsigned int held = longPress ? KODI::KEYBOARD::KEY_HOLD_TRESHOLD + 1 : 0;
  const CKey key{XBMCK_BACKSPACE, XBMCVK_BACK, 0, 0, modifiers, 0, held};
  CServiceBroker::GetInputManager().ProcessKeyPress(key);
  return true;
}

bool CXBMCApp::ExecuteOpenExternalSettingsCommand()
{
  if (!m_externalPlayerMode.load(std::memory_order_relaxed))
    return false;

  m_settingsRequested.store(true, std::memory_order_relaxed);
  CLog::Log(LOGINFO, "CXBMCApp: Settings requested from native Back dispatcher");
  return true;
}

bool CXBMCApp::onBackInputEvent(const AInputEvent* event)
{
  if (event == nullptr || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY ||
      AKeyEvent_getKeyCode(event) != AKEYCODE_BACK)
  {
    return false;
  }

  auto& dispatcher = CJNIMainActivity::GetJumpgateBackDispatcher();
  const int64_t downTime = AKeyEvent_getDownTime(event);
  const int64_t eventTime = AKeyEvent_getEventTime(event);
  const int64_t heldNanos = eventTime > downTime ? eventTime - downTime : 0;
  const auto heldDuration =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds{heldNanos});
  const uint64_t sequenceId = downTime > 0 ? static_cast<uint64_t>(downTime) : 0;
  const int32_t flags = AKeyEvent_getFlags(event);
  const bool cancelled =
      (flags & AKEY_EVENT_FLAG_CANCELED) != 0 || (flags & AKEY_EVENT_FLAG_CANCELED_LONG_PRESS) != 0;
  const int32_t action = AKeyEvent_getAction(event);
  const bool down = action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_MULTIPLE;
  const bool repeat = action == AKEY_EVENT_ACTION_MULTIPLE || AKeyEvent_getRepeatCount(event) > 0;

  if (CJNIBuild::SDK_INT >= 36)
  {
    return dispatcher.OnApi36RawBack(m_jumpgateBackLifecycleToken, sequenceId, heldDuration, down,
                                     repeat, cancelled);
  }

  switch (action)
  {
    case AKEY_EVENT_ACTION_DOWN:
      if (cancelled)
        return dispatcher.OnLegacyRawUp(m_jumpgateBackLifecycleToken, sequenceId, heldDuration,
                                        true);
      return dispatcher.OnLegacyRawDown(m_jumpgateBackLifecycleToken, sequenceId, heldDuration,
                                        AKeyEvent_getRepeatCount(event) > 0);
    case AKEY_EVENT_ACTION_UP:
      return dispatcher.OnLegacyRawUp(m_jumpgateBackLifecycleToken, sequenceId, heldDuration,
                                      cancelled);
    case AKEY_EVENT_ACTION_MULTIPLE:
      return dispatcher.OnLegacyRawDown(m_jumpgateBackLifecycleToken, sequenceId, heldDuration,
                                        true);
  }

  return true;
}

void CXBMCApp::SetJumpgateBackInputReady(bool ready)
{
  CJNIMainActivity::GetJumpgateBackDispatcher().SetWindowReady(m_jumpgateBackLifecycleToken, ready);
}

void CXBMCApp::onVolumeChanged(int volume)
{
  // don't do anything. User wants to use kodi's internal volume freely while
  // using the external volume to change it relatively
  // See: https://forum.kodi.tv/showthread.php?tid=350764
}

void CXBMCApp::onAudioFocusChange(int focusChange)
{
  android_printf("Audio Focus changed: %d", focusChange);
  if (focusChange == CJNIAudioManager::AUDIOFOCUS_LOSS)
  {
    if ((m_playback_state & PLAYBACK_STATE_PLAYING))
    {
      if (m_playback_state & PLAYBACK_STATE_CANNOT_PAUSE)
        CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                   static_cast<void*>(new CAction(ACTION_STOP)));
      else
        CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                                                   static_cast<void*>(new CAction(ACTION_PAUSE)));
    }
  }
}

void CXBMCApp::InitFrameCallback(CVideoSyncAndroid* syncImpl)
{
  m_syncImpl = syncImpl;
}

void CXBMCApp::DeinitFrameCallback()
{
  m_syncImpl = nullptr;
}

void CXBMCApp::doFrame(int64_t frameTimeNanos)
{
  if (m_syncImpl)
    m_syncImpl->FrameCallback(frameTimeNanos);

  // Calculate the time, when next surface buffer should be rendered
  m_frameTimeNanos = frameTimeNanos;

  m_vsyncEvent.Set();
}

int64_t CXBMCApp::GetNextFrameTime() const
{
  if (m_refreshRate > 0.0001f)
    return m_frameTimeNanos + static_cast<int64_t>(1500000000ll / m_refreshRate);
  else
    return m_frameTimeNanos;
}

float CXBMCApp::GetFrameLatencyMs() const
{
  return (CurrentHostCounter() - m_frameTimeNanos) * 0.000001;
}

bool CXBMCApp::WaitVSync(unsigned int milliSeconds)
{
  return m_vsyncEvent.Wait(std::chrono::milliseconds(milliSeconds));
}

void CXBMCApp::SetupEnv()
{
  setenv("KODI_ANDROID_SYSTEM_LIBS", CJNISystem::getProperty("java.library.path").c_str(), 0);
  setenv("KODI_ANDROID_LIBS", getApplicationInfo().nativeLibraryDir.c_str(), 0);
  setenv("KODI_ANDROID_APK", getPackageResourcePath().c_str(), 0);

  std::string appName = CCompileInfo::GetAppName();
  StringUtils::ToLower(appName);
  std::string className = CCompileInfo::GetPackage();

  std::string cacheDir = getCacheDir().getAbsolutePath();
  std::string xbmcTemp = CJNISystem::getProperty("xbmc.temp", "");
  if (!xbmcTemp.empty())
  {
    setenv("KODI_TEMP", xbmcTemp.c_str(), 0);
  }

  std::string xbmcHome = CJNISystem::getProperty("xbmc.home", "");
  if (xbmcHome.empty())
  {
    setenv("KODI_BIN_HOME", (cacheDir + "/apk/assets").c_str(), 0);
    setenv("KODI_HOME", (cacheDir + "/apk/assets").c_str(), 0);
  }
  else
  {
    setenv("KODI_BIN_HOME", (xbmcHome + "/assets").c_str(), 0);
    setenv("KODI_HOME", (xbmcHome + "/assets").c_str(), 0);
  }
  setenv("KODI_BINADDON_PATH", (cacheDir + "/lib").c_str(), 0);

  std::string externalDir = CJNISystem::getProperty("xbmc.data", "");
  if (externalDir.empty())
  {
    CJNIFile androidPath = getExternalFilesDir("");
    if (!androidPath)
      androidPath = getDir(className, 1);

    if (androidPath)
      externalDir = androidPath.getAbsolutePath();
  }

  if (!externalDir.empty())
    setenv("HOME", externalDir.c_str(), 0);
  else
    setenv("HOME", getenv("KODI_TEMP"), 0);

  std::string pythonPath;
  if (xbmcHome.empty())
    pythonPath = cacheDir + "/apk/assets/python" + CCompileInfo::GetPythonVersion();
  else
    pythonPath = xbmcHome + "/assets/python" + CCompileInfo::GetPythonVersion();

  setenv("PYTHONHOME", pythonPath.c_str(), 1);
  setenv("PYTHONPATH", "", 1);
  setenv("PYTHONOPTIMIZE", "", 1);
  setenv("PYTHONNOUSERSITE", "1", 1);
}

std::string CXBMCApp::GetFilenameFromIntent(const CJNIIntent& intent)
{
  std::string ret;
  if (!intent)
    return ret;
  CJNIURI data = intent.getData();
  if (!data)
    return ret;
  std::string scheme = data.getScheme();
  StringUtils::ToLower(scheme);
  if (scheme == "content")
  {
    std::vector<std::string> filePathColumn;
    filePathColumn.push_back(CJNIMediaStoreMediaColumns::DATA);
    CJNICursor cursor = getContentResolver().query(data, filePathColumn, std::string(),
                                                   std::vector<std::string>(), std::string());
    if (cursor.moveToFirst())
    {
      int columnIndex = cursor.getColumnIndex(filePathColumn[0]);
      ret = cursor.getString(columnIndex);
    }
    cursor.close();
  }
  else if (scheme == "file")
    ret = data.getPath();
  else
    ret = data.toString();
  return ret;
}

std::shared_ptr<CNativeWindow> CXBMCApp::GetNativeWindow(int timeout) const
{
  if (!m_window)
    m_mainView->waitForSurface(timeout);

  return m_window;
}

// The map must contain keys "id" and "color", both are integers
void CXBMCApp::SetViewBackgroundColorCallback(void* mapVariant)
{
  CVariant* mapV = static_cast<CVariant*>(mapVariant);
  int viewId = (*mapV)["id"].asInteger();
  int color = (*mapV)["color"].asInteger();

  delete mapV;

  CJNIView view = findViewById(viewId);
  if (view)
  {
    view.setBackgroundColor(color);
  }
}

void CXBMCApp::SetVideoLayoutBackgroundColor(const int color)
{
  CJNIResources resources = CJNIContext::getResources();
  if (resources)
  {
    int id = resources.getIdentifier("VideoLayout", "id", CJNIContext::getPackageName());
    if (id > 0)
    {
      // this object is deallocated in the callback
      CVariant* msg = new CVariant(CVariant::VariantTypeObject);
      (*msg)["id"] = id;
      (*msg)["color"] = color;

      runNativeOnUiThread(SetViewBackgroundColorCallback, msg);
    }
  }
}

void CXBMCApp::RegisterInputDeviceCallbacks(IInputDeviceCallbacks* handler)
{
  if (handler != nullptr)
    m_inputDeviceCallbacks = handler;
}

void CXBMCApp::UnregisterInputDeviceCallbacks()
{
  m_inputDeviceCallbacks = nullptr;
}

void CXBMCApp::onInputDeviceAdded(int deviceId)
{
  android_printf("Input device added: %d", deviceId);

  if (m_inputDeviceCallbacks != nullptr)
    m_inputDeviceCallbacks->OnInputDeviceAdded(deviceId);
}

void CXBMCApp::onInputDeviceChanged(int deviceId)
{
  android_printf("Input device changed: %d", deviceId);

  if (m_inputDeviceCallbacks != nullptr)
    m_inputDeviceCallbacks->OnInputDeviceChanged(deviceId);
}

void CXBMCApp::onInputDeviceRemoved(int deviceId)
{
  android_printf("Input device removed: %d", deviceId);

  if (m_inputDeviceCallbacks != nullptr)
    m_inputDeviceCallbacks->OnInputDeviceRemoved(deviceId);
}

void CXBMCApp::RegisterInputDeviceEventHandler(IInputDeviceEventHandler* handler)
{
  if (handler != nullptr)
    m_inputDeviceEventHandler = handler;
}

void CXBMCApp::UnregisterInputDeviceEventHandler()
{
  m_inputDeviceEventHandler = nullptr;
}

bool CXBMCApp::onInputDeviceEvent(const AInputEvent* event)
{
  // This seam is reserved for joystick/peripheral traffic. Ordinary Back keys are
  // coordinated in CEventLoop before CAndroidKey receives them.
  if (m_externalPlayerMode.load(std::memory_order_relaxed) &&
      AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
  {
    int32_t keycode = AKeyEvent_getKeyCode(event);
    int32_t action = AKeyEvent_getAction(event);
    // AKEYCODE_MENU = 82, AKEYCODE_GUIDE = 172
    if ((keycode == 82 || keycode == 172) && action == AKEY_EVENT_ACTION_UP)
    {
      m_settingsRequested = true;
      return true;
    }
  }

  if (m_inputDeviceEventHandler != nullptr)
    return m_inputDeviceEventHandler->OnInputDeviceEvent(event);

  return false;
}

void CXBMCApp::onDisplayAdded(int displayId)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::onDisplayChanged(int displayId)
{
  CLog::Log(LOGDEBUG, "CXBMCApp::{}: id: {}", __FUNCTION__, displayId);

  if (!g_application.IsInitialized())
    // Display mode has been changed during app startup; we want to reset audio engine on next ACTION_HDMI_AUDIO_PLUG event
    m_aeReset = true;

  // Update display modes
  CWinSystemAndroid* winSystemAndroid =
      dynamic_cast<CWinSystemAndroid*>(CServiceBroker::GetWinSystem());
  if (winSystemAndroid)
    winSystemAndroid->UpdateDisplayModes();

  m_displayChangeEvent.Set();
  m_inputHandler.setDPI(GetDPI());
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::onDisplayRemoved(int displayId)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::surfaceChanged(CJNISurfaceHolder holder, int format, int width, int height)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
}

void CXBMCApp::surfaceCreated(CJNISurfaceHolder holder)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);

  m_window = CNativeWindow::CreateFromSurface(holder);
  if (m_window == nullptr)
  {
    android_printf(" => invalid ANativeWindow object");
    return;
  }

  if (!m_firstrun)
    XBMC_SetupDisplay();

  auto& components = CServiceBroker::GetAppComponents();
  const auto appPower = components.GetComponent<CApplicationPowerHandling>();
  appPower->SetRenderGUI(true);
}

void CXBMCApp::surfaceDestroyed(CJNISurfaceHolder holder)
{
  android_printf("%s: ", __PRETTY_FUNCTION__);
  // If we have exited XBMC, it no longer exists.
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPower = components.GetComponent<CApplicationPowerHandling>();
  appPower->SetRenderGUI(false);
  if (!m_exiting)
    XBMC_DestroyDisplay();

  m_window.reset();
}
