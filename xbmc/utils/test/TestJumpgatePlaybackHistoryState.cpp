/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackHistoryState.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
JumpgatePlaybackHistoryIdentity Identity(uint64_t generation,
                                         const std::string& profileId,
                                         char keyCharacter)
{
  JumpgatePlaybackHistoryIdentity identity;
  identity.generation = generation;
  identity.historyNamespace = JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
  identity.profileId = profileId;
  identity.contentKey = std::string(64, keyCharacter);
  identity.display.title = "Generation-bound title";
  return identity;
}
} // namespace

TEST(TestJumpgatePlaybackHistoryState,
     ActivatesFingerprintAndRawUriFallbackWithoutRetainingAuthenticatedIdentity)
{
  const std::string rawLaunchUri =
      "https://provider.example/private/video.m3u8?token=never-retain-this";
  const std::vector<std::string> fingerprints{"v1:url:sha256:" + std::string(64, 'a')};

  CJumpgatePlaybackHistoryState fingerprintState;
  ASSERT_TRUE(fingerprintState.AdvanceGeneration(1));
  ASSERT_TRUE(fingerprintState.ActivateLocalSource(1, fingerprints, rawLaunchUri, 10));
  const auto fingerprintToken = fingerprintState.BeginResume(1);
  const auto fingerprintKey = DeriveJumpgateLocalSourceHistoryKey(fingerprints);
  ASSERT_TRUE(fingerprintToken);
  ASSERT_TRUE(fingerprintKey);
  EXPECT_EQ(fingerprintToken->historyNamespace, JumpgatePlaybackHistoryNamespace::LocalSource);
  EXPECT_TRUE(fingerprintToken->profileId.empty());
  EXPECT_EQ(fingerprintToken->contentKey, *fingerprintKey);

  CJumpgatePlaybackHistoryState fallbackState;
  ASSERT_TRUE(fallbackState.AdvanceGeneration(1));
  ASSERT_TRUE(fallbackState.ActivateLocalSource(1, {}, rawLaunchUri, 10));
  const auto fallbackToken = fallbackState.BeginResume(1);
  ASSERT_TRUE(fallbackToken);
  EXPECT_EQ(fallbackToken->historyNamespace, JumpgatePlaybackHistoryNamespace::LocalSource);
  EXPECT_TRUE(fallbackToken->profileId.empty());
  EXPECT_EQ(fallbackToken->contentKey, DeriveJumpgateLocalSourceFallbackHistoryKey(rawLaunchUri));
  EXPECT_EQ(fallbackToken->contentKey.find(rawLaunchUri), std::string::npos);
  EXPECT_EQ(fallbackToken->contentKey.find("never-retain-this"), std::string::npos);

  ASSERT_TRUE(fallbackState.UpdateProgress(1, 400, 1000, 20));
  const auto localEntry = fallbackState.Finalize(1, false, 30);
  ASSERT_TRUE(localEntry);
  EXPECT_EQ(localEntry->historyNamespace, JumpgatePlaybackHistoryNamespace::LocalSource);
  EXPECT_TRUE(localEntry->profileId.empty());
  EXPECT_FALSE(localEntry->canonicalIdentity);
}

TEST(TestJumpgatePlaybackHistoryState, PromotionPreservesProgressAndInvalidatesLocalResumeToken)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(7));
  ASSERT_TRUE(state.ActivateLocalSource(7, {"v1:url:sha256:" + std::string(64, 'a')},
                                        "private://raw-launch", 10));
  ASSERT_TRUE(state.UpdateProgress(7, 4567, 10000, 20));
  const auto localToken = state.BeginResume(7);
  ASSERT_TRUE(localToken);

  JumpgatePlaybackHistoryIdentity authenticated = Identity(7, "profile-promoted", 'b');
  JumpgateCanonicalIdentity canonical;
  canonical.provider = JumpgateCanonicalProvider::Imdb;
  canonical.id = "tt0133093";
  canonical.mediaType = JumpgateMediaType::Movie;
  authenticated.canonicalIdentity = canonical;
  ASSERT_TRUE(state.Promote(authenticated));

  bool staleApplied = false;
  EXPECT_FALSE(
      state.ApplyResume(*localToken, 1234, [&staleApplied](int64_t) { staleApplied = true; }));
  EXPECT_FALSE(staleApplied);

  const auto authenticatedToken = state.BeginResume(7);
  ASSERT_TRUE(authenticatedToken);
  EXPECT_EQ(authenticatedToken->historyNamespace,
            JumpgatePlaybackHistoryNamespace::AuthenticatedProfile);
  EXPECT_EQ(authenticatedToken->profileId, "profile-promoted");
  EXPECT_EQ(authenticatedToken->contentKey, std::string(64, 'b'));
  EXPECT_NE(authenticatedToken->serial, localToken->serial);

  bool authenticatedApplied = false;
  ASSERT_TRUE(state.ApplyResume(*authenticatedToken, 2222,
                                [&authenticatedApplied](int64_t positionMs)
                                {
                                  authenticatedApplied = true;
                                  EXPECT_EQ(positionMs, 2222);
                                }));
  EXPECT_TRUE(authenticatedApplied);
  const auto entry = state.Finalize(7, false, 30);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->historyNamespace, JumpgatePlaybackHistoryNamespace::AuthenticatedProfile);
  EXPECT_EQ(entry->profileId, "profile-promoted");
  EXPECT_EQ(entry->contentKey, std::string(64, 'b'));
  EXPECT_EQ(entry->positionMs, 4567);
  EXPECT_EQ(entry->durationMs, 10000);
  ASSERT_TRUE(entry->canonicalIdentity);
  EXPECT_EQ(entry->canonicalIdentity->id, "tt0133093");
}

