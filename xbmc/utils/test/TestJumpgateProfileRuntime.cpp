/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgateProfileRuntime.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* PROFILE_A = "profile_source_backed_a";
constexpr const char* PROFILE_B = "profile_source_backed_b";
constexpr const char* DEVICE_A = "device_source_backed_a";
constexpr const char* DEVICE_B = "device_source_backed_b";
constexpr const char* ORIGIN = "https://bridge.example";
const std::string TOKEN_A(43, 'A');
const std::string TOKEN_B(43, 'B');
const std::string CONFIG_A(32, 'C');
const std::string CONFIG_B(32, 'D');

class RuntimeProfileStorage : public IJumpgateProfileStorage
{
public:
  bool Read(std::string& contents, bool& exists, std::string& error) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_readAttempts;
    if (m_failRead)
    {
      error = "simulated read failure";
      return false;
    }
    contents = m_contents;
    exists = m_exists;
    return true;
  }

  bool WriteAtomic(const std::string& contents, std::string& error) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_writeAttempts;
    if (m_failWrite)
    {
      error = "simulated atomic write failure";
      return false;
    }
    m_contents = contents;
    m_exists = true;
    return true;
  }

  void SetFailRead(bool fail)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failRead = fail;
  }

  void SetFailWrite(bool fail)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failWrite = fail;
  }

  int ReadAttempts() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_readAttempts;
  }

  std::string Contents() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_contents;
  }

private:
  mutable std::mutex m_mutex;
  std::string m_contents;
  bool m_exists{false};
  bool m_failRead{false};
  bool m_failWrite{false};
  int m_readAttempts{0};
  int m_writeAttempts{0};
};

class RuntimeCredentialStore : public IJumpgateCredentialStore
{
public:
  struct Record
  {
    std::string profileId;
    std::string deviceId;
    std::string secretJson;
  };

  bool Store(const std::string& profileId,
             const std::string& deviceId,
             const std::string& secretJson,
             std::string& credentialRef,
             std::string&) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    credentialRef = "jgcred_" + std::string(16, 'x') + std::to_string(++m_sequence);
    m_records[credentialRef] = {profileId, deviceId, secretJson};
    return true;
  }

  bool Load(const std::string& profileId,
            const std::string& deviceId,
            const std::string& credentialRef,
            std::string& secretJson,
            std::string& error) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failLoad)
    {
      error = "simulated credential read failure";
      return false;
    }
    const auto it = m_records.find(credentialRef);
    if (it == m_records.end() || it->second.profileId != profileId ||
        it->second.deviceId != deviceId)
    {
      error = "credential authentication failed";
      return false;
    }
    secretJson = it->second.secretJson;
    return true;
  }

  bool Remove(const std::string& credentialRef, std::string&) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.erase(credentialRef);
    return true;
  }

  size_t Size() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records.size();
  }

  void SetFailLoad(bool fail)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failLoad = fail;
  }

private:
  mutable std::mutex m_mutex;
  int m_sequence{0};
  bool m_failLoad{false};
  std::map<std::string, Record> m_records;
};

CVariant PairingResponse(const std::string& profileId,
                         const std::string& deviceId,
                         const std::string& token,
                         const std::string& config,
                         const std::string& origin = ORIGIN)
{
  CVariant response(CVariant::VariantTypeObject);
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
  settings["auto_update_check"] = true;
  response["settings"] = settings;
  return response;
}

bool StorePair(CJumpgateProfileRuntime& runtime,
               const std::string& profileId,
               const std::string& deviceId,
               const std::string& token,
               const std::string& config,
               int64_t now,
               std::string& error)
{
  return runtime
      .StorePairingResponse(PairingResponse(profileId, deviceId, token, config), ORIGIN, false, now,
                            error)
      .IsCommitted();
}

} // namespace

TEST(TestJumpgateProfileRuntime, InitializeLoadsOnceAndReloadIsExplicit)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;

  ASSERT_TRUE(runtime.Initialize(error)) << error;
  ASSERT_TRUE(runtime.Initialize(error)) << error;
  EXPECT_EQ(storage.ReadAttempts(), 1);
  ASSERT_TRUE(runtime.Reload(error)) << error;
  EXPECT_EQ(storage.ReadAttempts(), 2);
}

TEST(TestJumpgateProfileRuntime, PairingResponseIsBoundToExactCapturedOrigin)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(runtime.Initialize(error)) << error;
  const CVariant response = PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A);

  EXPECT_FALSE(
      runtime.StorePairingResponse(response, "https://alternate.example", false, 1, error));
  EXPECT_FALSE(runtime.StorePairingResponse(response, std::string{ORIGIN} + "/_c/" + CONFIG_A,
                                            false, 2, error));
  EXPECT_TRUE(runtime.GetProfiles().empty());
  EXPECT_EQ(credentials.Size(), 0u);

  ASSERT_TRUE(runtime.StorePairingResponse(response, "HTTPS://BRIDGE.EXAMPLE", false, 3, error))
      << error;
  EXPECT_EQ(credentials.Size(), 1u);
  EXPECT_EQ(runtime.GetActive().bridgeOrigin, ORIGIN);
}

