/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/JumpgatePlaybackHistory.h"
#include "utils/JumpgateProfileHistoryPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* PROFILE_A = "profile_history_a";
constexpr const char* PROFILE_B = "profile_history_b";

class FakeHistoryStorage final : public IJumpgateProfileStorage
{
public:
  bool Read(std::string& contents, bool& exists, std::string& error) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++readAttempts;
    condition.notify_all();
    if (failRead)
    {
      error = "simulated read failure";
      return false;
    }
    contents = bytes;
    exists = hasContents;
    return true;
  }

  bool WriteAtomic(const std::string& contents, std::string& error) override
  {
    std::unique_lock<std::mutex> lock(mutex);
    ++writeAttempts;
    if (blockNextWrite)
    {
      blockNextWrite = false;
      writeBlocked = true;
      condition.notify_all();
      condition.wait(lock, [this] { return releaseWrite; });
      releaseWrite = false;
      writeBlocked = false;
    }
    if (failWrite || (failWriteAttempt > 0 && writeAttempts == failWriteAttempt))
    {
      error = "simulated atomic replace failure";
      return false;
    }
    bytes = contents;
    hasContents = true;
    return true;
  }

  void BlockNextWrite()
  {
    std::lock_guard<std::mutex> lock(mutex);
    blockNextWrite = true;
    writeBlocked = false;
    releaseWrite = false;
  }

  void WaitForBlockedWrite()
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this] { return writeBlocked; });
  }

  void ReleaseWrite()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      releaseWrite = true;
    }
    condition.notify_all();
  }

  int ReadAttempts()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return readAttempts;
  }

  bool WaitForAdditionalRead(int previousAttempts)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::milliseconds(50),
                              [this, previousAttempts] { return readAttempts > previousAttempts; });
  }

  bool hasContents{false};
  bool failRead{false};
  bool failWrite{false};
  int failWriteAttempt{0};
  int readAttempts{0};
  int writeAttempts{0};
  std::string bytes;

private:
  std::mutex mutex;
  std::condition_variable condition;
  bool blockNextWrite{false};
  bool writeBlocked{false};
  bool releaseWrite{false};
};

std::string Key(uint64_t value)
{
  constexpr char HEX[] = "0123456789abcdef";
  std::string key(64, '0');
  for (std::size_t index = 0; index < 16; ++index)
  {
    key[key.size() - 1 - index] = HEX[value & 0xf];
    value >>= 4;
  }
  return key;
}

JumpgatePlaybackHistoryEntry Entry(const std::string& profileId,
                                   const std::string& contentKey,
                                   int64_t positionMs = 1000,
                                   int64_t durationMs = 10000,
                                   int64_t updatedAtMs = 1)
{
  JumpgatePlaybackHistoryEntry entry;
  entry.profileId = profileId;
  entry.contentKey = contentKey;
  entry.display.title = "History title";
  entry.positionMs = positionMs;
  entry.durationMs = durationMs;
  entry.updatedAtMs = updatedAtMs;
  return entry;
}

JumpgatePlaybackHistoryEntry LocalEntry(const std::string& contentKey,
                                        int64_t positionMs = 1000,
                                        int64_t durationMs = 10000,
                                        int64_t updatedAtMs = 1)
{
  JumpgatePlaybackHistoryEntry entry = Entry("", contentKey, positionMs, durationMs, updatedAtMs);
  entry.historyNamespace = JumpgatePlaybackHistoryNamespace::LocalSource;
  return entry;
}

JumpgatePlaybackHistoryKey AuthenticatedKey(const std::string& profileId,
                                            const std::string& contentKey)
{
  return {JumpgatePlaybackHistoryNamespace::AuthenticatedProfile, profileId, contentKey};
}

JumpgatePlaybackHistoryKey LocalKey(const std::string& contentKey)
{
  return {JumpgatePlaybackHistoryNamespace::LocalSource, "", contentKey};
}

bool Save(CJumpgatePlaybackHistoryStore& store,
          const std::string& expectedProfileId,
          JumpgatePlaybackHistoryEntry entry,
          std::string& error)
{
  const JumpgatePlaybackHistoryKey expectedKey =
      AuthenticatedKey(expectedProfileId, entry.contentKey);
  return store.Save(expectedKey, std::move(entry), error);
}

JumpgateCanonicalIdentity Identity(JumpgateCanonicalProvider provider,
                                   const std::string& id,
                                   JumpgateMediaType mediaType = JumpgateMediaType::Movie)
{
  JumpgateCanonicalIdentity identity;
  identity.provider = provider;
  identity.id = id;
  identity.mediaType = mediaType;
  if (mediaType == JumpgateMediaType::Episode)
  {
    identity.season = 2;
    identity.episode = 3;
  }
  return identity;
}

CVariant ParseJson(const std::string& json)
{
  CVariant value;
  EXPECT_TRUE(CJSONVariantParser::Parse(json, value));
  return value;
}

std::string WriteJson(const CVariant& value)
{
  std::string json;
  EXPECT_TRUE(CJSONVariantWriter::Write(value, json, true));
  return json;
}

std::string NearLimitLegacyHistory(const std::string& entryProfileId,
                                   const std::vector<std::string>& blockedProfiles = {},
                                   const std::vector<std::string>& forgottenProfiles = {})
{
  JumpgatePlaybackHistoryDocument document;
  document.blockedProfiles = blockedProfiles;
  document.forgottenProfiles = forgottenProfiles;
  for (uint64_t index = 0; index < 64; ++index)
  {
    auto entry = Entry(entryProfileId, Key(index), 1, 1000, static_cast<int64_t>(index + 1));
    entry.display.title = std::string(7000, 'x');
    document.entries.emplace_back(std::move(entry));
  }

  std::string current;
  std::string error;
  if (!SerializeJumpgatePlaybackHistory(document, current, error))
  {
    ADD_FAILURE() << error;
    return {};
  }

  CVariant legacy = ParseJson(current);
  legacy["schemaVersion"] = 1;
  for (std::size_t index = 0; index < legacy["entries"].size(); ++index)
    legacy["entries"][index].erase("namespace");

  std::string fullest = WriteJson(legacy);
  for (std::size_t index = 0; index < legacy["entries"].size(); ++index)
  {
    const std::size_t originalLength =
        legacy["entries"][index]["display"]["title"].asString().size();
    legacy["entries"][index]["display"]["title"] = std::string(8192, 'x');
    std::string candidate = WriteJson(legacy);
    if (candidate.size() <= JUMPGATE_HISTORY_MAX_BYTES)
    {
      fullest = std::move(candidate);
      continue;
    }

    std::size_t low = originalLength;
    std::size_t high = 8192;
    std::size_t best = originalLength;
    while (low <= high)
    {
      const std::size_t candidateLength = low + (high - low) / 2;
      legacy["entries"][index]["display"]["title"] = std::string(candidateLength, 'x');
      candidate = WriteJson(legacy);
      if (candidate.size() <= JUMPGATE_HISTORY_MAX_BYTES)
      {
        best = candidateLength;
        fullest = std::move(candidate);
        low = candidateLength + 1;
      }
      else
      {
        high = candidateLength - 1;
      }
    }
    legacy["entries"][index]["display"]["title"] = std::string(best, 'x');
    break;
  }

  JumpgatePlaybackHistoryDocument parsed;
  EXPECT_TRUE(ParseJumpgatePlaybackHistory(fullest, parsed, error)) << error;
  EXPECT_TRUE(parsed.loadedFromLegacySchema);
  EXPECT_GT(fullest.size(), JUMPGATE_HISTORY_MAX_BYTES - 128);
  return fullest;
}

