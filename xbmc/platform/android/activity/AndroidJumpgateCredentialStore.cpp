/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AndroidJumpgateCredentialStore.h"

#include "CompileInfo.h"
#include "JNIMainActivity.h"

#include <androidjni/Context.h>
#include <androidjni/jutils-details.hpp>

using namespace jni;

namespace KODI::JUMPGATE
{
namespace
{
const std::string PROFILE_NAMESPACE = "jumpgate.profile";

jhclass CredentialStoreClass()
{
  const std::string className =
      std::string(CCompileInfo::GetPackage()) + ".JumpgateCredentialStore";
  return CJNIContext::getClassLoader().loadClass(className);
}

jhobject ActivityContext()
{
  CJNIMainActivity* activity = CJNIMainActivity::GetAppInstance();
  return activity ? activity->CJNIContext::get_raw() : jhobject{};
}

} // namespace

bool CAndroidJumpgateCredentialStore::Store(const std::string& profileId,
                                            const std::string& deviceId,
                                            const std::string& secretJson,
                                            std::string& credentialRef,
                                            std::string& error)
{
  credentialRef.clear();
  error.clear();
  const jhobject context = ActivityContext();
  if (!context)
  {
    error = "Android credential context is unavailable";
    return false;
  }

  try
  {
    const jhstring result = call_static_method<jhstring>(
        xbmc_jnienv(), CredentialStoreClass(), "store",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;)Ljava/lang/String;",
        context, jcast<jhstring>(PROFILE_NAMESPACE), jcast<jhstring>(profileId),
        jcast<jhstring>(deviceId), jcast<jhstring>(secretJson));
    if (!result)
    {
      error = "Android Keystore rejected the credential write";
      return false;
    }
    credentialRef = jcast<std::string>(result);
    return true;
  }
  catch (...)
  {
    error = "Android Keystore credential write failed closed";
    return false;
  }
}

bool CAndroidJumpgateCredentialStore::Load(const std::string& profileId,
                                           const std::string& deviceId,
                                           const std::string& credentialRef,
                                           std::string& secretJson,
                                           std::string& error)
{
  secretJson.clear();
  error.clear();
  const jhobject context = ActivityContext();
  if (!context)
  {
    error = "Android credential context is unavailable";
    return false;
  }

  try
  {
    const jhstring result = call_static_method<jhstring>(
        xbmc_jnienv(), CredentialStoreClass(), "load",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;)Ljava/lang/String;",
        context, jcast<jhstring>(PROFILE_NAMESPACE), jcast<jhstring>(profileId),
        jcast<jhstring>(deviceId), jcast<jhstring>(credentialRef));
    if (!result)
    {
      error = "Jumpgate credentials are unavailable or invalid; re-pairing is required";
      return false;
    }
    secretJson = jcast<std::string>(result);
    return true;
  }
  catch (...)
  {
    error = "Jumpgate credentials failed authentication; re-pairing is required";
    return false;
  }
}

bool CAndroidJumpgateCredentialStore::Remove(const std::string& credentialRef, std::string& error)
{
  error.clear();
  const jhobject context = ActivityContext();
  if (!context)
  {
    error = "Android credential context is unavailable";
    return false;
  }

  try
  {
    const jboolean removed = call_static_method<jboolean>(
        xbmc_jnienv(), CredentialStoreClass(), "remove",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z", context,
        jcast<jhstring>(PROFILE_NAMESPACE), jcast<jhstring>(credentialRef));
    if (!removed)
      error = "encrypted credential cleanup failed";
    return removed;
  }
  catch (...)
  {
    error = "encrypted credential cleanup failed";
    return false;
  }
}

} // namespace KODI::JUMPGATE