TEST(TestJumpgatePlaybackHistoryState, PromotionCarriesProvisionalResumeForZeroCorrection)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.ActivateLocalSource(1, {}, "private://source", 10));
  const auto localToken = state.BeginResume(1);
  ASSERT_TRUE(localToken);
  ASSERT_TRUE(state.ApplyResume(*localToken, 7200000, [](int64_t) {}));

  ASSERT_TRUE(state.Promote(Identity(1, "profile", 'b')));
  const auto authenticatedToken = state.BeginResume(1);
  ASSERT_TRUE(authenticatedToken);
  ASSERT_TRUE(authenticatedToken->previouslyAppliedPositionMs);
  EXPECT_EQ(*authenticatedToken->previouslyAppliedPositionMs, 7200000);

  int64_t appliedPosition = -1;
  ASSERT_TRUE(state.ApplyResume(*authenticatedToken, 0,
                                [&appliedPosition](int64_t positionMs)
                                { appliedPosition = positionMs; }));
  EXPECT_EQ(appliedPosition, 0);
}

TEST(TestJumpgatePlaybackHistoryState, RejectsStaleLateAndNamespaceOrKeyAliasedPromotion)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.ActivateLocalSource(1, {}, "private://source-one", 10));
  const auto localToken = state.BeginResume(1);
  ASSERT_TRUE(localToken);

  auto stale = Identity(2, "profile", 'b');
  EXPECT_FALSE(state.Promote(stale));

  auto wrongNamespace = Identity(1, "", 'c');
  wrongNamespace.historyNamespace = JumpgatePlaybackHistoryNamespace::LocalSource;
  EXPECT_FALSE(state.Promote(wrongNamespace));

  auto aliasedKey = Identity(1, "profile", 'b');
  aliasedKey.contentKey = localToken->contentKey;
  EXPECT_FALSE(state.Promote(aliasedKey));

  auto accepted = Identity(1, "profile", 'b');
  ASSERT_TRUE(state.Promote(accepted));
  EXPECT_FALSE(state.Promote(Identity(1, "other-profile", 'c')));

  CJumpgatePlaybackHistoryState lateState;
  ASSERT_TRUE(lateState.AdvanceGeneration(3));
  ASSERT_TRUE(lateState.ActivateLocalSource(3, {}, "private://source-three", 10));
  ASSERT_TRUE(lateState.UpdateProgress(3, 100, 1000, 20));
  ASSERT_TRUE(lateState.Finalize(3, false, 30));
  EXPECT_FALSE(lateState.Promote(Identity(3, "profile", 'd')));
  ASSERT_TRUE(lateState.AdvanceGeneration(4));
  EXPECT_FALSE(lateState.Promote(Identity(3, "profile", 'd')));
}

TEST(TestJumpgatePlaybackHistoryState, CapturesOneAtomicFinalSnapshotAndRejectsOldGeneration)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.Activate(Identity(1, "profile-one", 'a'), 10));
  ASSERT_TRUE(state.UpdateProgress(1, 1234567890123LL, 2234567890123LL, 20));

  const auto first = state.Finalize(1, false, 30);
  const auto repeated = state.Finalize(1, true, 40);
  ASSERT_TRUE(first);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->profileId, first->profileId);
  EXPECT_EQ(repeated->contentKey, first->contentKey);
  EXPECT_EQ(repeated->positionMs, first->positionMs);
  EXPECT_EQ(repeated->durationMs, first->durationMs);
  EXPECT_GT(repeated->updatedAtMs, first->updatedAtMs);
  EXPECT_FALSE(first->completed);
  EXPECT_TRUE(repeated->completed);
  const auto idempotent = state.Finalize(1, true, 50);
  ASSERT_TRUE(idempotent);
  EXPECT_EQ(idempotent->updatedAtMs, repeated->updatedAtMs);
  EXPECT_TRUE(idempotent->completed);
  EXPECT_FALSE(state.UpdateProgress(1, 0, 0, 50));

  ASSERT_TRUE(state.AdvanceGeneration(2));
  EXPECT_FALSE(state.Finalize(1, false, 60));
  EXPECT_FALSE(state.UpdateProgress(1, 1, 2, 60));
  ASSERT_TRUE(state.Activate(Identity(2, "profile-two", 'b'), 70));
  EXPECT_FALSE(state.Finalize(2, false, 80));
}