std::string NearLimitHistory(JumpgatePlaybackHistoryDocument document, bool legacy)
{
  std::string serialized;
  std::string error;
  if (!SerializeJumpgatePlaybackHistory(document, serialized, error))
  {
    ADD_FAILURE() << error;
    return {};
  }

  CVariant history = ParseJson(serialized);
  if (legacy)
  {
    history["schemaVersion"] = 1;
    for (std::size_t index = 0; index < history["entries"].size(); ++index)
      history["entries"][index].erase("namespace");
  }

  std::string fullest = WriteJson(history);
  if (fullest.size() > JUMPGATE_HISTORY_MAX_BYTES)
  {
    ADD_FAILURE() << "near-limit history fixture starts above the size limit";
    return {};
  }

  for (std::size_t index = 0; index < history["entries"].size(); ++index)
  {
    const std::size_t originalLength =
        history["entries"][index]["display"]["title"].asString().size();
    history["entries"][index]["display"]["title"] = std::string(8192, 'x');
    std::string candidate = WriteJson(history);
    if (candidate.size() <= JUMPGATE_HISTORY_MAX_BYTES)
    {
      fullest = std::move(candidate);
      continue;
    }

    std::size_t low = originalLength;
    std::size_t high = 8192;
    std::size_t best = originalLength;
    while (low <= high)
    {
      const std::size_t candidateLength = low + (high - low) / 2;
      history["entries"][index]["display"]["title"] = std::string(candidateLength, 'x');
      candidate = WriteJson(history);
      if (candidate.size() <= JUMPGATE_HISTORY_MAX_BYTES)
      {
        best = candidateLength;
        fullest = std::move(candidate);
        low = candidateLength + 1;
      }
      else
      {
        high = candidateLength - 1;
      }
    }
    history["entries"][index]["display"]["title"] = std::string(best, 'x');
    break;
  }

  JumpgatePlaybackHistoryDocument parsed;
  EXPECT_TRUE(ParseJumpgatePlaybackHistory(fullest, parsed, error)) << error;
  EXPECT_EQ(parsed.loadedFromLegacySchema, legacy);
  EXPECT_EQ(fullest.size(), JUMPGATE_HISTORY_MAX_BYTES);
  return fullest;
}

void ExpectSameHistoryEntries(const std::vector<JumpgatePlaybackHistoryEntry>& expected,
                              const std::vector<JumpgatePlaybackHistoryEntry>& actual);

void ExpectNearLimitPurgePreservesUnaffectedHistory(bool legacy)
{
  JumpgatePlaybackHistoryDocument document;
  document.blockedProfiles.emplace_back(PROFILE_A);
  for (uint64_t index = 0; index < 64; ++index)
  {
    // Unaffected history is oldest so a global-oldest eviction would remove it first.
    JumpgatePlaybackHistoryEntry entry;
    if (!legacy && index == 0)
      entry = LocalEntry(Key(index), 1, 1000, 1);
    else if (index == (legacy ? 0u : 1u))
      entry = Entry(PROFILE_B, Key(index), 2, 1000, 2);
    else
      entry = Entry(PROFILE_A, Key(index), 3, 1000, static_cast<int64_t>(index + 3));
    entry.display.title = std::string(7000, 'x');
    document.entries.emplace_back(std::move(entry));
  }

  const std::string committed = NearLimitHistory(std::move(document), legacy);
  ASSERT_FALSE(committed.empty());
  ASSERT_EQ(committed.size(), JUMPGATE_HISTORY_MAX_BYTES);

  std::string error;
  JumpgatePlaybackHistoryDocument expected;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(committed, expected, error)) << error;
  ASSERT_EQ(expected.loadedFromLegacySchema, legacy);
  ASSERT_EQ(expected.entries.size(), 64u);

  std::vector<JumpgatePlaybackHistoryEntry> unaffected;
  std::copy_if(expected.entries.begin(), expected.entries.end(), std::back_inserter(unaffected),
               [](const auto& entry)
               {
                 return entry.historyNamespace == JumpgatePlaybackHistoryNamespace::LocalSource ||
                        entry.profileId != PROFILE_A;
               });
  ASSERT_EQ(unaffected.size(), legacy ? 1u : 2u);

  CVariant oversized = ParseJson(committed);
  oversized["forgottenProfiles"].push_back(PROFILE_A);
  ASSERT_GT(WriteJson(oversized).size(), JUMPGATE_HISTORY_MAX_BYTES);

  FakeHistoryStorage storage;
  storage.bytes = committed;
  storage.hasContents = true;
  CJumpgatePlaybackHistoryStore store(storage);
  ASSERT_TRUE(store.PurgeBlockedProfile(PROFILE_A, error)) << error;
  EXPECT_EQ(storage.writeAttempts, 2);

  JumpgatePlaybackHistoryDocument purged;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, purged, error)) << error;
  ExpectSameHistoryEntries(unaffected, purged.entries);
  EXPECT_EQ(purged.entries.size(), unaffected.size());
  EXPECT_EQ(std::count_if(purged.entries.begin(), purged.entries.end(), [](const auto& entry)
                          {
                            return entry.historyNamespace ==
                                       JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                                   entry.profileId == PROFILE_B;
                          }),
            1);
  EXPECT_EQ(std::count_if(purged.entries.begin(), purged.entries.end(), [](const auto& entry)
                          {
                            return entry.historyNamespace ==
                                   JumpgatePlaybackHistoryNamespace::LocalSource;
                          }),
            legacy ? 0 : 1);
  EXPECT_NE(std::find(purged.blockedProfiles.begin(), purged.blockedProfiles.end(), PROFILE_A),
            purged.blockedProfiles.end());
  EXPECT_NE(std::find(purged.forgottenProfiles.begin(), purged.forgottenProfiles.end(), PROFILE_A),
            purged.forgottenProfiles.end());
}

void ExpectSameHistoryEntries(const std::vector<JumpgatePlaybackHistoryEntry>& expected,
                              const std::vector<JumpgatePlaybackHistoryEntry>& actual)
{
  ASSERT_EQ(actual.size(), expected.size());
  for (const auto& expectedEntry : expected)
  {
    const auto actualEntry = std::find_if(
        actual.begin(), actual.end(), [&expectedEntry](const auto& candidate)
        {
          return candidate.historyNamespace == expectedEntry.historyNamespace &&
                 candidate.profileId == expectedEntry.profileId &&
                 candidate.contentKey == expectedEntry.contentKey;
        });
    ASSERT_NE(actualEntry, actual.end()) << expectedEntry.contentKey;
    ASSERT_EQ(actualEntry->canonicalIdentity.has_value(),
              expectedEntry.canonicalIdentity.has_value());
    if (expectedEntry.canonicalIdentity)
    {
      EXPECT_EQ(actualEntry->canonicalIdentity->provider,
                expectedEntry.canonicalIdentity->provider);
      EXPECT_EQ(actualEntry->canonicalIdentity->id, expectedEntry.canonicalIdentity->id);
      EXPECT_EQ(actualEntry->canonicalIdentity->mediaType,
                expectedEntry.canonicalIdentity->mediaType);
      EXPECT_EQ(actualEntry->canonicalIdentity->season, expectedEntry.canonicalIdentity->season);
      EXPECT_EQ(actualEntry->canonicalIdentity->episode, expectedEntry.canonicalIdentity->episode);
    }
    EXPECT_EQ(actualEntry->display.title, expectedEntry.display.title);
    EXPECT_EQ(actualEntry->display.year, expectedEntry.display.year);
    EXPECT_EQ(actualEntry->display.season, expectedEntry.display.season);
    EXPECT_EQ(actualEntry->display.episode, expectedEntry.display.episode);
    EXPECT_EQ(actualEntry->positionMs, expectedEntry.positionMs);
    EXPECT_EQ(actualEntry->durationMs, expectedEntry.durationMs);
    EXPECT_EQ(actualEntry->completed, expectedEntry.completed);
    EXPECT_EQ(actualEntry->watched, expectedEntry.watched);
    EXPECT_EQ(actualEntry->updatedAtMs, expectedEntry.updatedAtMs);
  }
}

