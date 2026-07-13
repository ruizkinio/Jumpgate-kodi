/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/JumpgateProfileStore.h"

#include <algorithm>
#include <map>
#include <string>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* PROFILE_A = "profile_source_backed_a";
constexpr const char* PROFILE_B = "profile_source_backed_b";
constexpr const char* DEVICE_A = "device_source_backed_a";
constexpr const char* DEVICE_B = "device_source_backed_b";
const std::string TOKEN_A(43, 'A');
const std::string TOKEN_B(43, 'B');
const std::string CONFIG_A(32, 'C');
const std::string CONFIG_B(32, 'D');

class FakeProfileStorage : public IJumpgateProfileStorage
{
public:
  bool Read(std::string& contents, bool& exists, std::string& error) override
  {
    if (failRead)
    {
      error = "simulated read failure";
      return false;
    }
    exists = hasContents;
    contents = bytes;
    return true;
  }

  bool WriteAtomic(const std::string& contents, std::string& error) override
  {
    ++writeAttempts;
    if (failWrite)
    {
      error = "simulated atomic replace failure";
      return false;
    }
    bytes = contents;
    hasContents = true;
    if (warnAfterCommit)
      error = "metadata committed; directory sync was not confirmed";
    return true;
  }

  bool hasContents{false};
  bool failRead{false};
  bool failWrite{false};
  bool warnAfterCommit{false};
  int writeAttempts{0};
  std::string bytes;
};

class FakeCredentialStore : public IJumpgateCredentialStore
{
public:
  struct Record
  {
    std::string profileId;
    std::string deviceId;
    std::string sealedJson;
  };

  static std::string Seal(std::string value)
  {
    for (char& c : value)
      c = static_cast<char>(static_cast<unsigned char>(c) ^ 0xa5);
    return value;
  }

  bool Store(const std::string& profileId,
             const std::string& deviceId,
             const std::string& secretJson,
             std::string& credentialRef,
             std::string& error) override
  {
    if (failStore)
    {
      error = "simulated keystore failure";
      return false;
    }
    credentialRef = "jgcred_" + std::string(15, 'x') + std::to_string(++sequence);
    records[credentialRef] = {profileId, deviceId, Seal(secretJson)};
    return true;
  }

  bool Load(const std::string& profileId,
            const std::string& deviceId,
            const std::string& credentialRef,
            std::string& secretJson,
            std::string& error) override
  {
    const auto it = records.find(credentialRef);
    if (failLoad || it == records.end() || it->second.profileId != profileId ||
        it->second.deviceId != deviceId)
    {
      error = "credential authentication failed";
      return false;
    }
    secretJson = Seal(it->second.sealedJson);
    return true;
  }

  bool Remove(const std::string& credentialRef, std::string&) override
  {
    removed.push_back(credentialRef);
    if (failRemove)
      return false;
    records.erase(credentialRef);
    return true;
  }

  bool failStore{false};
  bool failLoad{false};
  bool failRemove{false};
  int sequence{0};
  std::map<std::string, Record> records;
  std::vector<std::string> removed;
};

CVariant PairingResponse(const std::string& profileId,
                         const std::string& deviceId,
                         const std::string& token,
                         const std::string& config,
                         const std::string& origin = "https://bridge.example")
{
  CVariant response(CVariant::VariantTypeObject);
  response["ok"] = true;
  response["paired"] = true;
  response["profileId"] = profileId;
  response["deviceId"] = deviceId;
  response["deviceToken"] = token;
  response["bridgeBaseUrl"] = origin + "/_c/" + config;
  response["config"] = config;
  response["name"] = profileId == PROFILE_A ? "Living room" : "Bedroom";
  CVariant settings(CVariant::VariantTypeObject);
  settings["subtitle_languages"] = "en,es";
  settings["subtitles_enabled"] = true;
  settings["trakt_enabled"] = true;
  settings["future_setting"] = "preserve-me";
  response["settings"] = settings;
  return response;
}

PairingPayload ParsePair(const CVariant& response, bool allowLoopback = true)
{
  PairingPayload payload;
  std::string error;
  EXPECT_TRUE(ParsePairingPayload(response, allowLoopback, payload, error)) << error;
  return payload;
}

