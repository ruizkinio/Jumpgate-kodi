/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "XBMCApp.h"

#include "JumpgateThresholds.h"
#include "AndroidJumpgateCredentialStore.h"
#include "JumpgateProfileStorage.h"
#include "SubtitleDownloader.h"
#include "TraktScrobbler.h"

#include "AndroidKey.h"
#include "CompileInfo.h"
#include "FileItem.h"
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
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "filesystem/VideoDatabaseFile.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogSelect.h"
#include "dialogs/GUIDialogYesNo.h"
#include "guilib/GUIKeyboardFactory.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/guiinfo/GUIInfoLabels.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
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
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/JumpgateProfileRuntime.h"
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
#include <memory>
#include <mutex>
#include <cstdio>
#include <sstream>
#include <stdlib.h>
#include <thread>
#include <string.h>
#include <time.h>

#include <android/bitmap.h>
#include <android/input.h>
#include <android/configuration.h>
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

#define PLAYBACK_STATE_STOPPED  0x0000
#define PLAYBACK_STATE_PLAYING  0x0001
#define PLAYBACK_STATE_VIDEO    0x0100
#define PLAYBACK_STATE_AUDIO    0x0200
#define PLAYBACK_STATE_CANNOT_PAUSE 0x0400

using namespace ANNOUNCEMENT;
using namespace jni;
using namespace KODI::GUILIB;
using namespace KODI::VIDEO;
using namespace std::chrono_literals;

// Forward declaration for static helper used in both Deinitialize() and onNewIntent()
static void SaveResumeForContent(const std::string& imdbId, int season, int episode,
                                 int64_t posMs, int64_t durMs,
                                  const std::string& bridgeUrl);

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
         model.find("emulator") != std::string::npos ||
         brand.find("generic") != std::string::npos;
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
        CJNISystemProperties::get("ro.hardware", ""), CJNISystemProperties::get("ro.product.model", ""));
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

std::unique_ptr<CXBMCApp> CXBMCApp::m_appinstance;

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
  m_mainView = std::make_unique<CJNIXBMCMainView>(this);
  m_hdmiSource = CJNISystemProperties::get("ro.hdmi.device_type", "") == "4";
  android_printf("CXBMCApp: Created");

  // crossguid requires init on android only once on process start
  JNIEnv* env = xbmc_jnienv();
  xg::initJni(env);
}

CXBMCApp::~CXBMCApp()
{
  StopBridgePairingWorker(true);
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
    if (message == "OnPlay" || message == "OnResume")
      OnPlayBackStarted();
    else if (message == "OnPause")
      OnPlayBackPaused();
    else if (message == "OnStop")
      OnPlayBackStopped();
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
    jboolean extMode = call_method<jboolean>(m_context,
                                              "isExternalPlayerMode", "()Z");
    if (extMode)
    {
      m_externalPlayerMode.store(true, std::memory_order_relaxed);
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

  StopBridgePairingWorker(true);

  // Safety-net: save resume position to local file (F-008)
  // Local file I/O only -- no Bridge POST (too slow for onDestroy)
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler)
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
        // Empty bridgeUrl skips the Bridge POST (SaveResumeForContent supports this)
        SaveResumeForContent(imdb, season, episode, posMs, durMs, "");
        android_printf("Jumpgate: onDestroy safety-net resume saved for %s pos=%lld dur=%lld",
                       imdb.c_str(), (long long)posMs, (long long)durMs);
      }
    }
  }

  if (m_subtitleDownloader)
  {
    m_subtitleDownloader->Deinitialize();
    m_subtitleDownloader.reset();
  }

  if (m_traktScrobbler)
  {
    m_traktScrobbler->Deinitialize();
    m_traktScrobbler.reset();
  }

  unregisterReceiver(*this);

  UnregisterDisplayListener();
  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);

  m_mediaSession.release();
}

void CXBMCApp::onSaveState(void **data, size_t *size)
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

    m_traktScrobbler = std::make_unique<TraktScrobbler>();
    ApplyActiveJumpgateProfile();
    m_traktScrobbler->Initialize();

    m_subtitleDownloader = std::make_unique<SubtitleDownloader>();
    m_subtitleDownloader->Initialize();

    // Pass subtitle language from settings
    std::string subLangs = GetSettingString("subtitle_languages", "en");
    m_subtitleDownloader->SetLanguages(subLangs);
  }
}

void CXBMCApp::Deinitialize()
{
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
  CVariant *variant = new CVariant(vmap);
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
  AConfiguration *config = AConfiguration_new();
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
    RESOLUTION_INFO renderRes = CDisplaySettings::GetInstance().GetResolutionInfo(CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution());
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

void CXBMCApp::OnPlayBackStarted()
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
}

void CXBMCApp::OnPlayBackPaused()
{
  CLog::Log(LOGDEBUG, "{}", __PRETTY_FUNCTION__);

  m_playback_state &= ~PLAYBACK_STATE_PLAYING;
  m_mediaSessionUpdated = false;
  UpdateSessionState();

  RequestVisibleBehind(false);
  ReleaseAudioFocus();
}

void CXBMCApp::OnPlayBackStopped()
{
  CLog::Log(LOGDEBUG, "{}", __PRETTY_FUNCTION__);

  m_playback_state = PLAYBACK_STATE_STOPPED;
  m_mediaSessionUpdated = false;
  UpdateSessionState();
  m_mediaSession->activate(false);

  RequestVisibleBehind(false);
  CAndroidKey::SetHandleMediaKeys(true);
  ReleaseAudioFocus();
}