std::optional<JumpgatePlaybackHistoryEntry> Get(CJumpgatePlaybackHistoryStore& store,
                                                const std::string& profileId,
                                                const std::string& contentKey)
{
  std::optional<JumpgatePlaybackHistoryEntry> entry;
  std::string error;
  EXPECT_TRUE(store.Get(AuthenticatedKey(profileId, contentKey), entry, error)) << error;
  return entry;
}

} // namespace

TEST(TestJumpgatePlaybackHistory, RoundTripsEveryCanonicalProviderAndMediaTypeLosslessly)
{
  struct ProviderCase
  {
    JumpgateCanonicalProvider provider;
    const char* id;
  };
  constexpr std::array<ProviderCase, 4> cases = {
      ProviderCase{JumpgateCanonicalProvider::Imdb, "tt0133093"},
      ProviderCase{JumpgateCanonicalProvider::Tmdb, "603"},
      ProviderCase{JumpgateCanonicalProvider::Tvdb, "169"},
      ProviderCase{JumpgateCanonicalProvider::Trakt, "603"},
  };

  JumpgatePlaybackHistoryDocument document;
  uint64_t key = 1;
  for (const ProviderCase& provider : cases)
  {
    auto movie = Entry(PROFILE_A, Key(key++));
    movie.canonicalIdentity = Identity(provider.provider, provider.id);
    document.entries.emplace_back(std::move(movie));

    auto episode = Entry(PROFILE_A, Key(key++));
    episode.canonicalIdentity =
        Identity(provider.provider, provider.id, JumpgateMediaType::Episode);
    episode.display.season = 2;
    episode.display.episode = 3;
    document.entries.emplace_back(std::move(episode));
  }

  std::string json;
  std::string error;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;
  JumpgatePlaybackHistoryDocument parsed;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(json, parsed, error)) << error;
  ASSERT_EQ(parsed.entries.size(), document.entries.size());
  for (std::size_t index = 0; index < parsed.entries.size(); ++index)
  {
    ASSERT_TRUE(parsed.entries[index].canonicalIdentity);
    ASSERT_TRUE(document.entries[index].canonicalIdentity);
    EXPECT_EQ(parsed.entries[index].canonicalIdentity->provider,
              document.entries[index].canonicalIdentity->provider);
    EXPECT_EQ(parsed.entries[index].canonicalIdentity->id,
              document.entries[index].canonicalIdentity->id);
    EXPECT_EQ(parsed.entries[index].canonicalIdentity->mediaType,
              document.entries[index].canonicalIdentity->mediaType);
    EXPECT_EQ(parsed.entries[index].canonicalIdentity->season,
              document.entries[index].canonicalIdentity->season);
    EXPECT_EQ(parsed.entries[index].canonicalIdentity->episode,
              document.entries[index].canonicalIdentity->episode);
  }
}

TEST(TestJumpgatePlaybackHistory, NamespacesDoNotCollideAndProfileClearPreservesDeviceLocalHistory)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string sharedKey = Key(77);

  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, sharedKey, 111, 1000, 1), error)) << error;
  auto local = LocalEntry(sharedKey, 222, 1000, 2);
  ASSERT_TRUE(store.Save(LocalKey(sharedKey), local, error)) << error;

  const auto authenticated = Get(store, PROFILE_A, sharedKey);
  std::optional<JumpgatePlaybackHistoryEntry> deviceLocal;
  ASSERT_TRUE(store.Get(LocalKey(sharedKey), deviceLocal, error)) << error;
  ASSERT_TRUE(authenticated);
  ASSERT_TRUE(deviceLocal);
  EXPECT_EQ(authenticated->historyNamespace,
            JumpgatePlaybackHistoryNamespace::AuthenticatedProfile);
  EXPECT_EQ(deviceLocal->historyNamespace, JumpgatePlaybackHistoryNamespace::LocalSource);
  EXPECT_EQ(authenticated->positionMs, 111);
  EXPECT_EQ(deviceLocal->positionMs, 222);
  EXPECT_TRUE(deviceLocal->profileId.empty());
  EXPECT_FALSE(deviceLocal->canonicalIdentity);

  ASSERT_TRUE(store.ClearProfile(PROFILE_A, error)) << error;
  EXPECT_FALSE(Get(store, PROFILE_A, sharedKey));
  ASSERT_TRUE(store.Get(LocalKey(sharedKey), deviceLocal, error)) << error;
  ASSERT_TRUE(deviceLocal);
  EXPECT_EQ(deviceLocal->positionMs, 222);

  local.positionMs = 333;
  local.updatedAtMs = 3;
  ASSERT_TRUE(store.Save(LocalKey(sharedKey), local, error)) << error;
  ASSERT_TRUE(store.Get(LocalKey(sharedKey), deviceLocal, error)) << error;
  ASSERT_TRUE(deviceLocal);
  EXPECT_EQ(deviceLocal->positionMs, 333);

  local.canonicalIdentity = Identity(JumpgateCanonicalProvider::Imdb, "tt0133093");
  EXPECT_FALSE(store.Save(LocalKey(sharedKey), local, error));
}

TEST(TestJumpgatePlaybackHistory, LegacyEntriesMigrateToAuthenticatedProfileNamespace)
{
  JumpgatePlaybackHistoryDocument document;
  document.entries.emplace_back(Entry(PROFILE_A, Key(1)));
  std::string json;
  std::string error;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;

  CVariant legacy = ParseJson(json);
  legacy["schemaVersion"] = 1;
  legacy["entries"][0].erase("namespace");
  const std::string legacyJson = WriteJson(legacy);

  JumpgatePlaybackHistoryDocument parsed;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(legacyJson, parsed, error)) << error;
  ASSERT_EQ(parsed.entries.size(), 1u);
  EXPECT_EQ(parsed.entries[0].historyNamespace,
            JumpgatePlaybackHistoryNamespace::AuthenticatedProfile);
  EXPECT_EQ(parsed.entries[0].profileId, PROFILE_A);

  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(parsed, json, error)) << error;
  const CVariant migrated = ParseJson(json);
  EXPECT_EQ(migrated["schemaVersion"].asInteger(), 2);
  EXPECT_EQ(migrated["entries"][0]["namespace"].asString(), "authenticated_profile");
  EXPECT_EQ(migrated["entries"][0]["profileId"].asString(), PROFILE_A);
}

