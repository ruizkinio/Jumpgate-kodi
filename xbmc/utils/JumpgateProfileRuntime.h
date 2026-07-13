/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/JumpgateProfileStore.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

struct ProfileMetadata
{
  bool active{false};
  bool sourceBacked{false};
  std::string profileId;
  std::string deviceId;
  std::string name;
  std::string state;
  std::string bridgeOrigin;
  std::string bridgeRouteKind;
  CVariant settings{CVariant::VariantTypeObject};
  int64_t addedAt{0};
  int64_t updatedAt{0};
};

class CJumpgateProfileRuntime
{
public:
  CJumpgateProfileRuntime(IJumpgateProfileStorage& storage, IJumpgateCredentialStore& credentials);
  ~CJumpgateProfileRuntime();

  CJumpgateProfileRuntime(const CJumpgateProfileRuntime&) = delete;
  CJumpgateProfileRuntime& operator=(const CJumpgateProfileRuntime&) = delete;

  bool Initialize(std::string& error);
  bool Reload(std::string& error);
  bool IsInitialized() const;

  ProfileDocument GetDocument() const;
  ActiveProfile GetActive() const;
  std::vector<ProfileMetadata> GetProfiles() const;
  std::string GetPairingOrigin() const;

  bool StorePairingResponse(const CVariant& response,
                            const std::string& expectedOrigin,
                            bool allowInsecureLoopback,
                            int64_t now,
                            std::string& error);
  bool SelectActive(const std::string& profileId, std::string& error);
  bool ClearActive(std::string& error);
  bool ForgetLocal(const std::string& profileId, const std::string& deviceId, std::string& error);
  bool SetActiveSetting(const std::string& key, const CVariant& value, std::string& error);
  bool SetPairingOrigin(const std::string& origin, bool allowInsecureLoopback, std::string& error);

private:
  bool EnsureInitializedLocked(std::string& error);
  bool ReloadLocked(std::string& error);
  bool RefreshActiveLocked(std::string& error);
  bool RefreshAfterMutationLocked(std::string& error);

  mutable std::mutex m_mutex;
  CJumpgateProfileStore m_store;
  ProfileDocument m_document;
  ActiveProfile m_active;
  bool m_initialized{false};
};

} // namespace KODI::JUMPGATE