TEST(TestJumpgateProfileRuntime, SnapshotsAreCopiesAndActiveStateIsIsolated)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;

  ActiveProfile active = runtime.GetActive();
  ProfileDocument document = runtime.GetDocument();
  std::vector<ProfileMetadata> profiles = runtime.GetProfiles();
  ASSERT_EQ(profiles.size(), 1u);
  active.deviceToken.assign(43, 'Z');
  document.profiles.clear();
  profiles[0].settings["trakt_enabled"] = false;

  const ActiveProfile unchanged = runtime.GetActive();
  EXPECT_EQ(unchanged.deviceToken, TOKEN_A);
  EXPECT_EQ(runtime.GetDocument().profiles.size(), 1u);
  EXPECT_TRUE(runtime.GetProfiles()[0].settings["trakt_enabled"].asBoolean());

  ASSERT_TRUE(runtime.ClearActive(error)) << error;
  const ActiveProfile cleared = runtime.GetActive();
  EXPECT_FALSE(cleared.selected);
  EXPECT_TRUE(cleared.deviceToken.empty());
  EXPECT_EQ(runtime.GetProfiles().size(), 1u);
}

TEST(TestJumpgateProfileRuntime, FailedMutationPreservesDocumentAndActiveSnapshot)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  const ActiveProfile before = runtime.GetActive();
  const ProfileDocument beforeDocument = runtime.GetDocument();
  const std::string beforeBytes = storage.Contents();

  storage.SetFailWrite(true);
  const ProfileMutationResult result =
      runtime.SetActiveSetting("trakt_enabled", CVariant{false}, error);
  EXPECT_EQ(result.status, ProfileMutationStatus::NotCommitted);
  EXPECT_FALSE(result.IsCommitted());
  const ActiveProfile after = runtime.GetActive();
  const ProfileDocument afterDocument = runtime.GetDocument();
  EXPECT_EQ(after.deviceToken, before.deviceToken);
  EXPECT_EQ(after.traktEnabled, before.traktEnabled);
  EXPECT_EQ(afterDocument.profiles[0].settings, beforeDocument.profiles[0].settings);
  EXPECT_EQ(storage.Contents(), beforeBytes);
}

TEST(TestJumpgateProfileRuntime, PairAndSelectFailuresReportNotCommitted)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(runtime.Initialize(error)) << error;

  const ProfileMutationResult invalidPair =
      runtime.StorePairingResponse(PairingResponse(PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A),
                                   "https://wrong.example", false, 1, error);
  EXPECT_EQ(invalidPair.status, ProfileMutationStatus::NotCommitted);
  EXPECT_TRUE(runtime.GetProfiles().empty());

  const ProfileMutationResult missingSelect = runtime.SelectActive(PROFILE_A, error);
  EXPECT_EQ(missingSelect.status, ProfileMutationStatus::NotCommitted);
  EXPECT_FALSE(runtime.GetActive().selected);
}

TEST(TestJumpgateProfileRuntime, ReloadFailureClearsCachedActiveSecrets)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  ASSERT_FALSE(runtime.GetActive().deviceToken.empty());

  storage.SetFailRead(true);
  EXPECT_FALSE(runtime.Reload(error));
  EXPECT_FALSE(runtime.IsInitialized());
  EXPECT_TRUE(runtime.GetActive().deviceToken.empty());
}

TEST(TestJumpgateProfileRuntime, MutationsRefreshActiveAndMetadataSnapshots)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B, 2, error)) << error;

  ASSERT_TRUE(runtime.SelectActive(PROFILE_A, error)) << error;
  EXPECT_EQ(runtime.GetActive().deviceToken, TOKEN_A);
  ASSERT_TRUE(runtime.SetActiveSetting("trakt_enabled", CVariant{false}, error)) << error;
  EXPECT_FALSE(runtime.GetActive().traktEnabled);
  ASSERT_TRUE(runtime.SetPairingOrigin("https://pairing.example", false, error)) << error;
  EXPECT_EQ(runtime.GetPairingOrigin(), "https://pairing.example");

  ASSERT_TRUE(runtime.ForgetLocal(PROFILE_A, DEVICE_A, error).IsFullyApplied()) << error;
  EXPECT_FALSE(runtime.GetActive().selected);
  const std::vector<ProfileMetadata> profiles = runtime.GetProfiles();
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].profileId, PROFILE_B);
  EXPECT_EQ(credentials.Size(), 1u);
}