TEST(TestJumpgatePlaybackHistory,
     LocalSourceKeysAreCanonicalLengthPrefixedDomainSeparatedAndSecretFree)
{
  const std::vector<std::string> fingerprints{"v1:url:sha256:" + std::string(64, 'a'),
                                              "v1:info-hash:sha256:" + std::string(64, 'b')};
  const auto key = DeriveJumpgateLocalSourceHistoryKey(fingerprints);
  const auto reordered =
      DeriveJumpgateLocalSourceHistoryKey({fingerprints[1], fingerprints[0], fingerprints[0]});
  const auto leftSplit = DeriveJumpgateLocalSourceHistoryKey({"v1:ab", "v1:c"});
  const auto rightSplit = DeriveJumpgateLocalSourceHistoryKey({"v1:a", "v1:bc"});
  ASSERT_TRUE(key);
  ASSERT_TRUE(reordered);
  ASSERT_TRUE(leftSplit);
  ASSERT_TRUE(rightSplit);
  EXPECT_EQ(*key, *reordered);
  EXPECT_NE(*leftSplit, *rightSplit);
  EXPECT_TRUE(IsValidJumpgateHistoryContentKey(*key));

  const std::string rawLaunchUri =
      "https://provider.example/private/video.m3u8?token=must-not-persist";
  const std::string fallback = DeriveJumpgateLocalSourceFallbackHistoryKey(rawLaunchUri);
  const auto rawAsFingerprint = DeriveJumpgateLocalSourceHistoryKey({rawLaunchUri});
  ASSERT_TRUE(rawAsFingerprint);
  EXPECT_TRUE(IsValidJumpgateHistoryContentKey(fallback));
  EXPECT_NE(fallback, *rawAsFingerprint);

  JumpgatePlaybackHistoryDocument document;
  document.entries.emplace_back(LocalEntry(fallback));
  std::string json;
  std::string error;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;
  EXPECT_EQ(json.find(rawLaunchUri), std::string::npos);
  EXPECT_EQ(json.find("must-not-persist"), std::string::npos);
  const CVariant serialized = ParseJson(json);
  EXPECT_EQ(serialized["entries"][0]["namespace"].asString(), "local_source");
  EXPECT_TRUE(serialized["entries"][0]["profileId"].isNull());
  EXPECT_TRUE(serialized["entries"][0]["canonicalIdentity"].isNull());
}

TEST(TestJumpgatePlaybackHistory,
     IsolatesProfilesWithTheSameContentKeyAndClearsOnlyForgottenProfile)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(7);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 111, 1000, 1), error)) << error;
  ASSERT_TRUE(Save(store, PROFILE_B, Entry(PROFILE_B, key, 222, 1000, 2), error)) << error;

  ASSERT_TRUE(Get(store, PROFILE_A, key));
  EXPECT_EQ(Get(store, PROFILE_A, key)->positionMs, 111);
  ASSERT_TRUE(Get(store, PROFILE_B, key));
  EXPECT_EQ(Get(store, PROFILE_B, key)->positionMs, 222);

  ASSERT_TRUE(store.ClearProfile(PROFILE_A, error)) << error;
  EXPECT_FALSE(Get(store, PROFILE_A, key));
  ASSERT_TRUE(Get(store, PROFILE_B, key));
  EXPECT_EQ(Get(store, PROFILE_B, key)->positionMs, 222);
}

TEST(TestJumpgatePlaybackHistory, RejectsProfileMismatchMalformedUnknownAndSecretFields)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  EXPECT_FALSE(Save(store, PROFILE_A, Entry(PROFILE_B, Key(1)), error));
  EXPECT_FALSE(Save(store, " profile", Entry(" profile", Key(1)), error));

  JumpgatePlaybackHistoryDocument document;
  document.entries.emplace_back(Entry(PROFILE_A, Key(1)));
  std::string json;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;

  std::array<CVariant, 8> malformed;
  malformed[0] = ParseJson(json);
  malformed[0]["unknown"] = true;
  malformed[1] = ParseJson(json);
  malformed[1]["entries"][0]["deviceToken"] = "secret";
  malformed[2] = ParseJson(json);
  malformed[2]["entries"][0]["display"]["providerUrl"] = "https://provider.example";
  malformed[3] = ParseJson(json);
  malformed[3]["entries"][0]["positionMs"] = -1;
  malformed[4] = ParseJson(json);
  malformed[4]["entries"][0]["contentKey"] = std::string(64, 'A');
  malformed[5] = ParseJson(json);
  malformed[5]["entries"][0].erase("namespace");
  malformed[6] = ParseJson(json);
  malformed[6]["entries"][0]["namespace"] = "local_source";
  malformed[7] = ParseJson(json);
  malformed[7]["entries"][0]["namespace"] = "profile_or_local";
  for (const CVariant& value : malformed)
  {
    JumpgatePlaybackHistoryDocument parsed;
    EXPECT_FALSE(ParseJumpgatePlaybackHistory(WriteJson(value), parsed, error));
  }

  JumpgatePlaybackHistoryDocument parsed;
  CVariant unsafeInteger = ParseJson(json);
  unsafeInteger["entries"][0]["positionMs"] = std::numeric_limits<uint64_t>::max();
  EXPECT_FALSE(ParseJumpgatePlaybackHistory(WriteJson(unsafeInteger), parsed, error));
  CVariant duplicate = ParseJson(json);
  duplicate["entries"].push_back(duplicate["entries"][0]);
  EXPECT_FALSE(ParseJumpgatePlaybackHistory(WriteJson(duplicate), parsed, error));
  CVariant tooMany = ParseJson(json);
  for (uint64_t index = 1; index <= JUMPGATE_HISTORY_MAX_ENTRIES; ++index)
  {
    CVariant item = tooMany["entries"][0];
    item["contentKey"] = Key(index);
    tooMany["entries"].push_back(std::move(item));
  }
  EXPECT_FALSE(ParseJumpgatePlaybackHistory(WriteJson(tooMany), parsed, error));
  EXPECT_FALSE(ParseJumpgatePlaybackHistory(std::string(JUMPGATE_HISTORY_MAX_BYTES + 1, 'x'),
                                            parsed, error));
}

TEST(TestJumpgatePlaybackHistory, AppliesCompletionAndWatchedThresholdsWithoutOverflow)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;

  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 799, 1000, 1), error)) << error;
  auto belowWatched = Get(store, PROFILE_A, Key(1));
  ASSERT_TRUE(belowWatched);
  EXPECT_FALSE(belowWatched->watched);
  EXPECT_FALSE(belowWatched->completed);

  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(2), 800, 1000, 2), error)) << error;
  auto watched = Get(store, PROFILE_A, Key(2));
  ASSERT_TRUE(watched);
  EXPECT_TRUE(watched->watched);
  EXPECT_FALSE(watched->completed);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*watched), 800);

  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(3), 900, 1000, 3), error)) << error;
  auto completed = Get(store, PROFILE_A, Key(3));
  ASSERT_TRUE(completed);
  EXPECT_TRUE(completed->watched);
  EXPECT_TRUE(completed->completed);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*completed), 0);

  auto explicitEnd = Entry(PROFILE_A, Key(4), 100, 1000, 4);
  explicitEnd.completed = true;
  ASSERT_TRUE(Save(store, PROFILE_A, explicitEnd, error)) << error;
  auto ended = Get(store, PROFILE_A, Key(4));
  ASSERT_TRUE(ended);
  EXPECT_TRUE(ended->completed);
  EXPECT_FALSE(ended->watched);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*ended), 0);

  const int64_t maximum = std::numeric_limits<int64_t>::max();
  EXPECT_TRUE(IsJumpgatePlaybackThresholdReached(maximum, maximum, 90));
  EXPECT_FALSE(IsJumpgatePlaybackThresholdReached(maximum / 2, maximum, 80));
}

