/*
 *  Copyright (C) 2015-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/JumpgateBackCoordinator.h"

#include <androidjni/Activity.h>
#include <androidjni/InputManager.h>
#include <androidjni/Rect.h>

#include <memory>
#include <string>
#include <utility>

namespace jni
{

class CJNIMainActivity : public CJNIActivity, public CJNIInputManagerInputDeviceListener
{
public:
  explicit CJNIMainActivity(const ANativeActivity *nativeActivity);
  ~CJNIMainActivity() override;

  using BackLifecycleToken = KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken;
  using BackLifecycleOperation = KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleOperation;
  using AppInstancePublicationToken =
      KODI::JUMPGATE::CJumpgateLifecycleTargetRegistry<CJNIMainActivity>::PublicationToken;

  class AppInstanceOperation final
  {
  public:
    AppInstanceOperation() = default;
    AppInstanceOperation(AppInstanceOperation&&) noexcept = default;
    AppInstanceOperation& operator=(AppInstanceOperation&& other) noexcept
    {
      if (this != &other)
      {
        m_appInstance.reset();
        m_lifecycleOperation = std::move(other.m_lifecycleOperation);
        m_appInstance = std::move(other.m_appInstance);
      }
      return *this;
    }

    AppInstanceOperation(const AppInstanceOperation&) = delete;
    AppInstanceOperation& operator=(const AppInstanceOperation&) = delete;

    explicit operator bool() const { return m_lifecycleOperation && m_appInstance != nullptr; }
    CJNIMainActivity* get() const { return m_appInstance.get(); }
    CJNIMainActivity* operator->() const { return m_appInstance.get(); }

  private:
    friend class CJNIMainActivity;

    AppInstanceOperation(BackLifecycleOperation lifecycleOperation,
                         std::shared_ptr<CJNIMainActivity> appInstance)
      : m_lifecycleOperation(std::move(lifecycleOperation)),
        m_appInstance(std::move(appInstance))
    {
    }

    BackLifecycleOperation m_lifecycleOperation;
    std::shared_ptr<CJNIMainActivity> m_appInstance;
  };

  static AppInstancePublicationToken PublishAppInstance(
      BackLifecycleToken lifecycleToken, std::shared_ptr<CJNIMainActivity> appInstance);
  static AppInstanceOperation AcquireAppInstance(BackLifecycleToken lifecycleToken);
  static bool RetireAppInstance(BackLifecycleToken lifecycleToken,
                                AppInstancePublicationToken publicationToken,
                                const CJNIMainActivity* expectedInstance);
  static bool RetireAppLifecycle(BackLifecycleToken lifecycleToken);
  static KODI::JUMPGATE::CJumpgateBackDispatcher& GetJumpgateBackDispatcher();

  static void RegisterNatives(JNIEnv* env);

  static void _onNewIntent(JNIEnv* env,
                           jobject context,
                           jlong lifecycleToken,
                           jobject intent,
                           jstring preparedRequestId);
  static void _onActivityResult(JNIEnv* env,
                                jobject context,
                                jlong lifecycleToken,
                                jint requestCode,
                                jint resultCode,
                                jobject resultData);
  static jlong _onBackCreated(JNIEnv* env, jobject context, jboolean initialExternalMode);
  static void _onBackStarted(JNIEnv* env, jobject context, jlong lifecycleToken, jint source);
  static void _onBackLongPress(JNIEnv* env, jobject context, jlong lifecycleToken);
  static void _onBackCancelled(JNIEnv* env, jobject context, jlong lifecycleToken);
  static void _onBackInvoked(JNIEnv* env, jobject context, jlong lifecycleToken);
  static void _onBackDestroyed(JNIEnv* env, jobject context, jlong lifecycleToken);
  static void _onVolumeChanged(JNIEnv* env, jobject context, jlong lifecycleToken, jint volume);
  static void _doFrame(JNIEnv* env,
                       jobject context,
                       jlong lifecycleToken,
                       jlong frameTimeNanos);
  static void _onInputDeviceAdded(JNIEnv* env,
                                  jobject context,
                                  jlong lifecycleToken,
                                  jint deviceId);
  static void _onInputDeviceChanged(JNIEnv* env,
                                    jobject context,
                                    jlong lifecycleToken,
                                    jint deviceId);
  static void _onInputDeviceRemoved(JNIEnv* env,
                                    jobject context,
                                    jlong lifecycleToken,
                                    jint deviceId);
  static void _onVisibleBehindCanceled(JNIEnv* env, jobject context, jlong lifecycleToken);

  static void _callNative(JNIEnv *env, jobject context, jlong funcAddr, jlong variantAddr);
  static void runNativeOnUiThread(void (*callback)(void*), void* variant);

  CJNIRect getDisplayRect();
  KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken GetJumpgateBackLifecycleToken(
      const ANativeActivity* nativeActivity);

protected:
  virtual void onNewIntent(CJNIIntent intent, std::string preparedRequestId) = 0;
  virtual void onBackLifecycleRetiring(BackLifecycleToken lifecycleToken) = 0;
  virtual void onActivityResult(int requestCode, int resultCode, CJNIIntent resultData)=0;
  virtual void onVolumeChanged(int volume)=0;
  virtual void doFrame(int64_t frameTimeNanos)=0;
  void onVisibleBehindCanceled() override = 0;

  virtual void onDisplayAdded(int displayId)=0;
  virtual void onDisplayChanged(int displayId)=0;
  virtual void onDisplayRemoved(int displayId)=0;
};

} // namespace jni