TEST(TestJumpgatePlaybackHistoryState, IgnoresTransientZeroClockAfterPositiveProgress)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.Activate(Identity(1, "profile-one", 'a'), 10));
  ASSERT_TRUE(state.UpdateProgress(1, 500, 1000, 20));
  EXPECT_FALSE(state.UpdateProgress(1, 0, 0, 21));
  const auto snapshot = state.Finalize(1, false, 22);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->positionMs, 500);
  EXPECT_EQ(snapshot->durationMs, 1000);
}

TEST(TestJumpgatePlaybackHistoryState, CancelsResumeBeforeStateMutationAndSeek)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.Activate(Identity(1, "profile-one", 'a'), 10));
  const auto token = state.BeginResume(1);
  ASSERT_TRUE(token);
  ASSERT_TRUE(state.AdvanceGeneration(2));

  bool applied = false;
  EXPECT_FALSE(state.ApplyResume(*token, 1234, [&applied](int64_t) { applied = true; }));
  EXPECT_FALSE(applied);
}

TEST(TestJumpgatePlaybackHistoryState, GenerationCannotAdvanceDuringResumeApplication)
{
  CJumpgatePlaybackHistoryState state;
  ASSERT_TRUE(state.AdvanceGeneration(1));
  ASSERT_TRUE(state.Activate(Identity(1, "profile-one", 'a'), 10));
  const auto token = state.BeginResume(1);
  ASSERT_TRUE(token);

  std::mutex mutex;
  std::condition_variable condition;
  bool callbackEntered = false;
  bool releaseCallback = false;
  std::atomic<bool> advanceAttempted{false};
  std::atomic<bool> generationAdvanced{false};
  std::thread applyThread(
      [&]
      {
        EXPECT_TRUE(state.ApplyResume(*token, 1234,
                                      [&](int64_t positionMs)
                                      {
                                        EXPECT_EQ(positionMs, 1234);
                                        std::unique_lock<std::mutex> lock(mutex);
                                        callbackEntered = true;
                                        condition.notify_all();
                                        condition.wait(lock, [&] { return releaseCallback; });
                                      }));
      });
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return callbackEntered; });
  }
  std::thread advanceThread(
      [&]
      {
        advanceAttempted.store(true);
        generationAdvanced.store(state.AdvanceGeneration(2));
      });
  while (!advanceAttempted.load())
    std::this_thread::yield();
  EXPECT_FALSE(generationAdvanced.load());
  {
    std::lock_guard<std::mutex> lock(mutex);
    releaseCallback = true;
  }
  condition.notify_all();
  applyThread.join();
  advanceThread.join();
  EXPECT_TRUE(generationAdvanced.load());
}

TEST(TestJumpgatePlaybackHistoryState, ParsesPositiveInt64WithoutNarrowingOrOverflow)
{
  const auto maximum = ParseJumpgatePositiveInt64("9223372036854775807");
  ASSERT_TRUE(maximum);
  EXPECT_EQ(*maximum, std::numeric_limits<int64_t>::max());
  EXPECT_FALSE(ParseJumpgatePositiveInt64("9223372036854775808"));
  EXPECT_FALSE(ParseJumpgatePositiveInt64("0"));
  EXPECT_FALSE(ParseJumpgatePositiveInt64("-1"));
  EXPECT_FALSE(ParseJumpgatePositiveInt64("12ms"));
}

TEST(TestJumpgatePlaybackHistoryState, ResumeCorrectionWindowUsesElapsedTransportTime)
{
  EXPECT_TRUE(IsJumpgateResumeCorrectionWithinWindow(0, 999999999, 60000));
  EXPECT_TRUE(IsJumpgateResumeCorrectionWithinWindow(1000, 61000, 60000));
  EXPECT_FALSE(IsJumpgateResumeCorrectionWithinWindow(1000, 61001, 60000));
  EXPECT_FALSE(IsJumpgateResumeCorrectionWithinWindow(1000, 999, 60000));
  EXPECT_FALSE(IsJumpgateResumeCorrectionWithinWindow(-1, 1000, 60000));
  EXPECT_FALSE(IsJumpgateResumeCorrectionWithinWindow(1000, 1001, -1));
}