TEST(TestJumpgatePlaybackHistory, PreservesInt64ProgressAndSupportsRewatch)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const int64_t position = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1234567;
  const int64_t duration = position * 2;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), position, duration, 10), error))
      << error;
  auto saved = Get(store, PROFILE_A, Key(1));
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->positionMs, position);
  EXPECT_EQ(saved->durationMs, duration);

  auto completed = Entry(PROFILE_A, Key(2), 900, 1000, 20);
  ASSERT_TRUE(Save(store, PROFILE_A, completed, error)) << error;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(2), 10, 1000, 21), error)) << error;
  saved = Get(store, PROFILE_A, Key(2));
  ASSERT_TRUE(saved);
  EXPECT_FALSE(saved->completed);
  EXPECT_TRUE(saved->watched);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*saved), 10);

  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(2), 0, 0, 22), error)) << error;
  saved = Get(store, PROFILE_A, Key(2));
  ASSERT_TRUE(saved);
  EXPECT_FALSE(saved->completed);
  EXPECT_TRUE(saved->watched);
  EXPECT_EQ(saved->positionMs, 10);
  EXPECT_EQ(saved->durationMs, 1000);
}

TEST(TestJumpgatePlaybackHistory, FailedOpenClockPreservesCompletionAndWatched)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 900, 1000, 10), error)) << error;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 0, 0, 11), error)) << error;
  const auto saved = Get(store, PROFILE_A, Key(1));
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved->completed);
  EXPECT_TRUE(saved->watched);
  EXPECT_EQ(saved->positionMs, 900);
  EXPECT_EQ(saved->durationMs, 1000);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*saved), 0);
}

TEST(TestJumpgatePlaybackHistory, RejectsNonCanonicalNumericProviderIds)
{
  for (JumpgateCanonicalProvider provider :
       {JumpgateCanonicalProvider::Tmdb, JumpgateCanonicalProvider::Tvdb,
        JumpgateCanonicalProvider::Trakt})
  {
    EXPECT_FALSE(IsValidJumpgateHistoryCanonicalIdentity(Identity(provider, "slug")));
    EXPECT_FALSE(IsValidJumpgateHistoryCanonicalIdentity(Identity(provider, "0")));
    EXPECT_FALSE(IsValidJumpgateHistoryCanonicalIdentity(Identity(provider, "001")));
    EXPECT_FALSE(
        IsValidJumpgateHistoryCanonicalIdentity(Identity(provider, "9223372036854775808")));
    EXPECT_TRUE(IsValidJumpgateHistoryCanonicalIdentity(Identity(provider, "1")));
  }
}

TEST(TestJumpgatePlaybackHistory, SerializesConcurrentSavesWithoutLostUpdates)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  storage.BlockNextWrite();
  std::string firstError;
  std::string secondError;
  std::atomic<bool> secondStarted{false};
  bool firstSaved = false;
  bool secondSaved = false;
  std::thread first(
      [&]
      { firstSaved = Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 10, 1000, 1), firstError); });
  storage.WaitForBlockedWrite();
  const int readsBeforeSecond = storage.ReadAttempts();
  std::thread second(
      [&]
      {
        secondStarted.store(true);
        secondSaved = Save(store, PROFILE_A, Entry(PROFILE_A, Key(2), 20, 1000, 2), secondError);
      });
  while (!secondStarted.load())
    std::this_thread::yield();
  EXPECT_FALSE(storage.WaitForAdditionalRead(readsBeforeSecond));
  storage.ReleaseWrite();
  first.join();
  second.join();

  EXPECT_TRUE(firstSaved) << firstError;
  EXPECT_TRUE(secondSaved) << secondError;
  EXPECT_TRUE(Get(store, PROFILE_A, Key(1)));
  EXPECT_TRUE(Get(store, PROFILE_A, Key(2)));
}

TEST(TestJumpgatePlaybackHistory, ClearSerializesAndPreventsForgottenHistoryResurrection)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 10, 1000, 1), error)) << error;
  ASSERT_TRUE(Save(store, PROFILE_B, Entry(PROFILE_B, Key(2), 20, 1000, 2), error)) << error;

  storage.BlockNextWrite();
  std::string clearError;
  std::string staleError;
  bool cleared = false;
  bool staleSaved = true;
  std::atomic<bool> staleStarted{false};
  std::thread clearThread([&] { cleared = store.ClearProfile(PROFILE_A, clearError); });
  storage.WaitForBlockedWrite();
  const int readsBeforeStaleSave = storage.ReadAttempts();
  std::thread staleSaveThread(
      [&]
      {
        staleStarted.store(true);
        staleSaved = Save(store, PROFILE_A, Entry(PROFILE_A, Key(3), 30, 1000, 3), staleError);
      });
  while (!staleStarted.load())
    std::this_thread::yield();
  EXPECT_FALSE(storage.WaitForAdditionalRead(readsBeforeStaleSave));
  storage.ReleaseWrite();
  clearThread.join();
  staleSaveThread.join();

  EXPECT_TRUE(cleared) << clearError;
  EXPECT_FALSE(staleSaved);
  EXPECT_FALSE(Get(store, PROFILE_A, Key(1)));
  EXPECT_FALSE(Get(store, PROFILE_A, Key(3)));
  EXPECT_TRUE(Get(store, PROFILE_B, Key(2)));

  ASSERT_TRUE(store.ResetProfile(PROFILE_A, error)) << error;
  EXPECT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(4), 40, 1000, 4), error)) << error;
  EXPECT_TRUE(Get(store, PROFILE_A, Key(4)));
}

TEST(TestJumpgatePlaybackHistory, PrunesOldestEntryDeterministically)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  for (uint64_t index = 0; index < JUMPGATE_HISTORY_MAX_ENTRIES; ++index)
  {
    ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(index), 1, 1000, 10), error))
        << index << ": " << error;
  }
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(999), 1, 1000, 11), error)) << error;
  ASSERT_TRUE(store.Save(LocalKey(Key(999)), LocalEntry(Key(999), 2, 1000, 12), error)) << error;
  EXPECT_FALSE(Get(store, PROFILE_A, Key(0)));
  EXPECT_FALSE(Get(store, PROFILE_A, Key(1)));
  EXPECT_TRUE(Get(store, PROFILE_A, Key(2)));
  EXPECT_TRUE(Get(store, PROFILE_A, Key(999)));
  std::optional<JumpgatePlaybackHistoryEntry> local;
  ASSERT_TRUE(store.Get(LocalKey(Key(999)), local, error)) << error;
  ASSERT_TRUE(local);
  EXPECT_EQ(local->positionMs, 2);

  JumpgatePlaybackHistoryDocument parsed;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, parsed, error)) << error;
  EXPECT_EQ(parsed.entries.size(), JUMPGATE_HISTORY_MAX_ENTRIES);
}

TEST(TestJumpgatePlaybackHistory, PrunesBySerializedSizeAndNeverPersistsSecretFields)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  std::string wideTitle;
  for (int index = 0; index < 1024; ++index)
    wideTitle += "\xe2\x82\xac";

  for (uint64_t index = 0; index < 160; ++index)
  {
    auto entry = Entry(PROFILE_A, Key(index), 1, 1000, static_cast<int64_t>(index + 1));
    entry.display.title = wideTitle;
    ASSERT_TRUE(Save(store, PROFILE_A, std::move(entry), error)) << index << ": " << error;
  }
  EXPECT_LE(storage.bytes.size(),
            JUMPGATE_HISTORY_MAX_BYTES -
                JUMPGATE_HISTORY_PROFILE_PROTECTION_RESERVE_BYTES);
  JumpgatePlaybackHistoryDocument parsed;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, parsed, error)) << error;
  EXPECT_LT(parsed.entries.size(), 160u);
  EXPECT_EQ(storage.bytes.find("deviceToken"), std::string::npos);
  EXPECT_EQ(storage.bytes.find("configCapability"), std::string::npos);
  EXPECT_EQ(storage.bytes.find("credentialRef"), std::string::npos);
  EXPECT_EQ(storage.bytes.find("authorization"), std::string::npos);
  EXPECT_EQ(storage.bytes.find("bridgeBaseUrl"), std::string::npos);
  EXPECT_EQ(storage.bytes.find("providerUrl"), std::string::npos);
}