std::string Json(const CVariant& value)
{
  std::string json;
  EXPECT_TRUE(CJSONVariantWriter::Write(value, json, true));
  return json;
}

} // namespace

TEST(TestJumpgateProfileStore, PairingAliasesAreValidatedAndCanonicalized)
{
  CVariant response = PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A);
  response["profile_id"] = PROFILE_A;
  response["device_id"] = DEVICE_A;
  response["device_token"] = TOKEN_A;
  response["bridge_base_url"] = "https://bridge.example/_c/" + CONFIG_A;
  response["settings"]["subtitleLanguages"] = "en,es";

  PairingPayload payload;
  std::string error;
  ASSERT_TRUE(ParsePairingPayload(response, false, payload, error)) << error;
  EXPECT_EQ(payload.profileId, PROFILE_A);
  EXPECT_EQ(payload.deviceId, DEVICE_A);
  EXPECT_EQ(payload.bridgeOrigin, "https://bridge.example");
  EXPECT_EQ(payload.bridgeRouteKind, "configured");
  EXPECT_FALSE(payload.settings.isMember("future_setting"));
  EXPECT_FALSE(payload.settings.isMember("subtitleLanguages"));

  response["profile_id"] = PROFILE_B;
  EXPECT_FALSE(ParsePairingPayload(response, false, payload, error));
  EXPECT_TRUE(payload.deviceToken.empty());
}

TEST(TestJumpgateProfileStore, PairingEnforcesTlsLoopbackAndExactConfigRoute)
{
  PairingPayload payload;
  std::string error;

  CVariant production =
      PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, "http://bridge.example");
  EXPECT_FALSE(ParsePairingPayload(production, true, payload, error));

  CVariant loopback =
      PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, "http://127.0.0.1:7515");
  EXPECT_FALSE(ParsePairingPayload(loopback, false, payload, error));
  EXPECT_TRUE(ParsePairingPayload(loopback, true, payload, error)) << error;

  CVariant ipv6 = PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, "http://[::1]:7515");
  EXPECT_TRUE(ParsePairingPayload(ipv6, true, payload, error)) << error;

  CVariant mismatch = PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A);
  mismatch["bridgeBaseUrl"] = "https://bridge.example/_c/" + CONFIG_B;
  EXPECT_FALSE(ParsePairingPayload(mismatch, false, payload, error));
}

TEST(TestJumpgateProfileStore, SchemaSerializationIsCanonicalAndDropsUnknownMaterial)
{
  CVariant root(CVariant::VariantTypeObject);
  root["schemaVersion"] = 7;
  root["activeProfileId"] = PROFILE_A;
  root["pairingOrigin"] = "https://bridge.example";
  root["futureRoot"] = "root-value";
  root["oauthSecret"] = TOKEN_A;
  root["defaults"] = CVariant(CVariant::VariantTypeObject);
  CVariant profiles(CVariant::VariantTypeArray);
  CVariant item(CVariant::VariantTypeObject);
  item["schemaVersion"] = 9;
  item["profileId"] = PROFILE_A;
  item["deviceId"] = DEVICE_A;
  item["credentialRef"] = "jgcred_abcdefghijklmnop";
  item["name"] = "Living room";
  item["state"] = "paired";
  item["futureProfile"] = 42;
  item["deviceToken"] = TOKEN_A;
  item["bridge"] = CVariant(CVariant::VariantTypeObject);
  item["bridge"]["origin"] = "https://bridge.example";
  item["bridge"]["routeKind"] = "configured";
  item["bridge"]["futureRoute"] = "kept";
  item["settings"] = CVariant(CVariant::VariantTypeObject);
  item["settings"]["trakt_enabled"] = false;
  item["settings"]["future_setting"] = "kept";
  profiles.push_back(item);
  profiles.push_back(CVariant{42});
  CVariant malformed(CVariant::VariantTypeObject);
  malformed["profileId"] = "bad";
  malformed["bridgeBaseUrl"] = "https://bridge.example/_c/" + CONFIG_A;
  malformed["deviceToken"] = TOKEN_B;
  profiles.push_back(malformed);
  root["profiles"] = profiles;

  ProfileDocument document;
  bool rewrite = false;
  std::string error;
  ASSERT_TRUE(ParseProfileDocument(root, document, rewrite, error)) << error;
  EXPECT_TRUE(rewrite);
  CVariant serialized;
  ASSERT_TRUE(SerializeProfileDocument(document, serialized, error)) << error;
  EXPECT_EQ(serialized["schemaVersion"].asInteger(), PROFILE_SCHEMA_VERSION);
  ASSERT_EQ(serialized["profiles"].size(), 1u);
  EXPECT_EQ(serialized["profiles"][0]["schemaVersion"].asInteger(), PROFILE_SCHEMA_VERSION);
  EXPECT_FALSE(serialized.isMember("futureRoot"));
  EXPECT_FALSE(serialized.isMember("oauthSecret"));
  EXPECT_FALSE(serialized["profiles"][0].isMember("futureProfile"));
  EXPECT_FALSE(serialized["profiles"][0].isMember("deviceToken"));
  EXPECT_FALSE(serialized["profiles"][0]["bridge"].isMember("futureRoute"));
  EXPECT_FALSE(serialized["profiles"][0]["settings"].isMember("future_setting"));
  const std::string json = Json(serialized);
  EXPECT_EQ(json.find(TOKEN_A), std::string::npos);
  EXPECT_EQ(json.find(TOKEN_B), std::string::npos);
}