void CXBMCApp::ExitExternalPlayerMode(bool completed)
{
  // Kill timing analysis (F-012):
  // ProcessSlow updates m_lastPlaybackTimeMs every ~500ms. ExitExternalPlayerMode
  // reads the atomic values, which may be stale by up to 500ms. For a 2-hour movie
  // this is 0.007% -- negligible for resume purposes. We do NOT snap fresh position
  // from the player here because ApplicationPlayer may already be shutting down
  // (OnStop announcement already fired) and GetTime() could return 0.

  if (!m_externalPlayerMode.load(std::memory_order_relaxed))
    return;

  int64_t posMs = m_lastPlaybackTimeMs.load(std::memory_order_relaxed);
  int64_t durMs = m_lastPlaybackDurationMs.load(std::memory_order_relaxed);

  CLog::Log(LOGINFO, "CXBMCApp: Exiting external player mode (completed={}, pos={}, dur={})",
            completed, posMs, durMs);

  // Save resume position before exiting
  SaveResumePosition();

  bool wasStandalone = m_wasStandalone.load(std::memory_order_relaxed);

  if (wasStandalone)
  {
    CLog::Log(LOGINFO, "CXBMCApp: Returning to standalone mode (warm transition)");

    // Call Java-side with wasStandalone=true so it does NOT kill the process
    call_method<void>(m_context,
                      "exitExternalPlayerMode", "(JJZZ)V",
                      static_cast<jlong>(posMs),
                      static_cast<jlong>(durMs),
                      static_cast<jboolean>(completed),
                      static_cast<jboolean>(true));

    // Clean up C++ side external player state
    ReturnToStandaloneMode();
  }
  else
  {
    m_externalPlayerMode.store(false, std::memory_order_relaxed); // prevent re-entry

    // Call Java-side with wasStandalone=false (cold launch: finish + killProcess)
    call_method<void>(m_context,
                      "exitExternalPlayerMode", "(JJZZ)V",
                      static_cast<jlong>(posMs),
                      static_cast<jlong>(durMs),
                      static_cast<jboolean>(completed),
                      static_cast<jboolean>(false));
  }
}

void CXBMCApp::ReturnToStandaloneMode()
{
  CLog::Log(LOGINFO, "CXBMCApp: Returning to standalone mode");

  // Deinitialize and destroy TraktScrobbler so it doesn't fire during standalone playback
  if (m_traktScrobbler)
  {
    m_traktScrobbler->Deinitialize();
    m_traktScrobbler.reset();
  }

  // Deinitialize and destroy SubtitleDownloader
  if (m_subtitleDownloader)
  {
    m_subtitleDownloader->Deinitialize();
    m_subtitleDownloader.reset();
  }

  // Reset external player state
  m_externalPlayerMode.store(false, std::memory_order_relaxed);
  m_resumePositionMs.store(0, std::memory_order_relaxed);
  m_resumeApplied.store(false, std::memory_order_relaxed);
  m_lastPlaybackTimeMs.store(0, std::memory_order_relaxed);
  m_lastPlaybackDurationMs.store(0, std::memory_order_relaxed);
  m_wasStandalone.store(false, std::memory_order_relaxed);

  CLog::Log(LOGINFO, "CXBMCApp: Returned to standalone mode, TraktScrobbler and SubtitleDownloader deinitialized");
}

void CXBMCApp::SaveResumePosition()
{
  if (!m_traktScrobbler)
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
  bool completed = (durMs > 0 && posMs > 0 &&
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
    entry["timestamp"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
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

  // POST to Bridge /resume (fire-and-forget, 2s timeout)
  {
    XFILE::CCurlFile curl;
    curl.SetRequestHeader("Content-Type", "application/json");
    curl.SetTimeout(2);

    CVariant body(CVariant::VariantTypeObject);
    body["imdb"] = imdb;
    body["season"] = (season >= 0) ? std::to_string(season) : "";
    body["episode"] = (episode >= 0) ? std::to_string(episode) : "";
    body["position"] = posMs;
    body["duration"] = durMs;

    std::string bodyJson;
    CJSONVariantWriter::Write(body, bodyJson, true);

    std::string bridgeUrl = m_traktScrobbler->GetBridgeUrl() + "/resume";
    std::string response;
    curl.Post(bridgeUrl, bodyJson, response);
    // Ignore errors — Bridge may not be running
  }
}

int CXBMCApp::LoadResumePosition(const std::string& imdbId, int season, int episode)
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

  return static_cast<int>(pos);
}

void CXBMCApp::OnContentIdentified()
{
  if (!m_traktScrobbler || !m_traktScrobbler->IsContentIdentified())
    return;

  std::string imdb = m_traktScrobbler->GetImdbId();
  if (imdb.empty())
    return;

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
    toastMsg = imdb;
    if (season >= 0 && episode >= 0)
      toastMsg += " S" + std::to_string(season) + "E" + std::to_string(episode);
  }
  CGUIDialogKaiToast::QueueNotification(
      CGUIDialogKaiToast::Info, "Jumpgate", "Identified: " + toastMsg, 5000, true);

  // Check resume position: Bridge → local store → Trakt sync
  int savedPos = m_traktScrobbler->GetBridgeResumePosition();
  if (savedPos <= 0)
    savedPos = LoadResumePosition(imdb, season, episode);
  if (savedPos <= 0)
    savedPos = m_traktScrobbler->GetTraktResumePosition();

  if (savedPos > 0 && m_resumePositionMs.load(std::memory_order_relaxed) <= 0)
  {
    auto& components = CServiceBroker::GetAppComponents();
    const auto appPlayer = components.GetComponent<CApplicationPlayer>();
    // Only seek if player is in the first 60 seconds (avoid disrupting user if they've been watching)
    if (appPlayer->GetTime() < 60000)
    {
      appPlayer->SeekTime(savedPos);
      CLog::Log(LOGINFO, "CXBMCApp: Late resume to {} ms (content identified after playback start)", savedPos);
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Info, "Jumpgate", "Resuming playback", 3000, true);
    }
  }

  m_resumeApplied.store(true, std::memory_order_relaxed);
  m_traktScrobbler->ClearBridgeResume();

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
    title = !episodeTitle.empty() ? episodeTitle : (!showOrMovieTitle.empty() ? showOrMovieTitle : imdb);

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

  if (!force && title == m_lastOverlayTitle && meta == m_lastOverlayMeta && logoUrl == m_lastOverlayLogoUrl)
    return;

  m_lastOverlayTitle = title;
  m_lastOverlayMeta = meta;
  m_lastOverlayLogoUrl = logoUrl;

  call_method<void>(m_context, "updateLoadingOverlayContentInfo",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                    jcast<jhstring>(title),
                    jcast<jhstring>(meta),
                    jcast<jhstring>(logoUrl));
}

// --- Secure profile runtime and settings ---

