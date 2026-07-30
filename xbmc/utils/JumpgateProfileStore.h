/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Variant.h"

#include <cstdint>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

constexpr int PROFILE_SCHEMA_VERSION = 2;
constexpr int CREDENTIAL_SCHEMA_VERSION = 1;

struct PairingRedemption
{
  std::string profileId;
  std::string deviceId;
  std::string deviceToken;
  std::string bridgeBaseUrl;
  std::string bridgeOrigin;
  std::string bridgeRouteKind;
  std::string config;
  std::string name;
  CVariant settings{CVariant::VariantTypeObject};
  CVariant capabilities{CVariant::VariantTypeObject};

  void ClearSecrets();
};

using PairingPayload = PairingRedemption;

struct StoredProfile
{
  std::string profileId;
  std::string deviceId;
  std::string credentialRef;
  std::string name;
  std::string state;
  std::string bridgeOrigin;
  std::string bridgeRouteKind;
  CVariant settings{CVariant::VariantTypeObject};
  int64_t addedAt{0};
  int64_t updatedAt{0};

  // Present only while importing a pre-schema URL-only profile. It must never be serialized.
  std::string legacyBridgeBaseUrl;

  bool IsSourceBacked() const;
};

struct ProfileDocument
{
  CVariant defaults{CVariant::VariantTypeObject};
  std::vector<StoredProfile> profiles;
  std::string activeProfileId;
  std::string pairingOrigin;
};

struct ActiveProfile
{
  bool selected{false};
  bool sourceBacked{false};
  bool credentialsValid{false};
  std::string profileId;
  std::string deviceId;
  std::string name;
  std::string state;
  std::string bridgeOrigin;
  std::string bridgeBaseUrl;
  std::string deviceToken;
  CVariant settings{CVariant::VariantTypeObject};
  bool traktEnabled{false};
  bool subtitlesEnabled{true};
  bool autoUpdateCheck{true};
  std::string subtitleLanguages{"en"};

  void ClearSecrets();
};

class IJumpgateProfileStorage
{
public:
  virtual ~IJumpgateProfileStorage() = default;
  virtual bool Read(std::string& contents, bool& exists, std::string& error) = 0;
  virtual bool WriteAtomic(const std::string& contents, std::string& error) = 0;
};

class IJumpgateCredentialStore
{
public:
  virtual ~IJumpgateCredentialStore() = default;
  virtual bool Store(const std::string& profileId,
                     const std::string& deviceId,
                     const std::string& secretJson,
                     std::string& credentialRef,
                     std::string& error) = 0;
  virtual bool Load(const std::string& profileId,
                    const std::string& deviceId,
                    const std::string& credentialRef,
                    std::string& secretJson,
                    std::string& error) = 0;
  virtual bool Remove(const std::string& credentialRef, std::string& error) = 0;
};

bool ParsePairingPayload(const CVariant& response,
                         bool allowInsecureLoopback,
                         PairingPayload& payload,
                         std::string& error);

bool ParseProfileDocument(const CVariant& root,
                          ProfileDocument& document,
                          bool& requiresRewrite,
                          std::string& error);

bool SerializeProfileDocument(const ProfileDocument& document, CVariant& root, std::string& error);

class CJumpgateProfileStore
{
public:
  CJumpgateProfileStore(IJumpgateProfileStorage& storage, IJumpgateCredentialStore& credentials);

  bool Load(ProfileDocument& document, std::string& error);
  bool StorePairing(ProfileDocument& document,
                    PairingPayload& payload,
                    int64_t now,
                    std::string& error);
  bool LoadActive(const ProfileDocument& document, ActiveProfile& active, std::string& error);
  bool SelectActive(ProfileDocument& document, const std::string& profileId, std::string& error);
  bool ClearActive(ProfileDocument& document, std::string& error);
  bool MarkRevocationPending(ProfileDocument& document,
                             const std::string& profileId,
                             const std::string& deviceId,
                             int64_t now,
                             std::string& error);
  bool ForgetLocal(ProfileDocument& document,
                   const std::string& profileId,
                   const std::string& deviceId,
                   std::string& error);
  bool SetActiveSetting(ProfileDocument& document,
                        const std::string& key,
                        const CVariant& value,
                        std::string& error);
  bool SetPairingOrigin(ProfileDocument& document,
                        const std::string& origin,
                        bool allowInsecureLoopback,
                        std::string& error);

private:
  bool SaveCandidate(ProfileDocument& document, ProfileDocument& candidate, std::string& error);
  bool MigrateLegacyCredentials(ProfileDocument& document,
                                std::vector<std::string>& createdRefs,
                                std::string& error);

  IJumpgateProfileStorage& m_storage;
  IJumpgateCredentialStore& m_credentials;
};

const StoredProfile* FindProfile(const ProfileDocument& document, const std::string& profileId);
StoredProfile* FindProfile(ProfileDocument& document, const std::string& profileId);

bool IsValidPairingOrigin(const std::string& origin, bool allowInsecureLoopback);
bool NormalizePairingOrigin(const std::string& origin,
                            bool allowInsecureLoopback,
                            std::string& normalized);
std::string RedactBridgeForDisplay(const std::string& origin);

} // namespace KODI::JUMPGATE
