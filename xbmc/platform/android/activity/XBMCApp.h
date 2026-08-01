/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IActivityHandler.h"
#include "IInputHandler.h"
#include "JNIMainActivity.h"
#include "JNIXBMCAudioManagerOnAudioFocusChangeListener.h"
#include "JNIXBMCDisplayManagerDisplayListener.h"
#include "JNIXBMCMainView.h"
#include "JNIXBMCMediaSession.h"
#include "interfaces/IAnnouncer.h"
#include "platform/xbmc.h"
#include "threads/Event.h"
#include "utils/Geometry.h"
#include "utils/JumpgateBackCoordinator.h"
#include "utils/JumpgatePlaybackAuthority.h"
#include "utils/JumpgatePlaybackHistory.h"
#include "utils/JumpgatePlaybackHistoryState.h"
#include "utils/JumpgatePlaybackResultState.h"
#include "utils/JumpgateShutdownCoordinator.h"
#include "utils/Variant.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <android/native_activity.h>
#include <androidjni/Activity.h>
#include <androidjni/AudioManager.h>
#include <androidjni/BroadcastReceiver.h>
#include <androidjni/SurfaceHolder.h>
#include <androidjni/View.h>

// forward declares
class CAESinkAUDIOTRACK;
class CVariant;
class CFileItem;
class IInputDeviceCallbacks;
class IInputDeviceEventHandler;
class CVideoSyncAndroid;
class TraktScrobbler;
class SubtitleDownloader;

namespace KODI::JUMPGATE
{
class CAndroidJumpgateCredentialStore;
class CAndroidJumpgateSubtitleController;
class CJumpgateProfileRuntime;
class CJumpgateProfileStorage;
class CJumpgatePlaybackClaimCoordinator;
class CJumpgatePairingCoordinator;
} // namespace KODI::JUMPGATE

namespace KODI::MESSAGING
{
class COwnedThreadMessagePayload;
}

typedef struct _JNIEnv JNIEnv;

struct androidIcon
{
  unsigned int width;
  unsigned int height;
  void* pixels;
};

struct androidPackage
{
  std::string packageName;
  std::string packageLabel;
  int icon;
};

class CNativeWindow
{
  friend class CWinSystemAndroidGLESContext; // meh

public:
  static std::shared_ptr<CNativeWindow> CreateFromSurface(CJNISurfaceHolder holder);
  ~CNativeWindow();

  bool SetBuffersGeometry(int width, int height, int format);
  int32_t GetWidth() const;
  int32_t GetHeight() const;

private:
  explicit CNativeWindow(ANativeWindow* window);

  CNativeWindow() = delete;
  CNativeWindow(const CNativeWindow&) = delete;
  CNativeWindow& operator=(const CNativeWindow&) = delete;

  ANativeWindow* m_window{nullptr};
};