TEST(TestJumpgateProfileRuntime, ForgetReportsCommittedWhenPostCommitRefreshFails)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B, 2, error)) << error;
  ASSERT_TRUE(runtime.SelectActive(PROFILE_A, error)) << error;

  credentials.SetFailLoad(true);
  const ForgetLocalResult result = runtime.ForgetLocal(PROFILE_B, DEVICE_B, error);
  EXPECT_EQ(result.status, ForgetLocalStatus::CommittedRefreshFailed);
  EXPECT_TRUE(result.IsCommitted());
  EXPECT_FALSE(result.IsFullyApplied());
  EXPECT_EQ(error, "simulated credential read failure");
  EXPECT_TRUE(runtime.GetActive().deviceToken.empty());
  ASSERT_EQ(runtime.GetProfiles().size(), 1u);
  EXPECT_EQ(runtime.GetProfiles().front().profileId, PROFILE_A);

  credentials.SetFailLoad(false);
  ASSERT_TRUE(runtime.Reload(error)) << error;
  EXPECT_EQ(runtime.GetActive().profileId, PROFILE_A);
}

TEST(TestJumpgateProfileRuntime, EveryMutationReportsCommittedRefreshFailure)
{
  const auto expectRefreshFailure = [](auto mutation)
  {
    RuntimeProfileStorage storage;
    RuntimeCredentialStore credentials;
    CJumpgateProfileRuntime runtime(storage, credentials);
    std::string error;
    ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
    ASSERT_TRUE(StorePair(runtime, PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B, 2, error)) << error;
    credentials.SetFailLoad(true);

    const ProfileMutationResult result = mutation(runtime, error);
    EXPECT_EQ(result.status, ProfileMutationStatus::CommittedRefreshFailed);
    EXPECT_TRUE(result.IsCommitted());
    EXPECT_FALSE(result.IsFullyApplied());
    EXPECT_EQ(error, "simulated credential read failure");
  };

  expectRefreshFailure([](auto& runtime, auto& error)
                       { return runtime.SelectActive(PROFILE_A, error); });
  expectRefreshFailure(
      [](auto& runtime, auto& error)
      { return runtime.SetActiveSetting("trakt_enabled", CVariant{false}, error); });
  expectRefreshFailure(
      [](auto& runtime, auto& error)
      { return runtime.SetPairingOrigin("https://pairing.example", false, error); });
}

TEST(TestJumpgateProfileRuntime, ClearActiveReturnsStructuredCommittedOutcome)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;

  const ProfileMutationResult result = runtime.ClearActive(error);
  EXPECT_EQ(result.status, ProfileMutationStatus::Committed);
  EXPECT_TRUE(result.IsFullyApplied());
  EXPECT_FALSE(runtime.GetActive().selected);
}

TEST(TestJumpgateProfileRuntime, PairingReportsCommittedRefreshFailure)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  credentials.SetFailLoad(true);

  const ProfileMutationResult result = runtime.StorePairingResponse(
      PairingResponse(PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B), ORIGIN, false, 2, error);
  EXPECT_EQ(result.status, ProfileMutationStatus::CommittedRefreshFailed);
  EXPECT_TRUE(result.IsCommitted());
  ASSERT_EQ(runtime.GetProfiles().size(), 2u);
  EXPECT_EQ(runtime.GetDocument().activeProfileId, PROFILE_B);
}

TEST(TestJumpgateProfileRuntime, ConcurrentReadsAndMutationsRemainConsistent)
{
  RuntimeProfileStorage storage;
  RuntimeCredentialStore credentials;
  CJumpgateProfileRuntime runtime(storage, credentials);
  std::string error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_A, DEVICE_A, TOKEN_A, CONFIG_A, 1, error)) << error;
  ASSERT_TRUE(StorePair(runtime, PROFILE_B, DEVICE_B, TOKEN_B, CONFIG_B, 2, error)) << error;

  std::atomic<bool> valid{true};
  std::thread writer(
      [&]()
      {
        for (int i = 0; i < 80; ++i)
        {
          std::string mutationError;
          const std::string profileId = (i % 2 == 0) ? PROFILE_A : PROFILE_B;
          if (!runtime.SelectActive(profileId, mutationError) ||
              !runtime.SetActiveSetting("subtitles_enabled", CVariant{i % 3 != 0}, mutationError))
          {
            valid = false;
            return;
          }
        }
      });

  std::vector<std::thread> readers;
  for (int reader = 0; reader < 4; ++reader)
  {
    readers.emplace_back(
        [&]()
        {
          for (int i = 0; i < 300; ++i)
          {
            const ActiveProfile active = runtime.GetActive();
            if (!active.selected ||
                (active.profileId == PROFILE_A && active.deviceToken != TOKEN_A) ||
                (active.profileId == PROFILE_B && active.deviceToken != TOKEN_B) ||
                (active.profileId != PROFILE_A && active.profileId != PROFILE_B) ||
                runtime.GetPairingOrigin() != "https://jumpgate-bridge.fly.dev")
            {
              valid = false;
              return;
            }
            const std::vector<ProfileMetadata> profiles = runtime.GetProfiles();
            if (profiles.size() != 2u ||
                std::count_if(profiles.begin(), profiles.end(),
                              [](const ProfileMetadata& profile) { return profile.active; }) != 1)
            {
              valid = false;
              return;
            }
          }
        });
  }

  writer.join();
  for (std::thread& reader : readers)
    reader.join();
  EXPECT_TRUE(valid.load());
}
