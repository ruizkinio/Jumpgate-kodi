/*
 *  Copyright (C) 2015-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JNIMainActivity.h"

#include "CompileInfo.h"

#include <mutex>

#include <android/native_activity.h>
#include <androidjni/Activity.h>
#include <androidjni/Intent.h>
#include <androidjni/jutils-details.hpp>

using namespace jni;

namespace
{
using BackDispatcher = KODI::JUMPGATE::CJumpgateBackDispatcher;
using LifecycleToken = BackDispatcher::LifecycleToken;
using AppTargetRegistry = KODI::JUMPGATE::CJumpgateLifecycleTargetRegistry<CJNIMainActivity>;

AppTargetRegistry& GetAppTargetRegistry()
{
  // JNI callbacks can outlive an Activity destructor. Exact publication tokens
  // keep a stale destructor from clearing or reacquiring its replacement.
  static auto* registry = new AppTargetRegistry;
  return *registry;
}

class CBackActivityBinding
{
public:
  LifecycleToken OnCreated(JNIEnv* env, jobject context, bool initialExternalMode)
  {
    if (context == nullptr)
      return BackDispatcher::INVALID_LIFECYCLE_TOKEN;

    jweak activity = env->NewWeakGlobalRef(context);
    if (activity == nullptr)
      return BackDispatcher::INVALID_LIFECYCLE_TOKEN;

    const LifecycleToken token =
        CJNIMainActivity::GetJumpgateBackDispatcher().OnLifecycleStarted(initialExternalMode);
    jweak previousActivity{nullptr};
    {
      std::unique_lock lock(m_mutex);
      previousActivity = m_activity;
      m_activity = activity;
      m_token = token;
    }

    if (previousActivity != nullptr)
      env->DeleteWeakGlobalRef(previousActivity);
    return token;
  }

  bool IsCurrent(JNIEnv* env, jobject context, LifecycleToken token)
  {
    if (context == nullptr || token == BackDispatcher::INVALID_LIFECYCLE_TOKEN)
      return false;

    std::unique_lock lock(m_mutex);
    return token == m_token && m_activity != nullptr &&
           env->IsSameObject(context, m_activity) == JNI_TRUE;
  }

  void OnDestroyed(JNIEnv* env, jobject context, LifecycleToken token)
  {
    jweak activity{nullptr};
    {
      std::unique_lock lock(m_mutex);
      if (token == BackDispatcher::INVALID_LIFECYCLE_TOKEN || token != m_token ||
          m_activity == nullptr || env->IsSameObject(context, m_activity) != JNI_TRUE)
      {
        return;
      }

      activity = m_activity;
      m_activity = nullptr;
      m_token = BackDispatcher::INVALID_LIFECYCLE_TOKEN;
    }

    CJNIMainActivity::GetJumpgateBackDispatcher().OnLifecycleDestroyed(token);
    env->DeleteWeakGlobalRef(activity);
  }

private:
  std::mutex m_mutex;
  jweak m_activity{nullptr};
  LifecycleToken m_token{BackDispatcher::INVALID_LIFECYCLE_TOKEN};
};

CBackActivityBinding& GetBackActivityBinding()
{
  static auto* binding = new CBackActivityBinding;
  return *binding;
}

LifecycleToken ToLifecycleToken(jlong lifecycleToken)
{
  return static_cast<LifecycleToken>(lifecycleToken);
}
} // namespace

KODI::JUMPGATE::CJumpgateBackDispatcher& CJNIMainActivity::GetJumpgateBackDispatcher()
{
  // The broker spans Activity recreation and must remain valid through JNI/static teardown.
  static auto* dispatcher = new KODI::JUMPGATE::CJumpgateBackDispatcher;
  return *dispatcher;
}

CJNIMainActivity::AppInstancePublicationToken CJNIMainActivity::PublishAppInstance(
    BackLifecycleToken lifecycleToken, std::shared_ptr<CJNIMainActivity> appInstance)
{
  return GetAppTargetRegistry().Publish(lifecycleToken, std::move(appInstance));
}

CJNIMainActivity::AppInstanceOperation CJNIMainActivity::AcquireAppInstance(
    BackLifecycleToken lifecycleToken)
{
  auto lifecycleOperation =
      GetJumpgateBackDispatcher().TryAcquireLifecycleOperation(lifecycleToken);
  if (!lifecycleOperation)
    return {};

  // Do not nest the dispatcher and registry mutexes. The lifecycle lease keeps
  // a replacement pending while the acquired shared target executes.
  auto appInstance = GetAppTargetRegistry().Acquire(lifecycleToken);
  if (!appInstance)
    return {};
  return AppInstanceOperation{std::move(lifecycleOperation), std::move(appInstance)};
}

bool CJNIMainActivity::RetireAppInstance(BackLifecycleToken lifecycleToken,
                                         AppInstancePublicationToken publicationToken,
                                         const CJNIMainActivity* expectedInstance)
{
  return GetAppTargetRegistry().Retire(lifecycleToken, publicationToken, expectedInstance);
}

bool CJNIMainActivity::RetireAppLifecycle(BackLifecycleToken lifecycleToken)
{
  return GetAppTargetRegistry().RetireLifecycle(lifecycleToken);
}

CJNIMainActivity::CJNIMainActivity(const ANativeActivity *nativeActivity)
  : CJNIActivity(nativeActivity)
{
}

CJNIMainActivity::~CJNIMainActivity() = default;

void CJNIMainActivity::RegisterNatives(JNIEnv* env)
{
  std::string pkgRoot = CCompileInfo::GetClass();

  const std::string mainClass = pkgRoot + "/Main";
  const std::string settingsObserver = pkgRoot + "/XBMCSettingsContentObserver";
  const std::string inputDeviceListener = pkgRoot + "/XBMCInputDeviceListener";

  jclass cMain = env->FindClass(mainClass.c_str());
  if (cMain)
  {
    JNINativeMethod methods[] = {
        {"_onNewIntent", "(JLandroid/content/Intent;Ljava/lang/String;)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onNewIntent)},
        {"_onActivityResult", "(JIILandroid/content/Intent;)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onActivityResult)},
        {"_onBackCreated", "(Z)J", reinterpret_cast<void*>(&CJNIMainActivity::_onBackCreated)},
        {"_onBackStarted", "(JI)V", reinterpret_cast<void*>(&CJNIMainActivity::_onBackStarted)},
        {"_onBackLongPress", "(J)V", reinterpret_cast<void*>(&CJNIMainActivity::_onBackLongPress)},
        {"_onBackCancelled", "(J)V", reinterpret_cast<void*>(&CJNIMainActivity::_onBackCancelled)},
        {"_onBackInvoked", "(J)V", reinterpret_cast<void*>(&CJNIMainActivity::_onBackInvoked)},
        {"_onBackDestroyed", "(J)V", reinterpret_cast<void*>(&CJNIMainActivity::_onBackDestroyed)},
        {"_doFrame", "(JJ)V", reinterpret_cast<void*>(&CJNIMainActivity::_doFrame)},
        {"_callNative", "(JJ)V", reinterpret_cast<void*>(&CJNIMainActivity::_callNative)},
        {"_onVisibleBehindCanceled", "(J)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onVisibleBehindCanceled)},
    };
    env->RegisterNatives(cMain, methods, sizeof(methods) / sizeof(methods[0]));
  }

  jclass cSettingsObserver = env->FindClass(settingsObserver.c_str());
  if (cSettingsObserver)
  {
    JNINativeMethod methods[] = {
        {"_onVolumeChanged", "(JI)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onVolumeChanged)},
    };
    env->RegisterNatives(cSettingsObserver, methods, sizeof(methods) / sizeof(methods[0]));
  }

  jclass cInputDeviceListener = env->FindClass(inputDeviceListener.c_str());
  if (cInputDeviceListener)
  {
    JNINativeMethod methods[] = {
        {"_onInputDeviceAdded", "(JI)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onInputDeviceAdded)},
        {"_onInputDeviceChanged", "(JI)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onInputDeviceChanged)},
        {"_onInputDeviceRemoved", "(JI)V",
         reinterpret_cast<void*>(&CJNIMainActivity::_onInputDeviceRemoved)}};
    env->RegisterNatives(cInputDeviceListener, methods, sizeof(methods) / sizeof(methods[0]));
  }
}

void CJNIMainActivity::_onNewIntent(JNIEnv* env,
                                    jobject context,
                                    jlong lifecycleToken,
                                    jobject intent,
                                    jstring preparedRequestId)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (!GetBackActivityBinding().IsCurrent(env, context, token))
    return;
  if (const auto appInstance = AcquireAppInstance(token))
  {
    std::string requestId;
    if (preparedRequestId != nullptr)
    {
      const char* requestChars = env->GetStringUTFChars(preparedRequestId, nullptr);
      if (requestChars != nullptr)
      {
        requestId.assign(requestChars);
        env->ReleaseStringUTFChars(preparedRequestId, requestChars);
      }
    }
    appInstance->onNewIntent(CJNIIntent(jhobject::fromJNI(intent)), std::move(requestId));
  }
}

void CJNIMainActivity::_onActivityResult(JNIEnv* env,
                                         jobject context,
                                         jlong lifecycleToken,
                                         jint requestCode,
                                         jint resultCode,
                                         jobject resultData)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (!GetBackActivityBinding().IsCurrent(env, context, token))
    return;
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onActivityResult(requestCode, resultCode,
                                  CJNIIntent(jhobject::fromJNI(resultData)));
}

jlong CJNIMainActivity::_onBackCreated(JNIEnv* env,
                                       jobject context,
                                       jboolean initialExternalMode)
{
  return static_cast<jlong>(
      GetBackActivityBinding().OnCreated(env, context, initialExternalMode == JNI_TRUE));
}

void CJNIMainActivity::_onBackStarted(JNIEnv* env,
                                      jobject context,
                                      jlong lifecycleToken,
                                      jint source)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (GetBackActivityBinding().IsCurrent(env, context, token))
    GetJumpgateBackDispatcher().OnApi36BackStarted(token, source);
}

void CJNIMainActivity::_onBackLongPress(JNIEnv* env, jobject context, jlong lifecycleToken)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (GetBackActivityBinding().IsCurrent(env, context, token))
    GetJumpgateBackDispatcher().OnApi36BackLongPress(token);
}

void CJNIMainActivity::_onBackCancelled(JNIEnv* env, jobject context, jlong lifecycleToken)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (GetBackActivityBinding().IsCurrent(env, context, token))
    GetJumpgateBackDispatcher().OnApi36BackCancelled(token);
}

void CJNIMainActivity::_onBackInvoked(JNIEnv* env, jobject context, jlong lifecycleToken)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (GetBackActivityBinding().IsCurrent(env, context, token))
    GetJumpgateBackDispatcher().OnApi36BackInvoked(token);
}

void CJNIMainActivity::_onBackDestroyed(JNIEnv* env, jobject context, jlong lifecycleToken)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (GetBackActivityBinding().IsCurrent(env, context, token))
  {
    if (const auto appInstance = AcquireAppInstance(token))
      appInstance->onBackLifecycleRetiring(token);
    RetireAppLifecycle(token);
  }
  GetBackActivityBinding().OnDestroyed(env, context, token);
}

void CJNIMainActivity::_callNative(JNIEnv *env, jobject context, jlong funcAddr, jlong variantAddr)
{
  (void)env;
  (void)context;
  ((void (*)(CVariant *))funcAddr)((CVariant *)variantAddr);
}

void CJNIMainActivity::_onVisibleBehindCanceled(JNIEnv* env,
                                                jobject context,
                                                jlong lifecycleToken)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (!GetBackActivityBinding().IsCurrent(env, context, token))
    return;
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onVisibleBehindCanceled();
}

void CJNIMainActivity::runNativeOnUiThread(void (*callback)(void*), void* variant)
{
  call_method<void>(m_context,
                    "runNativeOnUiThread", "(JJ)V", (jlong)callback, (jlong)variant);
}

void CJNIMainActivity::_onVolumeChanged(JNIEnv* env,
                                        jobject context,
                                        jlong lifecycleToken,
                                        jint volume)
{
  (void)env;
  (void)context;
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onVolumeChanged(volume);
}

void CJNIMainActivity::_onInputDeviceAdded(JNIEnv* env,
                                           jobject context,
                                           jlong lifecycleToken,
                                           jint deviceId)
{
  static_cast<void>(env);
  static_cast<void>(context);

  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onInputDeviceAdded(deviceId);
}

void CJNIMainActivity::_onInputDeviceChanged(JNIEnv* env,
                                             jobject context,
                                             jlong lifecycleToken,
                                             jint deviceId)
{
  static_cast<void>(env);
  static_cast<void>(context);

  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onInputDeviceChanged(deviceId);
}

void CJNIMainActivity::_onInputDeviceRemoved(JNIEnv* env,
                                             jobject context,
                                             jlong lifecycleToken,
                                             jint deviceId)
{
  static_cast<void>(env);
  static_cast<void>(context);

  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->onInputDeviceRemoved(deviceId);
}

void CJNIMainActivity::_doFrame(JNIEnv* env,
                                jobject context,
                                jlong lifecycleToken,
                                jlong frameTimeNanos)
{
  const LifecycleToken token = ToLifecycleToken(lifecycleToken);
  if (!GetBackActivityBinding().IsCurrent(env, context, token))
    return;
  if (const auto appInstance = AcquireAppInstance(token))
    appInstance->doFrame(frameTimeNanos);
}

CJNIRect CJNIMainActivity::getDisplayRect()
{
  return call_method<jhobject>(m_context,
                               "getDisplayRect", "()Landroid/graphics/Rect;");
}

KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken CJNIMainActivity::
    GetJumpgateBackLifecycleToken(const ANativeActivity* nativeActivity)
{
  if (nativeActivity == nullptr || nativeActivity->clazz == nullptr)
    return KODI::JUMPGATE::CJumpgateBackDispatcher::INVALID_LIFECYCLE_TOKEN;

  return static_cast<KODI::JUMPGATE::CJumpgateBackDispatcher::LifecycleToken>(
      call_method<jlong>(jhobject::fromJNI(nativeActivity->clazz), "getBackLifecycleToken", "()J"));
}