TEST(TestJumpgateProfileStore, UrlOnlyProfilesMigrateToEncryptedLegacyUnpairedRecords)
{
  const std::string legacyUrl = "https://bridge.example/_c/" + CONFIG_A;
  CVariant root(CVariant::VariantTypeObject);
  root["bridge_url"] = legacyUrl;
  root["active_profile_name"] = "Old living room";
  root["subtitle_languages"] = "nl,en";
  root["trakt_enabled"] = true;
  CVariant profiles(CVariant::VariantTypeArray);
  CVariant old(CVariant::VariantTypeObject);
  old["name"] = "Old living room";
  old["bridge_url"] = legacyUrl;
  old["unknownLegacy"] = "kept";
  profiles.push_back(old);
  root["profiles"] = profiles;

  FakeProfileStorage metadata;
  metadata.hasContents = true;
  metadata.bytes = Json(root);
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;
  ASSERT_EQ(document.profiles.size(), 1u);
  const StoredProfile& migrated = document.profiles[0];
  EXPECT_EQ(migrated.state, "legacy_unpaired");
  EXPECT_TRUE(migrated.legacyBridgeBaseUrl.empty());
  EXPECT_FALSE(migrated.credentialRef.empty());
  EXPECT_EQ(document.activeProfileId, migrated.profileId);
  EXPECT_EQ(migrated.settings["subtitle_languages"].asString(), "nl,en");
  EXPECT_EQ(credentials.records.size(), 1u);
  EXPECT_EQ(metadata.bytes.find(legacyUrl), std::string::npos);
  EXPECT_EQ(metadata.bytes.find(CONFIG_A), std::string::npos);
  EXPECT_EQ(metadata.bytes.find("unknownLegacy"), std::string::npos);

  const std::string stableProfileId = migrated.profileId;
  ProfileDocument reloaded;
  ASSERT_TRUE(store.Load(reloaded, error)) << error;
  ASSERT_EQ(reloaded.profiles.size(), 1u);
  EXPECT_EQ(reloaded.profiles[0].profileId, stableProfileId);
}