class CXBMCApp : public IActivityHandler,
                 public jni::CJNIMainActivity,
                 public CJNIBroadcastReceiver,
                 public ANNOUNCEMENT::IAnnouncer,
                 public CJNISurfaceHolderCallback,
                 public std::enable_shared_from_this<CXBMCApp>,
                 private KODI::JUMPGATE::CJumpgateBackDispatcher::ISink
{
public:
  static CXBMCApp& Create(ANativeActivity* nativeActivity, IInputHandler& inputhandler)
  {
    if (m_appinstance)
      Destroy();

    auto app = std::shared_ptr<CXBMCApp>(new CXBMCApp(nativeActivity, inputhandler));
    auto& dispatcher = CJNIMainActivity::GetJumpgateBackDispatcher();
    auto sink = std::shared_ptr<KODI::JUMPGATE::CJumpgateBackDispatcher::ISink>(
        app, static_cast<KODI::JUMPGATE::CJumpgateBackDispatcher::ISink*>(app.get()));
    app->m_jumpgateBackPublicationToken =
        dispatcher.PublishSink(app->m_jumpgateBackLifecycleToken, std::move(sink));
    if (app->m_jumpgateBackPublicationToken ==
        KODI::JUMPGATE::CJumpgateBackDispatcher::INVALID_PUBLICATION_TOKEN)
      throw std::runtime_error("Unable to publish CXBMCApp Back sink");

    auto jniTarget = std::shared_ptr<CJNIMainActivity>(
        app, static_cast<CJNIMainActivity*>(app.get()));
    app->m_jumpgateAppPublicationToken = CJNIMainActivity::PublishAppInstance(
        app->m_jumpgateBackLifecycleToken, std::move(jniTarget));
    if (app->m_jumpgateAppPublicationToken == 0)
    {
      dispatcher.UnpublishSink(app->m_jumpgateBackLifecycleToken,
                               app->m_jumpgateBackPublicationToken);
      throw std::runtime_error("Unable to publish CXBMCApp JNI target");
    }

    m_appinstance = app;
    return *m_appinstance;
  }
  static CXBMCApp& Get() { return *m_appinstance; }
  static bool HasInstance() { return m_appinstance != nullptr; }
  static void Destroy()
  {
    if (m_appinstance)
    {
      CJNIMainActivity::RetireAppInstance(m_appinstance->m_jumpgateBackLifecycleToken,
                                          m_appinstance->m_jumpgateAppPublicationToken,
                                          m_appinstance.get());
      CJNIMainActivity::GetJumpgateBackDispatcher().UnpublishSink(
          m_appinstance->m_jumpgateBackLifecycleToken,
          m_appinstance->m_jumpgateBackPublicationToken);
    }
    m_appinstance.reset();
  }

  CXBMCApp() = delete;
  ~CXBMCApp() override;

  // IAnnouncer IF
  void Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                const std::string& sender,
                const std::string& message,
                const CVariant& data) override;

  void onReceive(CJNIIntent intent) override;
  void onNewIntent(CJNIIntent intent, std::string preparedRequestId) override;
  void onBackLifecycleRetiring(
      KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken) override;
  void onActivityResult(int requestCode, int resultCode, CJNIIntent resultData) override;
  void onVolumeChanged(int volume) override;
  virtual void onAudioFocusChange(int focusChange);
  void doFrame(int64_t frameTimeNanos) override;
  void onVisibleBehindCanceled() override;

  // implementation of CJNIInputManagerInputDeviceListener
  void onInputDeviceAdded(int deviceId) override;
  void onInputDeviceChanged(int deviceId) override;
  void onInputDeviceRemoved(int deviceId) override;

  // implementation of DisplayManager::DisplayListener
  void onDisplayAdded(int displayId) override;
  void onDisplayChanged(int displayId) override;
  void onDisplayRemoved(int displayId) override;
  jni::jhobject getDisplayListener() { return m_displayListener.get_raw(); }

  bool isValid() { return m_activity != NULL; }

  void onStart() override;
  void onResume() override;
  void onPause() override;
  void onStop() override;
  void onDestroy() override;

  void onSaveState(void** data, size_t* size) override;
  void onConfigurationChanged() override;
  void onLowMemory() override;

  void onCreateWindow(ANativeWindow* window) override;
  void onResizeWindow() override;
  void onDestroyWindow() override;
  void onGainFocus() override;
  void onLostFocus() override;

  void Initialize();
  void Deinitialize();

  bool Stop(int exitCode);
  void Quit();

  std::shared_ptr<CNativeWindow> GetNativeWindow(int timeout) const;

  bool SetBuffersGeometry(int width, int height, int format);
  static int android_printf(const char* format, ...);

  int GetBatteryLevel() const;
  void KeepScreenOn(bool on);
  bool HasFocus() const { return m_hasFocus; }

  static bool StartActivity(const std::string& package,
                            const std::string& intent = std::string(),
                            const std::string& dataType = std::string(),
                            const std::string& dataURI = std::string(),
                            const std::string& flags = std::string(),
                            const std::string& extras = std::string(),
                            const std::string& action = std::string(),
                            const std::string& category = std::string(),
                            const std::string& className = std::string());
  std::vector<androidPackage> GetApplications() const;

  static int GetMaxSystemVolume();
  static float GetSystemVolume();
  static void SetSystemVolume(float percent);

  void SetDisplayMode(int mode, float rate);
  int GetDPI() const;
  void SetVideoLayoutBackgroundColor(const int color);

  CRect MapRenderToDroid(const CRect& srcRect);

  // Playback callbacks
  void OnPlayBackStarted(bool resumed = false, uint64_t token = 0);
  void OnPlayBackPaused();
  void OnPlayBackStopped(bool completed = false);
  void CommitExternalPlaybackOpenFailure(uint64_t token);
  void CommitExternalPlaybackTerminal(bool completed, uint64_t token, bool started);
  void QueuePendingExternalPlayerResult();
  void DeliverPendingExternalPlayerResult();
  void NotifyExternalPlayerCleanupReady();
  std::optional<uint64_t> BeginExternalPlaybackContinuation();
  bool IsLatestExternalPlaybackAdmission(uint64_t token);

  // External player mode
  bool IsExternalPlayerMode() const { return m_externalPlayerMode.load(std::memory_order_relaxed); }
  void SetExternalPlayerMode(bool mode);
  void ReturnToStandaloneMode();

  bool onBackInputEvent(const AInputEvent* event) override;
  void SetJumpgateBackInputReady(bool ready);
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken GetJumpgateBackLifecycleToken() const
  {
    return m_jumpgateBackLifecycleToken;
  }

  // Version
  static constexpr const char* JUMPGATE_VERSION = "3.0.0";

  // Resume store (content-ID based cross-source resume)
  void SaveResumePosition(bool explicitEnd = false);
  int64_t LoadResumePosition(const std::string& imdbId, int season, int episode);
  void OnContentIdentified();

  // Jumpgate profile/settings runtime
  bool InitializeJumpgateProfileRuntime();
  void ApplyActiveJumpgateProfile();
  void HandleJumpgateManagerCommand(const std::string& command);
  void ShowJumpgateProfileManager();
  void ShowJumpgateProfilePicker(bool removeProfile);
  void ShowSettingsDialog();
  std::string GetSettingString(const std::string& key, const std::string& defaultVal = "") const;
  bool GetSettingBool(const std::string& key, bool defaultVal = true) const;
  void SetSetting(const std::string& key, const std::string& value);
  void SetSetting(const std::string& key, bool value);

  // Auto-update
  void CheckForUpdate();

  // Info callback
  void UpdateSessionMetadata();
  void UpdateSessionState();

  // input device methods
  void RegisterInputDeviceCallbacks(IInputDeviceCallbacks* handler);
  void UnregisterInputDeviceCallbacks();
  static const CJNIViewInputDevice GetInputDevice(int deviceId);
  static std::vector<int> GetInputDeviceIds();

  void RegisterInputDeviceEventHandler(IInputDeviceEventHandler* handler);
  void UnregisterInputDeviceEventHandler();
  bool onInputDeviceEvent(const AInputEvent* event);

  void InitFrameCallback(CVideoSyncAndroid* syncImpl);
  void DeinitFrameCallback();

  // Application slow ping
  void ProcessSlow();

  bool WaitVSync(unsigned int milliSeconds);
  int64_t GetNextFrameTime() const;
  float GetFrameLatencyMs() const;

  bool getVideosurfaceInUse();
  void setVideosurfaceInUse(bool videosurfaceInUse);