bool CXBMCApp::InitializeJumpgateProfileRuntime()
{
  if (!m_jumpgateProfileStorage)
    m_jumpgateProfileStorage = std::make_unique<KODI::JUMPGATE::CJumpgateProfileStorage>(
        "special://profile/jumpgate_settings.json");
  if (!m_jumpgateCredentialStore)
    m_jumpgateCredentialStore =
        std::make_unique<KODI::JUMPGATE::CAndroidJumpgateCredentialStore>();
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
    m_traktScrobbler->SetBridgeProfile(active.profileId, active.bridgeOrigin,
                                       active.bridgeBaseUrl, active.deviceToken,
                                       active.traktEnabled && active.credentialsValid);
  }
  else
  {
    m_traktScrobbler->ClearBridgeProfile();
  }

  if (m_subtitleDownloader)
    m_subtitleDownloader->SetLanguages(active.subtitleLanguages);
  active.ClearSecrets();
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
  if (!m_jumpgateProfileRuntime->SetActiveSetting(key, CVariant{value}, error))
    CLog::Log(LOGERROR, "CXBMCApp: Jumpgate setting update rejected: {}", error);
  ApplyActiveJumpgateProfile();
}

void CXBMCApp::SetSetting(const std::string& key, bool value)
{
  if (!m_jumpgateProfileRuntime)
    return;
  std::string error;
  if (!m_jumpgateProfileRuntime->SetActiveSetting(key, CVariant{value}, error))
    CLog::Log(LOGERROR, "CXBMCApp: Jumpgate setting update rejected: {}", error);
  ApplyActiveJumpgateProfile();
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
  std::string authority = normalized.substr(
      authorityStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);
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
  m_pairingErrorPending = false;
  m_pairingErrorMessage.clear();
}

void CXBMCApp::QueuePairingError(const std::string& errorMessage)
{
  std::unique_lock lock(m_pairingMutex);
  m_pairingErrorPending = true;
  m_pairingErrorMessage = errorMessage;
  m_pairingRedemptionPending = false;
  ClearSensitiveString(m_pairingRedemptionJson);
  m_pairingRedemptionOrigin.clear();
  m_pairingApplyProfileName.clear();
}

void CXBMCApp::StopBridgePairingWorker(bool clearPendingState)
{
  m_pairingStopRequested.store(true, std::memory_order_relaxed);

  if (m_pairingThread.joinable())
    m_pairingThread.join();

  m_pairingInProgress.store(false, std::memory_order_relaxed);
  m_pairingStopRequested.store(false, std::memory_order_relaxed);

  if (clearPendingState)
  {
    std::unique_lock lock(m_pairingMutex);
    m_pairingRedemptionPending = false;
    ClearSensitiveString(m_pairingRedemptionJson);
    m_pairingRedemptionOrigin.clear();
    m_pairingApplyProfileName.clear();
    m_pairingErrorPending = false;
    m_pairingErrorMessage.clear();
  }
}