TEST(TestJumpgateProfileStore, MixedLegacyUrlAndCredentialPreservesCredentialReference)
{
  const std::string legacyUrl = "https://bridge.example/_c/" + CONFIG_A;
  const std::string credentialRef = "jgcred_abcdefghijklmnop";
  CVariant root(CVariant::VariantTypeObject);
  root["schemaVersion"] = PROFILE_SCHEMA_VERSION;
  root["activeProfileId"] = PROFILE_A;
  root["pairingOrigin"] = "https://bridge.example";
  root["defaults"] = CVariant(CVariant::VariantTypeObject);
  CVariant profiles(CVariant::VariantTypeArray);
  CVariant item(CVariant::VariantTypeObject);
  item["schemaVersion"] = PROFILE_SCHEMA_VERSION;
  item["profileId"] = PROFILE_A;
  item["deviceId"] = DEVICE_A;
  item["credentialRef"] = credentialRef;
  item["name"] = "Living room";
  item["state"] = "paired";
  item["bridgeBaseUrl"] = legacyUrl;
  item["bridge"] = CVariant(CVariant::VariantTypeObject);
  item["bridge"]["origin"] = "https://bridge.example";
  item["bridge"]["routeKind"] = "configured";
  item["settings"] = CVariant(CVariant::VariantTypeObject);
  item["addedAt"] = 1;
  item["updatedAt"] = 2;
  profiles.push_back(item);
  root["profiles"] = profiles;

  FakeProfileStorage metadata;
  metadata.hasContents = true;
  metadata.bytes = Json(root);
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;
  ASSERT_EQ(document.profiles.size(), 1u);
  EXPECT_EQ(document.profiles[0].credentialRef, credentialRef);
  EXPECT_TRUE(document.profiles[0].legacyBridgeBaseUrl.empty());
  EXPECT_TRUE(credentials.records.empty());
  EXPECT_EQ(metadata.bytes.find(legacyUrl), std::string::npos);
  EXPECT_EQ(metadata.bytes.find(CONFIG_A), std::string::npos);
}

TEST(TestJumpgateProfileStore, PairingRollsBackCredentialWhenAtomicMetadataWriteFails)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;
  const std::string before = metadata.bytes;

  PairingPayload payload = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  metadata.failWrite = true;
  EXPECT_FALSE(store.StorePairing(document, payload, 1000, error));
  EXPECT_EQ(metadata.bytes, before);
  EXPECT_TRUE(document.profiles.empty());
  EXPECT_TRUE(credentials.records.empty());
  EXPECT_FALSE(credentials.removed.empty());
  EXPECT_TRUE(payload.deviceToken.empty());
}

TEST(TestJumpgateProfileStore, UnconfirmedDirectorySyncRetainsBothCredentialGenerations)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;

  PairingPayload first = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  ASSERT_TRUE(store.StorePairing(document, first, 1000, error)) << error;
  const std::string oldRef = FindProfile(document, PROFILE_A)->credentialRef;
  ASSERT_TRUE(credentials.records.contains(oldRef));

  metadata.warnAfterCommit = true;
  PairingPayload replacement = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_B, CONFIG_B));
  ASSERT_TRUE(store.StorePairing(document, replacement, 2000, error));
  EXPECT_FALSE(error.empty());

  const std::string newRef = FindProfile(document, PROFILE_A)->credentialRef;
  EXPECT_NE(newRef, oldRef);
  EXPECT_TRUE(credentials.records.contains(oldRef));
  EXPECT_TRUE(credentials.records.contains(newRef));
  EXPECT_EQ(std::find(credentials.removed.begin(), credentials.removed.end(), oldRef),
            credentials.removed.end());

  ASSERT_TRUE(store.ForgetLocal(document, PROFILE_A, DEVICE_A, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(FindProfile(document, PROFILE_A), nullptr);
  EXPECT_TRUE(credentials.records.contains(newRef));
  EXPECT_EQ(std::find(credentials.removed.begin(), credentials.removed.end(), newRef),
            credentials.removed.end());
}

TEST(TestJumpgateProfileStore, FailedPreviousCredentialCleanupIsNonDestructive)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;

  PairingPayload first = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  ASSERT_TRUE(store.StorePairing(document, first, 1000, error)) << error;
  const std::string oldRef = FindProfile(document, PROFILE_A)->credentialRef;

  credentials.failRemove = true;
  PairingPayload replacement = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_B, CONFIG_B));
  ASSERT_TRUE(store.StorePairing(document, replacement, 2000, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(credentials.records.contains(oldRef));
  EXPECT_TRUE(credentials.records.contains(FindProfile(document, PROFILE_A)->credentialRef));
}

TEST(TestJumpgateProfileStore, ActiveProfileLoadingHasWarmColdParityAndExactIsolation)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;

  PairingPayload first = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  ASSERT_TRUE(store.StorePairing(document, first, 1000, error)) << error;
  PairingPayload second = ParsePair(PairingResponse(PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B));
  ASSERT_TRUE(store.StorePairing(document, second, 2000, error)) << error;
  ASSERT_TRUE(store.SelectActive(document, PROFILE_A, error)) << error;

  ActiveProfile cold;
  ActiveProfile warm;
  ASSERT_TRUE(store.LoadActive(document, cold, error)) << error;
  ASSERT_TRUE(store.LoadActive(document, warm, error)) << error;
  EXPECT_EQ(cold.profileId, PROFILE_A);
  EXPECT_EQ(cold.deviceId, DEVICE_A);
  EXPECT_EQ(cold.bridgeBaseUrl, "https://bridge.example/_c/" + CONFIG_A);
  EXPECT_EQ(cold.deviceToken, TOKEN_A);
  EXPECT_EQ(warm.profileId, cold.profileId);
  EXPECT_EQ(warm.deviceToken, cold.deviceToken);

  StoredProfile* a = FindProfile(document, PROFILE_A);
  const StoredProfile* b = FindProfile(document, PROFILE_B);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  a->credentialRef = b->credentialRef;
  ActiveProfile isolated;
  EXPECT_FALSE(store.LoadActive(document, isolated, error));
  EXPECT_TRUE(isolated.deviceToken.empty());
  EXPECT_FALSE(isolated.traktEnabled);
}

