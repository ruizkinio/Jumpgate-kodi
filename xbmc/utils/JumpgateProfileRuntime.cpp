/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateProfileRuntime.h"

#include <utility>

namespace KODI::JUMPGATE
{

CJumpgateProfileRuntime::CJumpgateProfileRuntime(IJumpgateProfileStorage& storage,
                                                 IJumpgateCredentialStore& credentials)
  : m_store(storage, credentials)
{
}

CJumpgateProfileRuntime::~CJumpgateProfileRuntime()
{
  m_active.ClearSecrets();
}

bool CJumpgateProfileRuntime::Initialize(std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_initialized)
  {
    error.clear();
    return true;
  }
  return ReloadLocked(error);
}

bool CJumpgateProfileRuntime::Reload(std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  return ReloadLocked(error);
}

bool CJumpgateProfileRuntime::IsInitialized() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_initialized;
}

ProfileDocument CJumpgateProfileRuntime::GetDocument() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_document;
}

ActiveProfile CJumpgateProfileRuntime::GetActive() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_active;
}

std::vector<ProfileMetadata> CJumpgateProfileRuntime::GetProfiles() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ProfileMetadata> profiles;
  profiles.reserve(m_document.profiles.size());
  for (const StoredProfile& stored : m_document.profiles)
  {
    ProfileMetadata profile;
    profile.active = stored.profileId == m_document.activeProfileId;
    profile.sourceBacked = stored.IsSourceBacked();
    profile.profileId = stored.profileId;
    profile.deviceId = stored.deviceId;
    profile.name = stored.name;
    profile.state = stored.state;
    profile.bridgeOrigin = stored.bridgeOrigin;
    profile.bridgeRouteKind = stored.bridgeRouteKind;
    profile.settings = stored.settings;
    profile.addedAt = stored.addedAt;
    profile.updatedAt = stored.updatedAt;
    profiles.push_back(std::move(profile));
  }
  return profiles;
}

std::string CJumpgateProfileRuntime::GetPairingOrigin() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_document.pairingOrigin;
}

ProfileMutationResult CJumpgateProfileRuntime::StorePairingResponse(
    const CVariant& response,
    const std::string& expectedOrigin,
    bool allowInsecureLoopback,
    int64_t now,
    std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error))
    return {ProfileMutationStatus::NotCommitted};

  std::string normalizedExpectedOrigin;
  if (!NormalizePairingOrigin(expectedOrigin, allowInsecureLoopback, normalizedExpectedOrigin))
  {
    error = "captured pairing origin is invalid";
    return {ProfileMutationStatus::NotCommitted};
  }

  PairingPayload payload;
  if (!ParsePairingPayload(response, allowInsecureLoopback, payload, error))
    return {ProfileMutationStatus::NotCommitted};
  if (payload.bridgeOrigin != normalizedExpectedOrigin)
  {
    payload.ClearSecrets();
    error = "pairing response origin does not match the captured pairing origin";
    return {ProfileMutationStatus::NotCommitted};
  }

  const bool stored = m_store.StorePairing(m_document, payload, now, error);
  payload.ClearSecrets();
  if (!stored)
    return {ProfileMutationStatus::NotCommitted};
  return RefreshAfterMutationLocked(error);
}

ProfileMutationResult CJumpgateProfileRuntime::SelectActive(const std::string& profileId,
                                                            std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error) || !m_store.SelectActive(m_document, profileId, error))
    return {ProfileMutationStatus::NotCommitted};
  return RefreshAfterMutationLocked(error);
}

ProfileMutationResult CJumpgateProfileRuntime::ClearActive(std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error) || !m_store.ClearActive(m_document, error))
    return {ProfileMutationStatus::NotCommitted};
  return RefreshAfterMutationLocked(error);
}

ForgetLocalResult CJumpgateProfileRuntime::ForgetLocal(const std::string& profileId,
                                                       const std::string& deviceId,
                                                       std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error) ||
      !m_store.ForgetLocal(m_document, profileId, deviceId, error))
  {
    return {ProfileMutationStatus::NotCommitted};
  }
  return RefreshAfterMutationLocked(error);
}

ProfileMutationResult CJumpgateProfileRuntime::SetActiveSetting(const std::string& key,
                                                                const CVariant& value,
                                                                std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error) || !m_store.SetActiveSetting(m_document, key, value, error))
    return {ProfileMutationStatus::NotCommitted};
  return RefreshAfterMutationLocked(error);
}

ProfileMutationResult CJumpgateProfileRuntime::SetPairingOrigin(const std::string& origin,
                                                                bool allowInsecureLoopback,
                                                                std::string& error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  error.clear();
  if (!EnsureInitializedLocked(error) ||
      !m_store.SetPairingOrigin(m_document, origin, allowInsecureLoopback, error))
    return {ProfileMutationStatus::NotCommitted};
  return RefreshAfterMutationLocked(error);
}

bool CJumpgateProfileRuntime::EnsureInitializedLocked(std::string& error)
{
  return m_initialized || ReloadLocked(error);
}

bool CJumpgateProfileRuntime::ReloadLocked(std::string& error)
{
  ProfileDocument document;
  ActiveProfile active;
  if (!m_store.Load(document, error) || !m_store.LoadActive(document, active, error))
  {
    active.ClearSecrets();
    m_active.ClearSecrets();
    m_active = ActiveProfile{};
    m_initialized = false;
    return false;
  }

  m_active.ClearSecrets();
  m_document = std::move(document);
  m_active = std::move(active);
  m_initialized = true;
  return true;
}

bool CJumpgateProfileRuntime::RefreshActiveLocked(std::string& error)
{
  ActiveProfile active;
  const bool loaded = m_store.LoadActive(m_document, active, error);
  if (!loaded)
    active.ClearSecrets();
  m_active.ClearSecrets();
  m_active = std::move(active);
  return loaded;
}

ProfileMutationResult CJumpgateProfileRuntime::RefreshAfterMutationLocked(std::string& error)
{
  const std::string mutationMessage = error;
  std::string refreshError;
  if (!RefreshActiveLocked(refreshError))
  {
    error = std::move(refreshError);
    return {ProfileMutationStatus::CommittedRefreshFailed};
  }
  error = mutationMessage;
  return {ProfileMutationStatus::Committed};
}

} // namespace KODI::JUMPGATE