void CXBMCApp::StartBridgePairing()
{
  if (m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO))
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Jumpgate",
                                          "Stop playback before pairing a profile", 4000,
                                          true);
    return;
  }

  if (m_pairingInProgress.load(std::memory_order_relaxed))
  {
    CGUIDialogKaiToast::QueueNotification(
        CGUIDialogKaiToast::Info, "Jumpgate", "Pairing already in progress", 3000, true);
    return;
  }

  StopBridgePairingWorker(false); // join stale completed worker, if any
  m_pairingStopRequested.store(false, std::memory_order_relaxed);
  m_pairingInProgress.store(true, std::memory_order_relaxed);

  {
    std::unique_lock lock(m_pairingMutex);
    m_pairingRedemptionPending = false;
    ClearSensitiveString(m_pairingRedemptionJson);
    m_pairingRedemptionOrigin.clear();
    m_pairingApplyProfileName.clear();
    m_pairingErrorPending = false;
    m_pairingErrorMessage.clear();
  }

  auto failStart = [this](const std::string& msg) {
    CLog::Log(LOGWARNING, "CXBMCApp: Pairing start failed: {}", msg);
    QueuePairingError(msg);
    m_pairingInProgress.store(false, std::memory_order_relaxed);
  };

  if (!InitializeJumpgateProfileRuntime())
  {
    failStart("Pairing failed: secure profile runtime is unavailable");
    return;
  }

  // Capture one immutable origin. Code issuance, polling, response validation,
  // and credential commit all remain bound to this exact origin.
  const std::string origin = m_jumpgateProfileRuntime->GetPairingOrigin();
  if (!KODI::JUMPGATE::IsValidPairingOrigin(origin, true))
  {
    failStart("Pairing failed: invalid Bridge origin");
    return;
  }

  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetTimeout(8);

  CVariant body(CVariant::VariantTypeObject);
  std::string bodyJson;
  if (!CJSONVariantWriter::Write(body, bodyJson, true))
    bodyJson = "{}";

  std::string response;
  if (!curl.Post(NoRedirectUrl(origin + "/pair/device/code"), bodyJson, response))
  {
    failStart("Pairing failed: unable to reach Bridge");
    return;
  }

  CVariant data;
  if (!CJSONVariantParser::Parse(response, data))
  {
    failStart("Pairing failed: invalid Bridge response");
    return;
  }

  if (data.isMember("ok") && !data["ok"].asBoolean())
  {
    const std::string err =
        data.isMember("error") ? data["error"].asString() : std::string("Pairing start rejected");
    failStart(err);
    return;
  }

  std::string userCode = data.isMember("user_code") ? data["user_code"].asString() : "";
  if (userCode.empty())
    userCode = data.isMember("userCode") ? data["userCode"].asString() : "";

  std::string deviceCode = data.isMember("device_code") ? data["device_code"].asString() : "";
  if (deviceCode.empty())
    deviceCode = data.isMember("deviceCode") ? data["deviceCode"].asString() : "";

  std::string verificationUrl =
      data.isMember("verification_url") ? data["verification_url"].asString() : "";
  if (verificationUrl.empty())
    verificationUrl = data.isMember("verificationUrl") ? data["verificationUrl"].asString() : "";

  int expiresIn =
      data.isMember("expires_in") ? static_cast<int>(data["expires_in"].asInteger(0)) : 0;
  if (expiresIn <= 0)
    expiresIn = data.isMember("expiresIn") ? static_cast<int>(data["expiresIn"].asInteger(0)) : 0;
  int interval = data.isMember("interval") ? static_cast<int>(data["interval"].asInteger(0)) : 0;

  if (verificationUrl.empty())
    verificationUrl = origin + "/configure";
  if (expiresIn <= 0)
    expiresIn = 600;
  if (interval <= 0)
    interval = 2;

  if (userCode.empty() || deviceCode.empty())
  {
    failStart("Pairing failed: invalid code response");
    return;
  }

  CGUIDialogOK::ShowAndGetInput(
      CVariant{"Pair Jumpgate"},
      CVariant{"On your phone or laptop, open:"},
      CVariant{verificationUrl},
      CVariant{"Enter code: " + userCode});

  m_pairingThread = std::thread([this, origin, deviceCode, expiresIn, interval]() {
    auto getStringOrEmpty = [](const CVariant& payload, const char* key) {
      return payload.isMember(key) ? payload[key].asString() : std::string{};
    };

    const int pollIntervalSec = std::max(1, interval);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn);

    while (!m_pairingStopRequested.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline)
    {
      CVariant pollBody(CVariant::VariantTypeObject);
      pollBody["device_code"] = deviceCode;

      std::string pollBodyJson;
      if (!CJSONVariantWriter::Write(pollBody, pollBodyJson, true))
      {
        if (!m_pairingStopRequested.load(std::memory_order_relaxed))
          QueuePairingError("Pairing failed: request serialization error");
        m_pairingInProgress.store(false, std::memory_order_relaxed);
        return;
      }

      XFILE::CCurlFile pollCurl;
      pollCurl.SetRequestHeader("Content-Type", "application/json");
      pollCurl.SetTimeout(8);

      std::string pollResponse;
      if (!pollCurl.Post(NoRedirectUrl(origin + "/pair/device/token"), pollBodyJson,
                         pollResponse))
      {
        if (!m_pairingStopRequested.load(std::memory_order_relaxed))
          QueuePairingError("Pairing failed: unable to reach Bridge");
        m_pairingInProgress.store(false, std::memory_order_relaxed);
        return;
      }

      CVariant pollData;
      if (!CJSONVariantParser::Parse(pollResponse, pollData))
      {
        ClearSensitiveString(pollResponse);
        if (!m_pairingStopRequested.load(std::memory_order_relaxed))
          QueuePairingError("Pairing failed: invalid token response");
        m_pairingInProgress.store(false, std::memory_order_relaxed);
        return;
      }

      if (pollData.isMember("ok") && !pollData["ok"].asBoolean())
      {
        std::string err = getStringOrEmpty(pollData, "error");
        if (err.empty())
          err = getStringOrEmpty(pollData, "message");
        if (err.empty())
          err = "Pairing failed";
        ClearSensitiveString(pollResponse);
        if (!m_pairingStopRequested.load(std::memory_order_relaxed))
          QueuePairingError(err);
        m_pairingInProgress.store(false, std::memory_order_relaxed);
        return;
      }

      const bool paired = pollData.isMember("paired") && pollData["paired"].asBoolean();
      if (paired)
      {
        std::string profileName = getStringOrEmpty(pollData, "name");
        if (profileName.empty())
          profileName = getStringOrEmpty(pollData, "profile_name");
        if (profileName.empty())
          profileName = getStringOrEmpty(pollData, "profileName");

        // Commit through AndroidKeyStore on Kodi's main thread. The polling
        // worker never makes JNI calls or mutates the active profile runtime.
        QueuePairingRedemption(std::move(pollResponse), origin, profileName);
        ClearSensitiveString(pollResponse);
        m_pairingInProgress.store(false, std::memory_order_relaxed);
        return;
      }

      ClearSensitiveString(pollResponse);

      const int sleepMs = pollIntervalSec * 1000;
      for (int elapsed = 0; elapsed < sleepMs; elapsed += 200)
      {
        if (m_pairingStopRequested.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }

    if (!m_pairingStopRequested.load(std::memory_order_relaxed))
      QueuePairingError("Pairing expired, try again");

    m_pairingInProgress.store(false, std::memory_order_relaxed);
  });
}

void CXBMCApp::CheckForUpdate()
{
  if (!m_traktScrobbler)
    return;

  std::string bridgeUrl = m_traktScrobbler->GetBridgeUrl();
  if (bridgeUrl.empty())
    return;

  // Configured Bridge URLs include /_c/<config>; version endpoint lives at host root.
  std::string bridgeOrigin = GetBridgeOriginFromUrl(bridgeUrl);
  if (bridgeOrigin.empty())
    bridgeOrigin = bridgeUrl;

  XFILE::CCurlFile curl;
  curl.SetTimeout(3);
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

  bool isNewer = (remoteMajor > localMajor) ||
                 (remoteMajor == localMajor && remoteMinor > localMinor) ||
                 (remoteMajor == localMajor && remoteMinor == localMinor && remotePatch > localPatch);

  if (isNewer)
  {
    CLog::Log(LOGINFO, "CXBMCApp: Update available: {} -> {}", JUMPGATE_VERSION, remoteVersion);
    CGUIDialogKaiToast::QueueNotification(
        CGUIDialogKaiToast::Info, "Jumpgate",
        "Update available: v" + remoteVersion, 7000, true);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CXBMCApp: Version up to date ({})", JUMPGATE_VERSION);
  }
}

