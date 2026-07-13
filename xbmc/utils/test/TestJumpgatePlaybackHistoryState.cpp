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
  identity.profileId = profileId;
  identity.contentKey = std::string(64, keyCharacter);
  identity.display.title = "Generation-bound title";
  return identity;
}
} // namespace

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