TEST(TestJumpgatePlaybackHistory, UnrepresentableBlockLeavesExactCommittedBytesUnchanged)
{
  const std::string committed = NearLimitLegacyHistory(PROFILE_A);
  ASSERT_FALSE(committed.empty());

  FakeHistoryStorage storage;
  storage.bytes = committed;
  storage.hasContents = true;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  EXPECT_FALSE(store.BlockProfile(PROFILE_A, error));
  EXPECT_NE(error.find("size limit"), std::string::npos);
  EXPECT_EQ(storage.writeAttempts, 0);
  EXPECT_EQ(storage.bytes, committed);
}

TEST(TestJumpgatePlaybackHistory, SchemaTwoBlockCapacityRejectionPreservesCommittedHistory)
{
  JumpgatePlaybackHistoryDocument document;
  for (uint64_t index = 0; index < 64; ++index)
  {
    auto entry = Entry(PROFILE_A, Key(index), 1, 1000, static_cast<int64_t>(index + 1));
    entry.display.title = std::string(7000, 'x');
    document.entries.emplace_back(std::move(entry));
  }
  const std::string committed = NearLimitHistory(std::move(document), false);
  ASSERT_FALSE(committed.empty());
  ASSERT_EQ(committed.size(), JUMPGATE_HISTORY_MAX_BYTES);

  FakeHistoryStorage storage;
  storage.bytes = committed;
  storage.hasContents = true;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  ASSERT_FALSE(store.BlockProfile(PROFILE_A, error));
  EXPECT_NE(error.find("size limit"), std::string::npos);
  EXPECT_EQ(storage.writeAttempts, 0);
  EXPECT_EQ(storage.bytes, committed);

  const auto available = Get(store, PROFILE_A, Key(0));
  ASSERT_TRUE(available);
  EXPECT_EQ(available->contentKey, Key(0));
  EXPECT_EQ(storage.writeAttempts, 0);
  EXPECT_EQ(storage.bytes, committed);
}

TEST(TestJumpgatePlaybackHistory, NearLimitSchemaOnePurgeEvictsOnlyAffectedProfileHistory)
{
  ExpectNearLimitPurgePreservesUnaffectedHistory(true);
}

TEST(TestJumpgatePlaybackHistory,
     NearLimitSchemaTwoPurgePreservesUnrelatedAndLocalSourceHistory)
{
  ExpectNearLimitPurgePreservesUnaffectedHistory(false);
}

TEST(TestJumpgatePlaybackHistory, DestructiveClearPrunesOnlyAffectedProfileHistory)
{
  JumpgatePlaybackHistoryDocument full;
  std::string fullest;
  std::string error;
  for (uint64_t index = 0; index < JUMPGATE_HISTORY_MAX_ENTRIES; ++index)
  {
    auto entry = index == 0
                     ? LocalEntry(Key(index), 1, 1000, static_cast<int64_t>(index + 1))
                     : Entry(index == 1 ? PROFILE_B : PROFILE_A, Key(index), 1, 1000,
                             static_cast<int64_t>(index + 1));
    entry.display.title = std::string(2048, 'x');
    full.entries.emplace_back(std::move(entry));
    std::string candidate;
    if (!SerializeJumpgatePlaybackHistory(full, candidate, error))
    {
      full.entries.pop_back();
      break;
    }
    fullest = std::move(candidate);
  }
  ASSERT_FALSE(fullest.empty());
  ASSERT_FALSE(full.entries.empty());
  std::size_t bestTitleLength = full.entries.back().display.title->size();
  std::size_t low = bestTitleLength + 1;
  std::size_t high = 8192;
  while (low <= high)
  {
    const std::size_t candidateLength = low + (high - low) / 2;
    full.entries.back().display.title = std::string(candidateLength, 'x');
    std::string candidate;
    if (SerializeJumpgatePlaybackHistory(full, candidate, error))
    {
      bestTitleLength = candidateLength;
      fullest = std::move(candidate);
      low = candidateLength + 1;
    }
    else
    {
      high = candidateLength - 1;
    }
  }
  full.entries.back().display.title = std::string(bestTitleLength, 'x');
  error.clear();
  ASSERT_GT(fullest.size(),
            JUMPGATE_HISTORY_MAX_BYTES -
                JUMPGATE_HISTORY_PROFILE_PROTECTION_RESERVE_BYTES);

  FakeHistoryStorage storage;
  storage.bytes = fullest;
  storage.hasContents = true;
  CJumpgatePlaybackHistoryStore store(storage);
  ASSERT_TRUE(store.ClearProfile(PROFILE_B, error)) << error;
  JumpgatePlaybackHistoryDocument protectedDocument;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, protectedDocument, error)) << error;
  EXPECT_EQ(std::count_if(protectedDocument.entries.begin(), protectedDocument.entries.end(),
                          [](const auto& entry)
                          {
                            return entry.historyNamespace ==
                                       JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                                   entry.profileId == PROFILE_A;
                          }),
            std::count_if(full.entries.begin(), full.entries.end(), [](const auto& entry)
                          {
                            return entry.historyNamespace ==
                                       JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                                   entry.profileId == PROFILE_A;
                          }));
  EXPECT_EQ(std::count_if(protectedDocument.entries.begin(), protectedDocument.entries.end(),
                          [](const auto& entry)
                          {
                            return entry.historyNamespace ==
                                   JumpgatePlaybackHistoryNamespace::LocalSource;
                          }),
            1);
  EXPECT_EQ(std::count_if(protectedDocument.entries.begin(), protectedDocument.entries.end(),
                          [](const auto& entry) { return entry.profileId == PROFILE_B; }),
            0);
  EXPECT_NE(std::find(protectedDocument.blockedProfiles.begin(),
                      protectedDocument.blockedProfiles.end(), PROFILE_B),
            protectedDocument.blockedProfiles.end());
  EXPECT_NE(std::find(protectedDocument.forgottenProfiles.begin(),
                      protectedDocument.forgottenProfiles.end(), PROFILE_B),
            protectedDocument.forgottenProfiles.end());
}

TEST(TestJumpgatePlaybackHistory, NearLimitLegacyBlockAndUnblockPreserveEveryEntry)
{
  const std::string representableBlocked = NearLimitLegacyHistory(PROFILE_A, {PROFILE_A});
  ASSERT_FALSE(representableBlocked.empty());
  CVariant initial = ParseJson(representableBlocked);
  initial["blockedProfiles"].clear();
  const std::string initialBytes = WriteJson(initial);

  std::string error;
  JumpgatePlaybackHistoryDocument expected;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(initialBytes, expected, error)) << error;
  ASSERT_TRUE(expected.loadedFromLegacySchema);
  ASSERT_EQ(expected.entries.size(), 64u);
  std::string schemaTwo;
  EXPECT_FALSE(SerializeJumpgatePlaybackHistory(expected, schemaTwo, error));
  EXPECT_NE(error.find("size limit"), std::string::npos);

  FakeHistoryStorage storage;
  storage.bytes = initialBytes;
  storage.hasContents = true;
  CJumpgatePlaybackHistoryStore store(storage);
  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;

  JumpgatePlaybackHistoryDocument blocked;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, blocked, error)) << error;
  EXPECT_TRUE(blocked.loadedFromLegacySchema);
  EXPECT_NE(std::find(blocked.blockedProfiles.begin(), blocked.blockedProfiles.end(), PROFILE_A),
            blocked.blockedProfiles.end());
  ExpectSameHistoryEntries(expected.entries, blocked.entries);

  ASSERT_TRUE(store.UnblockProfile(PROFILE_A, error)) << error;
  JumpgatePlaybackHistoryDocument unblocked;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, unblocked, error)) << error;
  EXPECT_TRUE(unblocked.loadedFromLegacySchema);
  EXPECT_EQ(std::find(unblocked.blockedProfiles.begin(), unblocked.blockedProfiles.end(), PROFILE_A),
            unblocked.blockedProfiles.end());
  ExpectSameHistoryEntries(expected.entries, unblocked.entries);
}

