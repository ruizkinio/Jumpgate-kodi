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

std::optional<JumpgatePlaybackHistoryEntry> Get(CJumpgatePlaybackHistoryStore& store,
                                                const std::string& profileId,
                                                const std::string& contentKey)
{
  std::optional<JumpgatePlaybackHistoryEntry> entry;
  std::string error;
  EXPECT_TRUE(store.Get(profileId, contentKey, entry, error)) << error;
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

TEST(TestJumpgatePlaybackHistory,
     IsolatesProfilesWithTheSameContentKeyAndClearsOnlyForgottenProfile)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(7);
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 111, 1000, 1), error)) << error;
  ASSERT_TRUE(store.Save(PROFILE_B, Entry(PROFILE_B, key, 222, 1000, 2), error)) << error;

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
  EXPECT_FALSE(store.Save(PROFILE_A, Entry(PROFILE_B, Key(1)), error));
  EXPECT_FALSE(store.Save(" profile", Entry(" profile", Key(1)), error));

  JumpgatePlaybackHistoryDocument document;
  document.entries.emplace_back(Entry(PROFILE_A, Key(1)));
  std::string json;
  ASSERT_TRUE(SerializeJumpgatePlaybackHistory(document, json, error)) << error;

  std::array<CVariant, 5> malformed;
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

  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), 799, 1000, 1), error)) << error;
  auto belowWatched = Get(store, PROFILE_A, Key(1));
  ASSERT_TRUE(belowWatched);
  EXPECT_FALSE(belowWatched->watched);
  EXPECT_FALSE(belowWatched->completed);

  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 800, 1000, 2), error)) << error;
  auto watched = Get(store, PROFILE_A, Key(2));
  ASSERT_TRUE(watched);
  EXPECT_TRUE(watched->watched);
  EXPECT_FALSE(watched->completed);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*watched), 800);

  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(3), 900, 1000, 3), error)) << error;
  auto completed = Get(store, PROFILE_A, Key(3));
  ASSERT_TRUE(completed);
  EXPECT_TRUE(completed->watched);
  EXPECT_TRUE(completed->completed);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*completed), 0);

  auto explicitEnd = Entry(PROFILE_A, Key(4), 100, 1000, 4);
  explicitEnd.completed = true;
  ASSERT_TRUE(store.Save(PROFILE_A, explicitEnd, error)) << error;
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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), position, duration, 10), error))
      << error;
  auto saved = Get(store, PROFILE_A, Key(1));
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->positionMs, position);
  EXPECT_EQ(saved->durationMs, duration);

  auto completed = Entry(PROFILE_A, Key(2), 900, 1000, 20);
  ASSERT_TRUE(store.Save(PROFILE_A, completed, error)) << error;
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 10, 1000, 21), error)) << error;
  saved = Get(store, PROFILE_A, Key(2));
  ASSERT_TRUE(saved);
  EXPECT_FALSE(saved->completed);
  EXPECT_TRUE(saved->watched);
  EXPECT_EQ(GetJumpgatePlaybackResumePosition(*saved), 10);

  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 0, 0, 22), error)) << error;
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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), 900, 1000, 10), error)) << error;
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), 0, 0, 11), error)) << error;
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
      { firstSaved = store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), 10, 1000, 1), firstError); });
  storage.WaitForBlockedWrite();
  const int readsBeforeSecond = storage.ReadAttempts();
  std::thread second(
      [&]
      {
        secondStarted.store(true);
        secondSaved = store.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 20, 1000, 2), secondError);
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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(1), 10, 1000, 1), error)) << error;
  ASSERT_TRUE(store.Save(PROFILE_B, Entry(PROFILE_B, Key(2), 20, 1000, 2), error)) << error;

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
        staleSaved = store.Save(PROFILE_A, Entry(PROFILE_A, Key(3), 30, 1000, 3), staleError);
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
  EXPECT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(4), 40, 1000, 4), error)) << error;
  EXPECT_TRUE(Get(store, PROFILE_A, Key(4)));
}

TEST(TestJumpgatePlaybackHistory, PrunesOldestEntryDeterministically)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  for (uint64_t index = 0; index < JUMPGATE_HISTORY_MAX_ENTRIES; ++index)
  {
    ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(index), 1, 1000, 10), error))
        << index << ": " << error;
  }
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(999), 1, 1000, 11), error)) << error;
  EXPECT_FALSE(Get(store, PROFILE_A, Key(0)));
  EXPECT_TRUE(Get(store, PROFILE_A, Key(1)));
  EXPECT_TRUE(Get(store, PROFILE_A, Key(999)));

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
    ASSERT_TRUE(store.Save(PROFILE_A, std::move(entry), error)) << index << ": " << error;
  }
  EXPECT_LE(storage.bytes.size(), JUMPGATE_HISTORY_MAX_BYTES);
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

TEST(TestJumpgatePlaybackHistory, AtomicFailureAndStaleUpdateLeaveCommittedBytesUntouched)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  const std::string committed = storage.bytes;

  storage.failWrite = true;
  EXPECT_FALSE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 200, 1000, 200), error));
  EXPECT_EQ(storage.bytes, committed);
  storage.failWrite = false;

  EXPECT_FALSE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 50, 1000, 99), error));
  EXPECT_EQ(storage.bytes, committed);
  auto saved = Get(store, PROFILE_A, key);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->positionMs, 100);
  EXPECT_EQ(saved->updatedAtMs, 100);
}

TEST(TestJumpgatePlaybackHistory, FailedAtomicClearStillBlocksStaleInProcessWrites)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
  const std::string committed = storage.bytes;

  storage.failWrite = true;
  EXPECT_FALSE(store.ClearProfile(PROFILE_A, error));
  EXPECT_EQ(storage.bytes, committed);
  EXPECT_FALSE(Get(store, PROFILE_A, key));
  EXPECT_FALSE(store.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;

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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 444, 1000, 100), error)) << error;
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
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
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
    ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
    ASSERT_TRUE(store.BlockProfile(PROFILE_A, error)) << error;
    ASSERT_TRUE(store.PurgeBlockedProfile(PROFILE_A, error)) << error;
  }

  CJumpgatePlaybackHistoryStore reopened(storage);
  EXPECT_FALSE(Get(reopened, PROFILE_A, key));
  EXPECT_FALSE(reopened.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
  ASSERT_TRUE(reopened.ResetProfile(PROFILE_A, error)) << error;
  EXPECT_TRUE(reopened.Save(PROFILE_A, Entry(PROFILE_A, Key(2), 200, 1000, 200), error));
}

TEST(TestJumpgatePlaybackHistory, ForgottenMarkerRequiresPurgeBeforeRepair)
{
  FakeHistoryStorage storage;
  CJumpgatePlaybackHistoryStore store(storage);
  std::string error;
  const std::string key = Key(1);
  ASSERT_TRUE(store.Save(PROFILE_A, Entry(PROFILE_A, key, 100, 1000, 100), error)) << error;
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