TEST(TestJumpgateProfileStore, MetadataNeverContainsDeviceTokenOrConfigCapability)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;
  PairingPayload payload = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  ASSERT_TRUE(store.StorePairing(document, payload, 1000, error)) << error;

  EXPECT_EQ(metadata.bytes.find(TOKEN_A), std::string::npos);
  EXPECT_EQ(metadata.bytes.find(CONFIG_A), std::string::npos);
  EXPECT_EQ(metadata.bytes.find("deviceToken"), std::string::npos);
  EXPECT_EQ(metadata.bytes.find("bridgeBaseUrl"), std::string::npos);
  ASSERT_EQ(credentials.records.size(), 1u);
  EXPECT_EQ(credentials.records.begin()->second.sealedJson.find(TOKEN_A), std::string::npos);
}

TEST(TestJumpgateProfileStore, SettingsClearRevocationAndForgetAreProfileScoped)
{
  FakeProfileStorage metadata;
  FakeCredentialStore credentials;
  CJumpgateProfileStore store(metadata, credentials);
  ProfileDocument document;
  std::string error;
  ASSERT_TRUE(store.Load(document, error)) << error;
  PairingPayload first = ParsePair(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A));
  ASSERT_TRUE(store.StorePairing(document, first, 1000, error)) << error;
  PairingPayload second = ParsePair(PairingResponse(PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B));
  ASSERT_TRUE(store.StorePairing(document, second, 2000, error)) << error;

  ASSERT_TRUE(store.SetActiveSetting(document, "trakt_enabled", CVariant{false}, error)) << error;
  const StoredProfile* b = FindProfile(document, PROFILE_B);
  ASSERT_NE(b, nullptr);
  EXPECT_FALSE(b->settings["trakt_enabled"].asBoolean());
  EXPECT_FALSE(b->settings.isMember("future_setting"));

  ASSERT_TRUE(store.ClearActive(document, error)) << error;
  EXPECT_TRUE(document.activeProfileId.empty());
  EXPECT_EQ(document.profiles.size(), 2u);
  EXPECT_EQ(credentials.records.size(), 2u);

  ASSERT_TRUE(store.SelectActive(document, PROFILE_A, error)) << error;
  ASSERT_TRUE(store.MarkRevocationPending(document, PROFILE_A, DEVICE_A, 3000, error)) << error;
  EXPECT_TRUE(document.activeProfileId.empty());
  EXPECT_EQ(FindProfile(document, PROFILE_A)->state, "revocation_pending");
  EXPECT_FALSE(store.SelectActive(document, PROFILE_A, error));

  ASSERT_TRUE(store.ForgetLocal(document, PROFILE_A, DEVICE_A, error)) << error;
  EXPECT_EQ(FindProfile(document, PROFILE_A), nullptr);
  EXPECT_NE(FindProfile(document, PROFILE_B), nullptr);
  EXPECT_EQ(credentials.records.size(), 1u);
}