void CXBMCApp::ShowJumpgateProfilePicker(bool removeProfile)
{
  if (m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO))
  {
    CGUIDialogKaiToast::QueueNotification(
        CGUIDialogKaiToast::Warning, "Jumpgate",
        "Stop playback before changing the active profile", 4000, true);
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
  dialog->SetHeading(CVariant{removeProfile ? "Forget Jumpgate Profile"
                                            : "Select Jumpgate Profile"});
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
  if (removeProfile)
  {
    if (!CGUIDialogYesNo::ShowAndGetInput(
            CVariant{"Forget Jumpgate Profile"},
            CVariant{"Remove " + profile.name + " and its encrypted local credential?"}))
      return;
    changed = m_jumpgateProfileRuntime->ForgetLocal(profile.profileId, profile.deviceId, error);
  }
  else
  {
    changed = m_jumpgateProfileRuntime->SelectActive(profile.profileId, error);
  }

  if (!changed)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate",
                                          error.empty() ? "Profile update failed" : error,
                                          5000, true);
    return;
  }

  ApplyActiveJumpgateProfile();
  CGUIDialogKaiToast::QueueNotification(
      error.empty() ? CGUIDialogKaiToast::Info : CGUIDialogKaiToast::Warning, "Jumpgate",
      error.empty() ? (removeProfile ? "Profile forgotten" : "Active profile updated")
                    : error,
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
    if (m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO))
    {
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Warning, "Jumpgate",
          "Stop playback before clearing the active profile", 4000, true);
      return;
    }
    auto active = m_jumpgateProfileRuntime->GetActive();
    if (!active.selected)
    {
      active.ClearSecrets();
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                            "Unpaired local mode is already active", 3000,
                                            true);
      return;
    }
    const std::string profileName = active.name.empty() ? "the active profile" : active.name;
    active.ClearSecrets();
    if (!CGUIDialogYesNo::ShowAndGetInput(
            CVariant{"Use Unpaired Local Mode"},
            CVariant{"Stop using " + profileName + " for this Kodi profile?"}))
      return;
    std::string error;
    if (!m_jumpgateProfileRuntime->ClearActive(error))
    {
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "Jumpgate", error,
                                            5000, true);
      return;
    }
    ApplyActiveJumpgateProfile();
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Jumpgate",
                                          "Using unpaired local mode", 4000, true);
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
        CVariant{"Finish Trakt and addon setup in the browser. The approved profile is applied automatically."},
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
  dialog->Add(active.selected ? "Trakt (managed by active profile)" :
                                "Trakt (pair a Jumpgate profile)");
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
        CGUIDialogKaiToast::QueueNotification(
            CGUIDialogKaiToast::Info, "Jumpgate",
            "Subtitle languages: " + langs, 3000, true);
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
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Info, "Jumpgate",
          std::string("Jumpgate v") + JUMPGATE_VERSION + " - External Player for Stremio",
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

  std::string pairingRedemptionJson;
  std::string pairingRedemptionOrigin;
  std::string pairingProfileName;
  std::string pairingErrorMessage;
  {
    std::unique_lock lock(m_pairingMutex);
    if (m_pairingRedemptionPending &&
        !(m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO)))
    {
      pairingRedemptionJson.swap(m_pairingRedemptionJson);
      pairingRedemptionOrigin.swap(m_pairingRedemptionOrigin);
      pairingProfileName.swap(m_pairingApplyProfileName);
      m_pairingRedemptionPending = false;
      m_pairingApplyProfileName.clear();
      m_pairingErrorPending = false;
      m_pairingErrorMessage.clear();
    }
    else if (m_pairingErrorPending)
    {
      pairingErrorMessage = m_pairingErrorMessage;
      m_pairingErrorPending = false;
      m_pairingErrorMessage.clear();
    }
  }

  if (!pairingRedemptionJson.empty())
  {
    CVariant redemption;
    if (!CJSONVariantParser::Parse(pairingRedemptionJson, redemption) ||
        !redemption.isObject())
    {
      pairingErrorMessage = "Pairing failed: invalid redemption response";
    }
    ClearSensitiveString(pairingRedemptionJson);

    std::string storeError;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (pairingErrorMessage.empty() &&
        (!InitializeJumpgateProfileRuntime() ||
         !m_jumpgateProfileRuntime->StorePairingResponse(
             redemption, pairingRedemptionOrigin, true, now, storeError)))
    {
      pairingErrorMessage = storeError.empty() ? "Pairing credential commit failed" : storeError;
    }
    redemption = CVariant{};

    if (pairingErrorMessage.empty())
    {
      if (pairingProfileName.empty())
      {
        KODI::JUMPGATE::ActiveProfile active = m_jumpgateProfileRuntime->GetActive();
        pairingProfileName = active.name.empty() ? "Jumpgate Profile" : active.name;
        active.ClearSecrets();
      }
      ApplyActiveJumpgateProfile();
      const std::string toast = "Paired and applied (" + pairingProfileName + ")";

      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Info, "Jumpgate", toast, 5000, true);
    }
  }
  if (!pairingErrorMessage.empty())
  {
    CGUIDialogKaiToast::QueueNotification(
        CGUIDialogKaiToast::Error, "Jumpgate", pairingErrorMessage, 5000, true);
  }

  // Track playback position for external player mode result
  // Track during both PLAYING and PAUSED states (video/audio flag stays set when paused)
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && (m_playback_state & (PLAYBACK_STATE_VIDEO | PLAYBACK_STATE_AUDIO)))
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

  // Trakt scrobbler: poll for device code auth
  if (m_traktScrobbler && m_externalPlayerMode.load(std::memory_order_relaxed))
    m_traktScrobbler->ProcessSlow();

  // Update Jumpgate overlay content info as soon as identification/hydration becomes available.
  // Safe even if the overlay isn't currently shown (Java side will no-op).
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && !m_overlayHidden && m_traktScrobbler &&
      m_traktScrobbler->IsContentIdentified())
  {
    UpdateLoadingOverlayContentInfo(false);
  }

  // Content-ID based late resume: when content is identified after playback starts
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler && !m_resumeApplied.load(std::memory_order_relaxed))
    OnContentIdentified();

  // Settings dialog (triggered by Menu key from input thread)
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_settingsRequested.exchange(false))
    ShowSettingsDialog();

  // One-time update check
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && !m_updateChecked && GetSettingBool("auto_update_check", true))
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
  CLog::LogF(LOGDEBUG, "package: {}", package);
  CLog::LogF(LOGDEBUG, "intent: {}", intent);
  CLog::LogF(LOGDEBUG, "dataType: {}", dataType);
  CLog::LogF(LOGDEBUG, "dataURI: {}", dataURI);
  CLog::LogF(LOGDEBUG, "flags: {}", flags);
  CLog::LogF(LOGDEBUG, "extras: {}", extras);
  CLog::LogF(LOGDEBUG, "action: {}", action);
  CLog::LogF(LOGDEBUG, "category: {}", category);
  CLog::LogF(LOGDEBUG, "className: {}", className);

  CJNIIntent newIntent = intent.empty() ?
    GetPackageManager().getLaunchIntentForPackage(package) :
    CJNIIntent(intent);

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
      jniURI = CJNIFileProvider::getUriForFile(CXBMCApp::Get(), "org.xbmc.kodi.fileprovider",
                                               CJNIFile(pathname));

      CLog::LogF(LOGINFO, "Share using FileProvider: {}", jniURI.toString());

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
    catch (const std::exception& e)
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
        CLog::LogF(LOGDEBUG, "Putting extra key: {}, value: {}", e["key"].asString(),
                   e["value"].asString());
      }
      else
        CLog::LogF(LOGDEBUG, "Intent extras data type ({}) not implemented", e["type"].asString());
    }
  }

  newIntent.setPackage(package);
  if (!className.empty())
    newIntent.setClassName(package, className);

  startActivity(newIntent);
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::LogF(LOGERROR, "ExceptionOccurred launching {}", package);
    xbmc_jnienv()->ExceptionDescribe();
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