TEST(TestJumpgatePlaybackHistory, NearLimitSchemaOneLifecycleRemainsRepairable)
{
  const std::string legacyA = NearLimitLegacyHistory(PROFILE_A);
  const std::string legacyABlocked = NearLimitLegacyHistory(PROFILE_A, {PROFILE_A});
  const std::string legacyBBlocked = NearLimitLegacyHistory(PROFILE_B, {PROFILE_A});
  const std::string legacyBRepair =
      NearLimitLegacyHistory(PROFILE_B, {PROFILE_A}, {PROFILE_A});
  ASSERT_FALSE(legacyA.empty());
  ASSERT_FALSE(legacyABlocked.empty());
  ASSERT_FALSE(legacyBBlocked.empty());
  ASSERT_FALSE(legacyBRepair.empty());
  std::string error;

  FakeHistoryStorage clearStorage;
  clearStorage.bytes = legacyA;
  clearStorage.hasContents = true;
  CJumpgatePlaybackHistoryStore clearStore(clearStorage);
  ASSERT_TRUE(clearStore.ClearProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(clearStore.PurgeBlockedProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(clearStore.CompleteProfileRepair(PROFILE_A, error)) << error;

  FakeHistoryStorage purgeStorage;
  purgeStorage.bytes = legacyABlocked;
  purgeStorage.hasContents = true;
  CJumpgatePlaybackHistoryStore purgeStore(purgeStorage);
  ASSERT_TRUE(purgeStore.PurgeBlockedProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(purgeStore.CompleteProfileRepair(PROFILE_A, error)) << error;

  FakeHistoryStorage unblockStorage;
  unblockStorage.bytes = legacyBBlocked;
  unblockStorage.hasContents = true;
  CJumpgatePlaybackHistoryStore unblockStore(unblockStorage);
  ASSERT_TRUE(unblockStore.UnblockProfile(PROFILE_A, error)) << error;
  JumpgatePlaybackHistoryDocument unblocked;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(unblockStorage.bytes, unblocked, error)) << error;
  EXPECT_EQ(unblocked.entries.size(), 64u);
  EXPECT_TRUE(std::all_of(unblocked.entries.begin(), unblocked.entries.end(), [](const auto& entry)
                          { return entry.profileId == PROFILE_B; }));

  FakeHistoryStorage repairStorage;
  repairStorage.bytes = legacyBRepair;
  repairStorage.hasContents = true;
  CJumpgatePlaybackHistoryStore repairStore(repairStorage);
  ASSERT_TRUE(repairStore.CompleteProfileRepair(PROFILE_A, error)) << error;
  JumpgatePlaybackHistoryDocument repaired;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(repairStorage.bytes, repaired, error)) << error;
  EXPECT_EQ(repaired.entries.size(), 64u);

  FakeHistoryStorage resetStorage;
  resetStorage.bytes = legacyA;
  resetStorage.hasContents = true;
  CJumpgatePlaybackHistoryStore resetStore(resetStorage);
  ASSERT_TRUE(resetStore.ResetProfile(PROFILE_A, error)) << error;
  JumpgatePlaybackHistoryDocument reset;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(resetStorage.bytes, reset, error)) << error;
  EXPECT_TRUE(reset.entries.empty());
}

TEST(TestJumpgatePlaybackHistory, AtomicFailureAndStaleUpdateLeaveCommittedBytesUntouched)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  const std::string committed = storage.bytes;

  storage.failWrite = true;
  EXPECT_FALSE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 200, 1000, 200), error));
  EXPECT_EQ(storage.bytes, committed);
  storage.failWrite = false;

  EXPECT_FALSE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 50, 1000, 99), error));
  EXPECT_EQ(storage.bytes, committed);
  auto saved = Get(store, PROFILE_A, key);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->positionMs, 100);
  EXPECT_EQ(saved->updatedAtMs, 100);
}

TEST(TestJumpgatePlaybackHistory, EqualTimestampIsIdempotentOnlyForEquivalentPayload)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  auto original = Entry(PROFILE_A, key, 100, 1000, 100);
  original.canonicalIdentity = Identity(JumpgateCanonicalProvider::Imdb, "tt0133093");
  ASSERT_TRUE(Save(store, PROFILE_A, original, error)) << error;
  const int writesAfterOriginal = storage.writeAttempts;

  auto equivalentRetry = original;
  equivalentRetry.canonicalIdentity.reset();
  EXPECT_TRUE(Save(store, PROFILE_A, equivalentRetry, error)) << error;
  EXPECT_EQ(storage.writeAttempts, writesAfterOriginal);

  auto conflictingRetry = original;
  conflictingRetry.positionMs = 200;
  EXPECT_FALSE(Save(store, PROFILE_A, conflictingRetry, error));
  EXPECT_NE(error.find("timestamp conflicts"), std::string::npos);
  EXPECT_EQ(storage.writeAttempts, writesAfterOriginal);
  const auto saved = Get(store, PROFILE_A, key);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->positionMs, 100);

  const std::string completedKey = Key(2);
  auto completed = Entry(PROFILE_A, completedKey, 900, 1000, 200);
  ASSERT_TRUE(Save(store, PROFILE_A, completed, error)) << error;
  const int writesAfterCompleted = storage.writeAttempts;
  auto zeroClockRetry = Entry(PROFILE_A, completedKey, 0, 0, 200);
  ASSERT_TRUE(Save(store, PROFILE_A, zeroClockRetry, error)) << error;
  EXPECT_EQ(storage.writeAttempts, writesAfterCompleted);
  const auto completedSaved = Get(store, PROFILE_A, completedKey);
  ASSERT_TRUE(completedSaved);
  EXPECT_TRUE(completedSaved->completed);
  EXPECT_TRUE(completedSaved->watched);
  EXPECT_EQ(completedSaved->positionMs, 900);
  EXPECT_EQ(completedSaved->durationMs, 1000);

  zeroClockRetry.display.title = "Conflicting retry";
  EXPECT_FALSE(Save(store, PROFILE_A, zeroClockRetry, error));
  EXPECT_NE(error.find("timestamp conflicts"), std::string::npos);
  EXPECT_EQ(storage.writeAttempts, writesAfterCompleted);
}

TEST(TestJumpgatePlaybackHistory, FailedAtomicClearStillBlocksStaleInProcessWrites)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  const std::string committed = storage.bytes;

  storage.failWrite = true;
  EXPECT_FALSE(store.ClearProfile(PROFILE_A, error));
  EXPECT_EQ(storage.bytes, committed);
  EXPECT_FALSE(Get(store, PROFILE_A, key));
  EXPECT_FALSE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
  storage.failWrite = false;

  ASSERT_TRUE(store.UnblockProfile(PROFILE_A, error)) << error;
  const auto restored = Get(store, PROFILE_A, key);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->positionMs, 100);
}