protected:
  // limit who can access Volume
  friend class CAESinkAUDIOTRACK;

  static int GetMaxSystemVolume(JNIEnv* env);
  bool AcquireAudioFocus();
  bool ReleaseAudioFocus();
  void RequestVisibleBehind(bool requested);

private:
  static std::shared_ptr<CXBMCApp> m_appinstance;

  CXBMCApp(ANativeActivity* nativeActivity, IInputHandler& inputhandler);

  jni::CJNIXBMCAudioManagerOnAudioFocusChangeListener m_audioFocusListener;
  jni::CJNIXBMCDisplayManagerDisplayListener m_displayListener;
  std::unique_ptr<jni::CJNIXBMCMainView> m_mainView;
  std::unique_ptr<jni::CJNIXBMCMediaSession> m_mediaSession;
  std::string GetFilenameFromIntent(const CJNIIntent& intent);

  void run();
  void stop();
  void SetupEnv();
  void StartBridgePairing();
  static std::string GetBridgeOriginFromUrl(const std::string& currentUrl);
  void StopBridgePairingWorker(bool clearPendingState, bool waitForCompletion = true);
  void QueuePairingRedemption(std::string responseJson,
                              const std::string& origin,
                              const std::string& profileName);
  void UpdateLoadingOverlayContentInfo(bool force);
  uint64_t QueuePlaybackSourceClaim(const std::string& rawLaunchUri, int64_t launchedAtMs);
  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Event> BeginExternalPlaybackAdmission(
      const std::string& rawLaunchUri,
      int64_t launchedAtMs,
      std::string resultRequestId,
      KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation);
  bool CommitExternalPlaybackAdmissionFailure(uint64_t token);
  void DeliverRejectedExternalPlaybackResult(
      std::string resultRequestId,
      KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation);
  void DeliverPendingExternalPlayerResult(
      KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation,
      bool deferPlayerCleanup = false);
  void ProcessPlaybackSourceClaim();
  void QueueJumpgateSubtitles(uint64_t generation);
  void ProcessJumpgateSubtitles();
  void StopJumpgateSubtitleController(bool playerMayRead, bool waitForCompletion = true);
  bool SavePairedPlaybackHistory(bool explicitEnd, uint64_t generation = 0);
  void LoadAndApplyPairedPlaybackResume(uint64_t generation, bool allowPlayerSeek = true);
  bool ReleasePlaybackSourceClaim(bool completed = false);
  void StopPlaybackClaimCoordinator(bool drainRelease);
  bool ExitExternalPlayerMode(
      const KODI::JUMPGATE::JumpgatePlaybackResult& result,
      KODI::JUMPGATE::CJumpgatePlaybackResultState::LifecycleOperation& lifecycleOperation,
      bool deferPlayerCleanup);
  std::optional<KODI::JUMPGATE::CJumpgatePlaybackAuthority::Token>
  BeginJumpgateProfileAuthorityTransition(std::string& error);
  void PrepareJumpgateProfileAuthorityTransition();
  bool FinishJumpgateProfileAuthorityTransition(
      KODI::JUMPGATE::CJumpgatePlaybackAuthority::Token token, bool committed);
  static void SetDisplayModeCallback(void* modeVariant);
  static void KeepScreenOnCallback(void* onVariant);
  static void SetViewBackgroundColorCallback(void* mapVariant);
  bool DispatchExternalBack(
      const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context) override;
  bool DispatchKodiBack(const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context,
                        bool longPress) override;
  bool OpenExternalSettings(
      const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context) override;
  enum class BackCommand
  {
    EXTERNAL_BACK,
    CANCEL_PENDING_PLAYBACK,
    KODI_SHORT_BACK,
    KODI_LONG_BACK,
    OPEN_SETTINGS,
  };
  bool DispatchBackCommand(const KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext& context,
                           BackCommand command);
  struct QueuedBackCommand;
  struct QueuedExternalPlayback;
  struct QueuedExternalPlayerResult;
  bool QueueBackCommand(BackCommand command,
                         uint64_t playbackGeneration = 0,
                         uint64_t playbackToken = 0,
                         std::optional<KODI::JUMPGATE::CJumpgateBackDispatcher::CommandContext>
                             context = std::nullopt);
  bool ExecuteQueuedBackCommand(QueuedBackCommand& command);
  void CancelQueuedBackCommand(const QueuedBackCommand& command) noexcept;
  bool QueueExternalPlayback(std::unique_ptr<CFileItem> item,
                              uint64_t admissionGeneration,
                              uint64_t admissionToken,
                              std::string resultRequestId);
  void ExecuteQueuedExternalPlayback(QueuedExternalPlayback& playback);
  void CancelQueuedExternalPlayback(const QueuedExternalPlayback& playback) noexcept;
  void CancelQueuedMediaPlayback(
      KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken lifecycleToken,
      uint64_t admissionGeneration,
      uint64_t admissionToken,
      const std::string& resultRequestId) noexcept;
  void CancelExternalPlaybackForLifecycleTeardown() noexcept;
  bool QueueExternalPlayerResult(uint64_t generation,
                                 std::string requestId,
                                 bool wasStandalone);
  void ExecuteQueuedExternalPlayerResult(const QueuedExternalPlayerResult& result);
  void CancelQueuedExternalPlayerResult(const QueuedExternalPlayerResult& result) noexcept;
  void PostExternalPlayerResultConvergence(uint64_t generation,
                                           const std::string& requestId,
                                           bool wasStandalone) noexcept;
  static void ConvergeExternalPlayerResultCallback(CVariant* payload);
  void ConvergeExternalPlayerResult(uint64_t generation,
                                    const std::string& requestId,
                                    bool wasStandalone);
  void HandoffWarmExternalPlayerTask(uint64_t generation, const std::string& requestId);
  void ExitExternalPlaybackForBack(uint64_t playbackToken);
  bool CancelPendingExternalPlaybackFromBack();
  bool ExecuteExternalBackCommand();
  bool ExecuteKodiBackCommand(bool longPress);
  bool ExecuteOpenExternalSettingsCommand();

  static void RegisterDisplayListenerCallback(void*);
  void UnregisterDisplayListener();

  ANativeActivity* m_activity{nullptr};
  IInputHandler& m_inputHandler;
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken m_jumpgateBackLifecycleToken{
      KODI::JUMPGATE::CJumpgateBackDispatcher::INVALID_LIFECYCLE_TOKEN};
  KODI::JUMPGATE::CJumpgateBackDispatcher::PublicationToken m_jumpgateBackPublicationToken{
      KODI::JUMPGATE::CJumpgateBackDispatcher::INVALID_PUBLICATION_TOKEN};
  CJNIMainActivity::AppInstancePublicationToken m_jumpgateAppPublicationToken{0};
  int m_batteryLevel{0};
  bool m_hasFocus{false};
  bool m_headsetPlugged{false};
  bool m_hdmiSource{false};
  bool m_wakeUp{false};
  bool m_aeReset{false};
  bool m_hdmiPlugged{true};
  bool m_mediaSessionUpdated{false};
  IInputDeviceCallbacks* m_inputDeviceCallbacks{nullptr};
  IInputDeviceEventHandler* m_inputDeviceEventHandler{nullptr};
  bool m_hasReqVisible{false};
  bool m_firstrun{true};
  std::atomic<bool> m_exiting{false};
  int m_exitCode{0};
  bool m_bResumePlayback{false};
  std::thread m_thread;
  mutable CCriticalSection m_applicationsMutex;
  mutable std::vector<androidPackage> m_applications;

  std::shared_ptr<CNativeWindow> m_window;

  CVideoSyncAndroid* m_syncImpl{nullptr};
  CEvent m_vsyncEvent;
  CEvent m_displayChangeEvent;

  bool XBMC_DestroyDisplay();
  bool XBMC_SetupDisplay();

  void OnSleep();
  void OnWakeup();

  uint32_t m_playback_state{0};
  int64_t m_frameTimeNanos{0};
  float m_refreshRate{0.0f};

  // External player mode state (atomic: written on JNI thread, read on Kodi Main thread)
  std::atomic<bool> m_externalPlayerMode{false};
  std::atomic<bool> m_wasStandalone{false}; // true when entered ext player mode from standalone
  std::atomic<int64_t> m_lastPlaybackTimeMs{0}; // F-003: atomic prevents torn reads on ARM32
  std::atomic<int64_t> m_lastPlaybackDurationMs{0}; // F-003: atomic prevents torn reads on ARM32
  mutable std::mutex m_externalProcessExitMutex;
  uint64_t m_externalProcessExitGeneration{0};
  std::string m_externalProcessExitRequestId;
  std::atomic<int64_t> m_resumePositionMs{0};
  std::atomic<bool> m_resumeApplied{false}; // Prevents double-seek from content-ID resume
  std::atomic<uint64_t> m_externalPlaybackStartedGeneration{0};
  std::atomic<int64_t> m_externalPlaybackStartedAtSteadyMs{0};

  // Resume store file
  static constexpr const char* RESUME_STORE_FILE = "special://profile/jumpgate_resume.json";

  // Profile metadata is plaintext but canonical and secret-free. Bearer/config
  // material is held by the Android Keystore-backed credential store.
  std::unique_ptr<KODI::JUMPGATE::CJumpgateProfileStorage> m_jumpgateProfileStorage;
  std::unique_ptr<KODI::JUMPGATE::CJumpgateProfileStorage> m_jumpgatePlaybackHistoryStorage;
  std::unique_ptr<KODI::JUMPGATE::CJumpgatePlaybackHistoryStore> m_jumpgatePlaybackHistoryStore;
  std::unique_ptr<KODI::JUMPGATE::CAndroidJumpgateCredentialStore> m_jumpgateCredentialStore;
  std::unique_ptr<KODI::JUMPGATE::CJumpgateProfileRuntime> m_jumpgateProfileRuntime;
  struct PendingPlaybackClaim
  {
    uint64_t generation{0};
    std::vector<std::string> fingerprints;
    std::string intentUrlHash;
    int64_t launchedAtMs{0};
  };
  mutable CCriticalSection m_playbackClaimMutex;
  std::unique_ptr<KODI::JUMPGATE::CJumpgatePlaybackClaimCoordinator> m_playbackClaimCoordinator;
  std::optional<PendingPlaybackClaim> m_pendingPlaybackClaim;
  uint64_t m_playbackClaimGeneration{0};
  uint64_t m_submittedPlaybackClaimGeneration{0};
  std::string m_submittedPlaybackClaimProfileId;
  std::string m_submittedPlaybackClaimDeviceId;
  std::string m_submittedPlaybackClaimOrigin;
  std::string m_activePlaybackClaimSessionId;
  std::string m_activePlaybackClaimProfileId;
  std::string m_activePlaybackClaimDeviceId;
  std::string m_activePlaybackClaimOrigin;
  KODI::JUMPGATE::CJumpgatePlaybackHistoryState m_playbackHistoryState;
  KODI::JUMPGATE::CJumpgatePlaybackAuthority m_playbackAuthority;
  std::atomic<uint64_t> m_ordinaryPlaybackAuthorityToken{0};
  KODI::JUMPGATE::CJumpgatePlaybackResultState m_playbackResultState;
  std::mutex m_externalPlaybackQueueMutex;
  uint64_t m_externalPlaybackDispatchGeneration{0};
  uint64_t m_externalPlaybackDispatchToken{0};
  std::string m_externalPlaybackDispatchRequestId;
  std::shared_ptr<KODI::MESSAGING::COwnedThreadMessagePayload>
      m_externalPlaybackDispatchPayload;
  uint64_t m_pendingExternalPlaybackStopGeneration{0};
  uint64_t m_pendingExternalPlaybackStopToken{0};
  std::atomic<uint64_t> m_rejectedPlaybackResultGeneration{1};
  KODI::JUMPGATE::CJumpgateShutdownCoordinator m_shutdownCoordinator;
  std::atomic<bool> m_settingsRequested{false};
  mutable CCriticalSection m_pairingMutex;
  std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> m_pairingCoordinator;
  bool m_pairingRedemptionPending{false};
  std::string m_pairingRedemptionJson;
  std::string m_pairingRedemptionOrigin;
  std::string m_pairingApplyProfileName;
  bool m_updateChecked{false};
  bool m_overlayHidden{false};
  std::string m_lastOverlayTitle;
  std::string m_lastOverlayMeta;
  std::string m_lastOverlayLogoUrl;

  // Stable for the CXBMCApp lifetime; initialized only in external-player mode.
  std::unique_ptr<TraktScrobbler> m_traktScrobbler;

  // Subtitle downloader (external player mode only)
  std::unique_ptr<SubtitleDownloader> m_subtitleDownloader;
  std::unique_ptr<KODI::JUMPGATE::CAndroidJumpgateSubtitleController> m_jumpgateSubtitleController;

public:
  // CJNISurfaceHolderCallback interface
  void surfaceChanged(CJNISurfaceHolder holder, int format, int width, int height) override;
  void surfaceCreated(CJNISurfaceHolder holder) override;
  void surfaceDestroyed(CJNISurfaceHolder holder) override;
};