int CXBMCApp::GetMaxSystemVolume(JNIEnv *env)
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
      newstate = (intent.getIntExtra("android.bluetooth.profile.extra.STATE", 0) == 2 /* STATE_CONNECTED */);

    if (newstate != m_headsetPlugged)
    {
      m_headsetPlugged = newstate;
      IAE *iae = CServiceBroker::GetActiveAE();
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

// --- Static helper for rapid content switching (F-007) ---
// Takes all parameters by value so it can be called from a detached thread
// with zero references to any object instance.
static void SaveResumeForContent(const std::string& imdbId, int season, int episode,
                                 int64_t posMs, int64_t durMs,
                                 const std::string& bridgeUrl)
{
  if (imdbId.empty() || posMs <= 0)
    return;

  std::string key = imdbId;
  if (season >= 0 && episode >= 0)
    key += ":" + std::to_string(season) + ":" + std::to_string(episode);

  bool completed = (durMs > 0 && posMs > 0 &&
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
    CLog::Log(LOGINFO, "SaveResumeForContent: Cleared {} (completed)", key);
  }
  else
  {
    CVariant entry(CVariant::VariantTypeObject);
    entry["position"] = posMs;
    entry["duration"] = durMs;
    entry["timestamp"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    store[key] = entry;
    CLog::Log(LOGINFO, "SaveResumeForContent: Saved {} - pos={} dur={}", key, posMs, durMs);
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

  // POST to Bridge /resume (best-effort)
  if (!bridgeUrl.empty())
  {
    XFILE::CCurlFile curl;
    curl.SetRequestHeader("Content-Type", "application/json");
    curl.SetTimeout(2);

    CVariant body(CVariant::VariantTypeObject);
    body["imdb"] = imdbId;
    body["season"] = (season >= 0) ? std::to_string(season) : "";
    body["episode"] = (episode >= 0) ? std::to_string(episode) : "";
    body["position"] = posMs;
    body["duration"] = durMs;

    std::string bodyJson;
    CJSONVariantWriter::Write(body, bodyJson, true);
    std::string response;
    curl.Post(bridgeUrl + "/resume", bodyJson, response);
    // Ignore errors -- Bridge may not be running
  }
}

void CXBMCApp::onNewIntent(CJNIIntent intent)
{
  if (!intent)
  {
    CLog::Log(LOGINFO, "CXBMCApp::onNewIntent - Got invalid intent.");
    return;
  }

  std::string action = intent.getAction();
  CLog::Log(LOGDEBUG, "CXBMCApp::onNewIntent - Got intent. Action: {}", action);
  std::string targetFile = GetFilenameFromIntent(intent);

  // Parse and strip Jumpgate Bridge metadata from URL (_mk_imdb, _mk_type, _mk_s, _mk_e)
  // These are appended by the Jumpgate Bridge Stremio addon for content identification.
  std::string mkImdbId;
  int mkSeason = -1;
  int mkEpisode = -1;
  if (!targetFile.empty())
  {
    auto extractParam = [](const std::string& url, const std::string& key) -> std::string {
      std::string search = key + "=";
      auto pos = url.find(search);
      if (pos == std::string::npos)
        return "";
      auto start = pos + search.size();
      auto end = url.find('&', start);
      return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    mkImdbId = extractParam(targetFile, "_mk_imdb");
    std::string mkS = extractParam(targetFile, "_mk_s");
    std::string mkE = extractParam(targetFile, "_mk_e");
    if (!mkS.empty()) mkSeason = std::atoi(mkS.c_str());
    if (!mkE.empty()) mkEpisode = std::atoi(mkE.c_str());

    if (!mkImdbId.empty())
    {
      CLog::Log(LOGINFO, "CXBMCApp: Jumpgate Bridge metadata - imdb={} S{}E{}", mkImdbId, mkSeason, mkEpisode);
      // Strip all _mk_* params from URL before passing to the player
      std::string cleanUrl = targetFile;
      // Remove _mk_* params (handles both ?_mk_ and &_mk_ cases)
      while (true)
      {
        auto pos = cleanUrl.find("_mk_");
        if (pos == std::string::npos)
          break;
        // Find the separator before this param (? or &)
        auto sepPos = (pos > 0 && (cleanUrl[pos - 1] == '?' || cleanUrl[pos - 1] == '&'))
                          ? pos - 1 : pos;
        // Find the end of this param
        auto endPos = cleanUrl.find('&', pos);
        if (endPos == std::string::npos)
          cleanUrl.erase(sepPos);
        else
          cleanUrl.erase(sepPos, endPos - sepPos);
      }
      // Clean up trailing ? if all params were removed
      if (!cleanUrl.empty() && cleanUrl.back() == '?')
        cleanUrl.pop_back();
      targetFile = cleanUrl;
    }
  }

  // Detect external player mode: ACTION_VIEW with a media file
  // (ACTION_GET_CONTENT is Kodi's internal leanback navigation, not external player)
  if (!targetFile.empty() && action == CJNIIntent::ACTION_VIEW)
  {
    // Only mark wasStandalone if we were NOT already in external player mode.
    // On cold launch from Stremio, m_externalPlayerMode is already true (set in onStart
    // via Java's isExternalPlayerMode()). On a genuine warm transition from standalone
    // Kodi, m_externalPlayerMode is false (reset by ReturnToStandaloneMode).
    bool wasColdLaunch = m_externalPlayerMode.load(std::memory_order_relaxed);
    if (!wasColdLaunch)
    {
      m_wasStandalone.store(true, std::memory_order_relaxed);
      CLog::Log(LOGINFO, "CXBMCApp: External player mode activated (warm transition) for: {}", targetFile);
    }
    else
    {
      CLog::Log(LOGINFO, "CXBMCApp: External player mode activated (cold launch) for: {}", targetFile);
    }
    m_externalPlayerMode.store(true, std::memory_order_relaxed);

    // Create TraktScrobbler lazily if not yet initialized
    if (!m_traktScrobbler)
    {
      m_traktScrobbler = std::make_unique<TraktScrobbler>();
      ApplyActiveJumpgateProfile();
      m_traktScrobbler->Initialize();
    }

    // Extract content ID extras forwarded from Splash
    std::string imdbId;
    std::string title;
    int year = 0;
    int season = -1;
    int episode = -1;

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

    // Jumpgate Bridge metadata takes priority over intent extras
    if (!mkImdbId.empty())
    {
      imdbId = mkImdbId;
      if (mkSeason >= 0) season = mkSeason;
      if (mkEpisode >= 0) episode = mkEpisode;
    }

    // --- Rapid content switching: stop old scrobble + save resume (F-007) ---
    // Before clearing content for the new intent, capture the old content's state
    // and fire-and-forget a ScrobbleStop + resume save on a detached thread.
    if (m_traktScrobbler)
    {
      // Capture ALL state by value for the detached thread (zero references to this)
      std::string oldImdbId = m_traktScrobbler->GetImdbId();
      std::string oldTraktSlug = m_traktScrobbler->GetTraktSlug();
      std::string oldTitle = m_traktScrobbler->GetTitle();
      int oldYear = m_traktScrobbler->GetYear();
      int oldSeason = m_traktScrobbler->GetSeason();
      int oldEpisode = m_traktScrobbler->GetEpisode();
      bool wasScrobbling = m_traktScrobbler->IsScrobbleActive();
      std::string accessToken = m_traktScrobbler->GetAccessToken();
      std::string bridgeUrl = m_traktScrobbler->GetBridgeUrl();
      int64_t oldPosMs = m_lastPlaybackTimeMs.load(std::memory_order_relaxed);
      int64_t oldDurMs = m_lastPlaybackDurationMs.load(std::memory_order_relaxed);

      // Calculate progress for ScrobbleStop
      float oldProgress = (oldDurMs > 0)
          ? static_cast<float>(oldPosMs) / static_cast<float>(oldDurMs) * 100.0f
          : 0.0f;

      // Always attempt ScrobbleStop + resume save, even if content was never identified
      // (per user decision: no-op on Trakt is acceptable, avoids edge cases)
      if (oldPosMs > 0 || wasScrobbling)
      {
        CLog::Log(LOGINFO, "CXBMCApp: Rapid switch - saving old content state (imdb={}, pos={}, scrobbling={})",
                  oldImdbId, oldPosMs, wasScrobbling);

        // Fire-and-forget: detached thread captures everything by value
        std::thread([oldImdbId, oldTraktSlug, oldTitle, oldYear, oldSeason, oldEpisode,
                     wasScrobbling, accessToken, bridgeUrl, oldPosMs, oldDurMs, oldProgress]() {
          // Save resume position for old content (local file + Bridge POST)
          SaveResumeForContent(oldImdbId, oldSeason, oldEpisode, oldPosMs, oldDurMs, bridgeUrl);

          // Send ScrobbleStop to Trakt if we were scrobbling and have content info
          if (wasScrobbling && !accessToken.empty() && (!oldImdbId.empty() || !oldTraktSlug.empty()))
          {
            // Build scrobble JSON manually (can't use member method from detached thread)
            CVariant root(CVariant::VariantTypeObject);
            bool isEpisode = (oldSeason >= 0 && oldEpisode >= 0);

            if (isEpisode)
            {
              CVariant show(CVariant::VariantTypeObject);
              CVariant ids(CVariant::VariantTypeObject);
              if (!oldImdbId.empty()) ids["imdb"] = oldImdbId;
              if (!oldTraktSlug.empty()) ids["slug"] = oldTraktSlug;
              show["ids"] = ids;
              if (!oldTitle.empty()) show["title"] = oldTitle;
              if (oldYear > 0) show["year"] = oldYear;

              CVariant ep(CVariant::VariantTypeObject);
              ep["season"] = oldSeason;
              ep["number"] = oldEpisode;

              root["show"] = show;
              root["episode"] = ep;
            }
            else
            {
              CVariant movie(CVariant::VariantTypeObject);
              CVariant ids(CVariant::VariantTypeObject);
              if (!oldImdbId.empty()) ids["imdb"] = oldImdbId;
              if (!oldTraktSlug.empty()) ids["slug"] = oldTraktSlug;
              movie["ids"] = ids;
              if (!oldTitle.empty()) movie["title"] = oldTitle;
              if (oldYear > 0) movie["year"] = oldYear;
              root["movie"] = movie;
            }

            root["progress"] = static_cast<double>(oldProgress);

            std::string json;
            if (CJSONVariantWriter::Write(root, json, true))
            {
              XFILE::CCurlFile curl;
              curl.SetRequestHeader("Content-Type", "application/json");
              curl.SetRequestHeader("trakt-api-version", "2");
              curl.SetRequestHeader("trakt-api-key", "d4161a7a106424551add171e5470112e4afdaf2438e6ef2fe0548edc75924868");
              curl.SetRequestHeader("Authorization", "Bearer " + accessToken);
              curl.SetTimeout(5);

              std::string url = std::string("https://api.trakt.tv") + "/scrobble/stop";
              std::string response;
              if (curl.Post(url, json, response))
                CLog::Log(LOGINFO, "CXBMCApp: Rapid switch - ScrobbleStop sent for old content");
              else
                CLog::Log(LOGWARNING, "CXBMCApp: Rapid switch - ScrobbleStop failed (accepting loss)");
            }
          }
        }).detach();
      }

      // Reset playback position/duration for new content
      m_lastPlaybackTimeMs.store(0, std::memory_order_relaxed);
      m_lastPlaybackDurationMs.store(0, std::memory_order_relaxed);
    }
    // --- End rapid content switching ---

    // Pass content info to TraktScrobbler
    if (m_traktScrobbler)
    {
      m_traktScrobbler->ClearContentInfo();
      m_traktScrobbler->SetContentInfo(imdbId, title, year, season, episode);
      m_traktScrobbler->SetMediaUrl(targetFile);
    }

    // Pass content info to SubtitleDownloader
    if (!m_subtitleDownloader)
    {
      m_subtitleDownloader = std::make_unique<SubtitleDownloader>();
      m_subtitleDownloader->Initialize();
      m_subtitleDownloader->SetLanguages(GetSettingString("subtitle_languages", "en"));
    }
    if (m_subtitleDownloader)
    {
      m_subtitleDownloader->ClearContentInfo();
      m_subtitleDownloader->SetContentInfo(imdbId, title, year, season, episode);
    }

    // Read resume position from caller intent (ms)
    // VLC uses "extra_position" (long), MX Player/mpv use "position" (int)
    // Stremio uses "startfrom" (milliseconds)
    int resumePositionMs = 0;
    if (intent.hasExtra("position"))
      resumePositionMs = intent.getIntExtra("position", 0);
    else if (intent.hasExtra("extra_position"))
      resumePositionMs = intent.getIntExtra("extra_position", 0);

    // Also check Stremio's "startfrom" extra
    if (resumePositionMs <= 0 && intent.hasExtra("startfrom"))
    {
      int startFrom = intent.getIntExtra("startfrom", 0);
      if (startFrom > 0)
      {
        // Auto-detect seconds vs milliseconds: if value < 10000 treat as seconds
        resumePositionMs = (startFrom < 10000) ? startFrom * 1000 : startFrom;
        CLog::Log(LOGINFO, "CXBMCApp: startfrom={} interpreted as {} ms", startFrom, resumePositionMs);
      }
    }

    if (resumePositionMs > 0)
      CLog::Log(LOGINFO, "CXBMCApp: Resume position from intent: {} ms", resumePositionMs);

    // If we have IMDB but no intent-provided resume position, check local resume store
    if (resumePositionMs <= 0 && !mkImdbId.empty())
    {
      resumePositionMs = LoadResumePosition(mkImdbId, mkSeason, mkEpisode);
      if (resumePositionMs > 0)
        CLog::Log(LOGINFO, "CXBMCApp: Resume from local store: {} ms", resumePositionMs);
    }

    m_resumePositionMs.store(resumePositionMs, std::memory_order_relaxed);
    m_resumeApplied.store((resumePositionMs > 0), std::memory_order_relaxed); // Mark applied if we got it from intent/store
  }

  if (!targetFile.empty() &&
      (action == CJNIIntent::ACTION_VIEW || action == CJNIIntent::ACTION_GET_CONTENT))
  {
    CLog::Log(LOGDEBUG, "-- targetFile: {}", targetFile);

    CURL targeturl(targetFile);
    std::string value;
    if (action == CJNIIntent::ACTION_GET_CONTENT ||
        (targeturl.GetOption("showinfo", value) && value == "true"))
    {
      if (targeturl.IsProtocol("videodb")
          || (targeturl.IsProtocol("special") && targetFile.find("playlists/video") != std::string::npos)
          || (targeturl.IsProtocol("special") && targetFile.find("playlists/mixed") != std::string::npos)
          )
      {
        std::vector<std::string> params;
        params.push_back(targeturl.Get());
        params.emplace_back("return");
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_GUI_ACTIVATE_WINDOW, WINDOW_VIDEO_NAV, 0,
                                                   nullptr, "", params);
      }
      else if (targeturl.IsProtocol("musicdb")
               || (targeturl.IsProtocol("special") && targetFile.find("playlists/music") != std::string::npos))
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
      CFileItem* item = new CFileItem(targetFile, false);
      if (IsVideoDb(*item))
      {
        *(item->GetVideoInfoTag()) = XFILE::CVideoDatabaseFile::GetVideoTag(item->GetURL());
        item->SetPath(item->GetVideoInfoTag()->m_strFileNameAndPath);
      }
      // Set resume position if provided by caller (in external player mode)
      int resumeMs = m_resumePositionMs.load(std::memory_order_relaxed);
      if (m_externalPlayerMode.load(std::memory_order_relaxed) && resumeMs > 0)
      {
        item->SetStartOffset(static_cast<int64_t>(resumeMs));
        CLog::Log(LOGINFO, "CXBMCApp: Setting start offset to {} ms for resume", resumeMs);
      }
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_PLAY, 0, 0, static_cast<void*>(item));
    }
  }
  else if (action == ACTION_XBMC_RESUME)
  {
    if (m_playback_state != PLAYBACK_STATE_STOPPED)
    {
      if (m_playback_state & PLAYBACK_STATE_VIDEO)
        RequestVisibleBehind(true);
      if (!(m_playback_state & PLAYBACK_STATE_PLAYING))
        CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
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

void CXBMCApp::onOpenSettingsRequested()
{
  if (!m_externalPlayerMode.load(std::memory_order_relaxed))
    return;

  m_settingsRequested.store(true, std::memory_order_relaxed);
  CLog::Log(LOGINFO, "CXBMCApp: Settings requested from Java long-press Back");
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

std::string CXBMCApp::GetFilenameFromIntent(const CJNIIntent &intent)
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
      CJNICursor cursor = getContentResolver().query(data, filePathColumn, std::string(), std::vector<std::string>(), std::string());
      if(cursor.moveToFirst())
      {
        int columnIndex = cursor.getColumnIndex(filePathColumn[0]);
        ret = cursor.getString(columnIndex);
      }
      cursor.close();
    }
    else if(scheme == "file")
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
  // Intercept Menu/Guide keys in external player mode to show settings dialog.
  // Phone long-press Back path is handled in Java Main.onKeyUp() and forwarded via JNI.
  if (m_externalPlayerMode.load(std::memory_order_relaxed) && AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
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
  CWinSystemAndroid* winSystemAndroid = dynamic_cast<CWinSystemAndroid*>(CServiceBroker::GetWinSystem());
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