TEST(TestJumpgatePlaybackHistory, FailedForgetMutationCanRecoverBlockedHistoryWithoutDataLoss)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;

  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;
  bool profileBlocked = false;
  ASSERT_TRUE(store.IsProfileBlocked(PROFILE_A, profileBlocked, error)) << error;
  EXPECT_TRUE(profileBlocked);
  EXPECT_FALSE(Get(store, PROFILE_A, key));
  JumpgatePlaybackHistoryDocument blocked;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(storage.bytes, blocked, error)) << error;
  ASSERT_EQ(blocked.entries.size(), 1u);
  ASSERT_EQ(blocked.blockedProfiles.size(), 1u);

  storage.failWrite = true;
  EXPECT_FALSE(store.UnblockProfile(PROFILE_A, error));
  EXPECT_FALSE(Get(store, PROFILE_A, key));
  storage.failWrite = false;

  ASSERT_TRUE(store.UnblockProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(store.IsProfileBlocked(PROFILE_A, profileBlocked, error)) << error;
  EXPECT_FALSE(profileBlocked);
  const auto recovered = Get(store, PROFILE_A, key);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->positionMs, 100);
}

TEST(TestJumpgatePlaybackHistory, RepairUnblocksTombstoneWithoutDeletingHistory)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 444, 1000, 100), error)) << error;
  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;

  bool blocked = false;
  ASSERT_TRUE(store.IsProfileBlocked(PROFILE_A, blocked, error)) << error;
  ASSERT_TRUE(blocked);
  ASSERT_TRUE(store.UnblockProfile(PROFILE_A, error)) << error;

  const auto restored = Get(store, PROFILE_A, key);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->positionMs, 444);
}

TEST(TestJumpgatePlaybackHistory, FailedPreForgetBlockLeavesHistoryAvailableAndUnchanged)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  const std::string committed = storage.bytes;

  storage.failWrite = true;
  EXPECT_FALSE(store.BlockProfile(PROFILE_A, error));
  EXPECT_EQ(storage.bytes, committed);
  storage.failWrite = false;
  const auto available = Get(store, PROFILE_A, key);
  ASSERT_TRUE(available);
  EXPECT_EQ(available->positionMs, 100);
}

TEST(TestJumpgatePlaybackHistory, SuccessfulForgetPurgePersistsNonResurrectionMarker)
{
  FakeHistoryStorage storage;
  std::string error;
  const std::string key = Key(1);
  {
    CJumpgatePlaybackHistoryStore store(storage);
    ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
    ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;
    ASSERT_TRUE(store.PurgeBlockedProfile(PROFILE_A, error)) << error;
  }

  CJumpgatePlaybackHistoryStore reopened(storage);
  EXPECT_FALSE(Get(reopened, PROFILE_A, key));
  EXPECT_FALSE(Save(reopened, PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
  ASSERT_TRUE(reopened.ResetProfile(PROFILE_A, error)) << error;
  EXPECT_TRUE(Save(reopened, PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
}

TEST(TestJumpgatePlaybackHistory, ForgottenMarkerRequiresPurgeBeforeRepair)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;

  storage.failWriteAttempt = storage.writeAttempts + 2;
  EXPECT_FALSE(store.PurgeBlockedProfile(PROFILE_A, error));
  bool blocked = false;
  bool forgotten = false;
  ASSERT_TRUE(store.GetProfileProtection(PROFILE_A, blocked, forgotten, error)) << error;
  EXPECT_TRUE(blocked);
  EXPECT_TRUE(forgotten);
  EXPECT_FALSE(store.UnblockProfile(PROFILE_A, error));

  storage.failWriteAttempt = 0;
  CJumpgatePlaybackHistoryStore reopenedAfterFailure(storage);
  ASSERT_TRUE(reopenedAfterFailure.GetProfileProtection(PROFILE_A, blocked, forgotten, error))
      << error;
  EXPECT_TRUE(blocked);
  EXPECT_TRUE(forgotten);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(blocked, forgotten, true),
            JumpgatePairingHistoryAction::PurgeThenUnblock);
  ASSERT_TRUE(reopenedAfterFailure.PurgeBlockedProfile(PROFILE_A, error)) << error;

  CJumpgatePlaybackHistoryStore reopenedAfterPurge(storage);
  ASSERT_TRUE(reopenedAfterPurge.GetProfileProtection(PROFILE_A, blocked, forgotten, error))
      << error;
  EXPECT_TRUE(blocked);
  EXPECT_TRUE(forgotten);
  EXPECT_EQ(GetJumpgatePairingHistoryAction(blocked, forgotten, true),
            JumpgatePairingHistoryAction::PurgeThenUnblock);
  EXPECT_FALSE(reopenedAfterPurge.UnblockProfile(PROFILE_A, error));
  ASSERT_TRUE(reopenedAfterPurge.CompleteProfileRepair(PROFILE_A, error)) << error;
  EXPECT_FALSE(Get(reopenedAfterPurge, PROFILE_A, key));
}

TEST(TestJumpgatePlaybackHistory, ProfileProtectionAndRepairNeverEraseLocalSourceHistory)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string localKey = Key(100);
  ASSERT_TRUE(store.Save(LocalKey(localKey), LocalEntry(localKey, 321, 1000, 10), error)) << error;
  ASSERT_TRUE(Save(store, PROFILE_A, Entry(PROFILE_A, Key(1), 123, 1000, 11), error)) << error;

  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(store.UnblockProfile(PROFILE_A, error)) << error;
  std::optional<JumpgatePlaybackHistoryEntry> local;
  ASSERT_TRUE(store.Get(LocalKey(localKey), local, error)) << error;
  ASSERT_TRUE(local);
  EXPECT_EQ(local->positionMs, 321);

  ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;
  storage.failWriteAttempt = storage.writeAttempts + 2;
  EXPECT_FALSE(store.PurgeBlockedProfile(PROFILE_A, error));
  ASSERT_TRUE(store.Get(LocalKey(localKey), local, error)) << error;
  ASSERT_TRUE(local);
  EXPECT_EQ(local->positionMs, 321);

  storage.failWriteAttempt = 0;
  CJumpgatePlaybackHistoryStore reopened(storage);
  ASSERT_TRUE(reopened.PurgeBlockedProfile(PROFILE_A, error)) << error;
  ASSERT_TRUE(reopened.CompleteProfileRepair(PROFILE_A, error)) << error;
  ASSERT_TRUE(reopened.ResetProfile(PROFILE_B, error)) << error;
  ASSERT_TRUE(reopened.Get(LocalKey(localKey), local, error)) << error;
  ASSERT_TRUE(local);
  EXPECT_EQ(local->positionMs, 321);
}

TEST(TestJumpgatePlaybackHistory, ForgottenProvenanceIsStrictAndSecretFree)
{
  JumpgatePlaybackHistoryDocument document;
  document.forgottenProfiles.emplace_back(PROFILE_A);
  std::string json;
  std::string error;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;
  EXPECT_NE(json.find("forgottenProfiles"), std::string::npos);
  EXPECT_EQ(json.find("deviceToken"), std::string::npos);
  EXPECT_EQ(json.find("credentialRef"), std::string::npos);

  JumpgatePlaybackHistoryDocument parsed;
  ASSERT_TRUE(ParseJumpgatePlaybackHistory(json, parsed, error)) << error;
  ASSERT_EQ(parsed.forgottenProfiles.size(), 1u);
  EXPECT_EQ(parsed.forgottenProfiles.front(), PROFILE_A);

  CVariant malformed;
  ASSERT_TRUE(CJSONVariantParser::Parse(json, malformed));
  malformed["forgottenProfiles"].push_back(PROFILE_A);
  ASSERT_TRUE(CJSONVariantWriter::Write(malformed, json, true));
  EXPECT_FALSE(ParseJumpgatePlaybackHistory(json, parsed, error));
}
