/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackAuthority.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;
using namespace std::chrono_literals;

TEST(TestJumpgatePlaybackAuthority, SerializesAdmissionWithProfileMutation)
{
  CJumpgatePlaybackAuthority authority;
  std::promise<void> admissionEntered;
  std::promise<void> releaseAdmission;
  std::atomic<bool> mutationEntered{false};

  std::thread admission(
      [&]
      {
        auto transaction = authority.BeginTransaction();
        admissionEntered.set_value();
        releaseAdmission.get_future().wait();
        transaction.CommitAdmission(1);
      });
  admissionEntered.get_future().wait();

  std::thread mutation(
      [&]
      {
        auto transaction = authority.BeginTransaction();
        mutationEntered.store(true);
        EXPECT_FALSE(transaction.CanMutateProfile());
      });
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(mutationEntered.load());
  releaseAdmission.set_value();
  admission.join();
  mutation.join();
  EXPECT_TRUE(mutationEntered.load());
}

TEST(TestJumpgatePlaybackAuthority, SerializesClaimApplicationWithProfileMutation)
{
  CJumpgatePlaybackAuthority authority;
  std::promise<void> applicationEntered;
  std::promise<void> releaseApplication;
  std::atomic<bool> mutationEntered{false};

  std::thread application(
      [&]
      {
        auto transaction = authority.BeginTransaction();
        applicationEntered.set_value();
        releaseApplication.get_future().wait();
      });
  applicationEntered.get_future().wait();

  std::thread mutation(
      [&]
      {
        auto transaction = authority.BeginTransaction();
        mutationEntered.store(true);
        EXPECT_TRUE(transaction.CanMutateProfile());
      });
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(mutationEntered.load());
  releaseApplication.set_value();
  application.join();
  mutation.join();
  EXPECT_TRUE(mutationEntered.load());
}

TEST(TestJumpgatePlaybackAuthority, OldStopBeforeReplacementStartPreservesAdmission)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Event oldAdmission;
  CJumpgatePlaybackAuthority::Event replacementAdmission;
  {
    auto transaction = authority.BeginTransaction();
    const auto admitted = transaction.CommitAdmission(1);
    ASSERT_TRUE(admitted);
    oldAdmission = *admitted;
    const auto started = transaction.CommitPlaybackStarted(1, oldAdmission.token);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->generation, 1u);
    const auto replacement = transaction.CommitAdmission(2);
    ASSERT_TRUE(replacement);
    replacementAdmission = *replacement;
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackStopped(oldAdmission.token);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 1u);
    EXPECT_FALSE(transaction.CanMutateProfile());
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto started = transaction.CommitPlaybackStarted(2, replacementAdmission.token);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->generation, 2u);
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackStopped(replacementAdmission.token);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 2u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, DelayedOldStopCannotDeactivateStartedReplacement)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token oldToken = 0;
  CJumpgatePlaybackAuthority::Token replacementToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto oldAdmission = transaction.CommitAdmission(1);
    ASSERT_TRUE(oldAdmission);
    oldToken = oldAdmission->token;
    ASSERT_TRUE(transaction.CommitPlaybackStarted(1, oldToken));
    const auto replacementAdmission = transaction.CommitAdmission(2);
    ASSERT_TRUE(replacementAdmission);
    const auto replacement = transaction.CommitPlaybackStarted(2, replacementAdmission->token);
    ASSERT_TRUE(replacement);
    replacementToken = replacement->token;
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackStopped(oldToken);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 1u);
    EXPECT_EQ(transaction.GetActiveToken(), replacementToken);
    EXPECT_FALSE(transaction.CanMutateProfile());
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackStopped(replacementToken);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 2u);
    EXPECT_EQ(transaction.GetActiveToken(), 0u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, ResumeKeepsExistingTokenAndStopAllowsMutation)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Event started;
  {
    auto transaction = authority.BeginTransaction();
    const auto admission = transaction.CommitAdmission(9);
    ASSERT_TRUE(admission);
    const auto startedEvent = transaction.CommitPlaybackStarted(9, admission->token);
    ASSERT_TRUE(startedEvent);
    started = *startedEvent;
    const auto resumed = transaction.CommitPlaybackResumed();
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->token, started.token);
    EXPECT_EQ(resumed->generation, started.generation);
    EXPECT_EQ(transaction.GetActiveToken(), started.token);
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackStopped(started.token);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->token, started.token);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, ResumeWithoutActivePlaybackDoesNotAllocateToken)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  EXPECT_FALSE(transaction.CommitPlaybackResumed());
  EXPECT_EQ(transaction.GetActiveToken(), 0u);
  EXPECT_TRUE(transaction.CanMutateProfile());
}

TEST(TestJumpgatePlaybackAuthority, CanceledPendingAdmissionAllowsProfileMutation)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto admitted = transaction.CommitAdmission(11);
  ASSERT_TRUE(admitted);
  EXPECT_TRUE(transaction.IsLatestPendingAdmission(admitted->token));
  EXPECT_FALSE(transaction.IsLatestPendingAdmission(0));
  EXPECT_FALSE(transaction.IsLatestPendingAdmission(admitted->token + 1));
  EXPECT_FALSE(transaction.CanMutateProfile());
  const auto canceled = transaction.CancelPendingAdmission(11);
  ASSERT_TRUE(canceled);
  EXPECT_EQ(canceled->token, admitted->token);
  EXPECT_FALSE(transaction.IsLatestPendingAdmission(admitted->token));
  EXPECT_TRUE(transaction.CanMutateProfile());
}

TEST(TestJumpgatePlaybackAuthority, CancelTargetsGenerationWithoutRemovingReplacement)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto first = transaction.CommitAdmission(1);
  const auto replacement = transaction.CommitAdmission(2);
  ASSERT_TRUE(first);
  ASSERT_TRUE(replacement);
  EXPECT_FALSE(transaction.IsLatestPendingAdmission(first->token));
  EXPECT_TRUE(transaction.IsLatestPendingAdmission(replacement->token));
  const auto canceled = transaction.CancelPendingAdmission(1);
  ASSERT_TRUE(canceled);
  EXPECT_EQ(canceled->generation, 1u);
  const auto started = transaction.CommitPlaybackStarted(2, replacement->token);
  ASSERT_TRUE(started);
  EXPECT_EQ(started->generation, 2u);
}

TEST(TestJumpgatePlaybackAuthority, InternalContinuationRetainsExternalGeneration)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token initialToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto admission = transaction.CommitAdmission(21);
    ASSERT_TRUE(admission);
    initialToken = admission->token;
    const auto started = transaction.CommitPlaybackStarted(21, initialToken);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->generation, 21u);
    ASSERT_TRUE(transaction.CommitPlaybackStopped(initialToken));
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto started = transaction.CommitPlaybackStarted(21);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->generation, 21u);
    const auto stopped = transaction.CommitPlaybackStopped(started->token);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 21u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, ProfileMutationTokenBlocksAdmissionAfterLockRelease)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token mutationToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto token = transaction.BeginProfileMutation();
    ASSERT_TRUE(token);
    mutationToken = *token;
  }

  {
    auto transaction = authority.BeginTransaction();
    EXPECT_FALSE(transaction.CanAdmitPlayback());
    EXPECT_FALSE(transaction.CommitAdmission(12));
    EXPECT_FALSE(transaction.CommitPlaybackStarted(12));
  }

  {
    auto transaction = authority.BeginTransaction();
    EXPECT_TRUE(transaction.CommitProfileMutation(mutationToken));
    EXPECT_TRUE(transaction.CanAdmitPlayback());
    EXPECT_TRUE(transaction.CommitAdmission(12));
  }
}

TEST(TestJumpgatePlaybackAuthority, StaleProfileMutationCompletionCannotUnblockNewMutation)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token firstToken = 0;
  CJumpgatePlaybackAuthority::Token secondToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto token = transaction.BeginProfileMutation();
    ASSERT_TRUE(token);
    firstToken = *token;
    EXPECT_TRUE(transaction.CommitProfileMutation(firstToken));
    const auto next = transaction.BeginProfileMutation();
    ASSERT_TRUE(next);
    secondToken = *next;
    EXPECT_FALSE(transaction.RollbackProfileMutation(firstToken));
    EXPECT_FALSE(transaction.CanAdmitPlayback());
  }
  {
    auto transaction = authority.BeginTransaction();
    EXPECT_TRUE(transaction.RollbackProfileMutation(secondToken));
    EXPECT_TRUE(transaction.CanAdmitPlayback());
  }
}

TEST(TestJumpgatePlaybackAuthority, AdmissionWinsBeforeProfileMutationAndMustTerminateFirst)
{
  CJumpgatePlaybackAuthority authority;
  {
    auto transaction = authority.BeginTransaction();
    ASSERT_TRUE(transaction.CommitAdmission(31));
    EXPECT_FALSE(transaction.BeginProfileMutation());
  }
  {
    auto transaction = authority.BeginTransaction();
    ASSERT_TRUE(transaction.CancelPendingAdmission(31));
    EXPECT_TRUE(transaction.BeginProfileMutation());
  }
}

TEST(TestJumpgatePlaybackAuthority, ExactOldThenNewTerminalPreservesReplacementOrdering)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token oldToken = 0;
  CJumpgatePlaybackAuthority::Token newToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto oldAdmission = transaction.CommitAdmission(41);
    ASSERT_TRUE(oldAdmission);
    oldToken = oldAdmission->token;
    ASSERT_TRUE(transaction.CommitPlaybackStarted(41, oldToken));
    const auto newAdmission = transaction.CommitAdmission(42);
    ASSERT_TRUE(newAdmission);
    newToken = newAdmission->token;
    ASSERT_TRUE(transaction.CommitPlaybackStarted(42, newToken));
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto oldTerminal = transaction.CommitPlaybackStopped(oldToken);
    ASSERT_TRUE(oldTerminal);
    EXPECT_EQ(oldTerminal->generation, 41u);
    EXPECT_EQ(transaction.GetActiveToken(), newToken);
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto newTerminal = transaction.CommitPlaybackStopped(newToken);
    ASSERT_TRUE(newTerminal);
    EXPECT_EQ(newTerminal->generation, 42u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, ExactNewTerminalPurgesOldCallbackWhoseMessageWasRemoved)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token newToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto oldAdmission = transaction.CommitAdmission(51);
    ASSERT_TRUE(oldAdmission);
    ASSERT_TRUE(transaction.CommitPlaybackStarted(51, oldAdmission->token));
    const auto newAdmission = transaction.CommitAdmission(52);
    ASSERT_TRUE(newAdmission);
    newToken = newAdmission->token;
    ASSERT_TRUE(transaction.CommitPlaybackStarted(52, newToken));
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto terminal = transaction.CommitPlaybackStopped(newToken);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->generation, 52u);
    EXPECT_EQ(transaction.GetActiveToken(), 0u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, ExactPendingTokenCancellationCannotRemoveReplacement)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto first = transaction.CommitAdmission(61);
  const auto replacement = transaction.CommitAdmission(62);
  ASSERT_TRUE(first);
  ASSERT_TRUE(replacement);

  const auto canceled = transaction.CancelPendingAdmissionByToken(first->token);
  ASSERT_TRUE(canceled);
  EXPECT_EQ(canceled->generation, 61u);
  const auto started = transaction.CommitPlaybackStarted(62, replacement->token);
  ASSERT_TRUE(started);
  EXPECT_EQ(started->token, replacement->token);
}

TEST(TestJumpgatePlaybackAuthority, ExactStartPurgesSupersededPendingCallback)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Token oldToken = 0;
  CJumpgatePlaybackAuthority::Token replacementToken = 0;
  {
    auto transaction = authority.BeginTransaction();
    const auto old = transaction.CommitAdmission(71);
    const auto replacement = transaction.CommitAdmission(72);
    ASSERT_TRUE(old);
    ASSERT_TRUE(replacement);
    oldToken = old->token;
    replacementToken = replacement->token;

    const auto started = transaction.CommitPlaybackStarted(0, replacementToken);
    ASSERT_TRUE(started);
    EXPECT_EQ(started->generation, 72u);
    EXPECT_FALSE(transaction.CancelPendingAdmissionByToken(oldToken));
    EXPECT_EQ(transaction.GetActiveToken(), replacementToken);
  }
  {
    auto transaction = authority.BeginTransaction();
    EXPECT_FALSE(transaction.CommitPlaybackStopped(oldToken));
    const auto stopped = transaction.CommitPlaybackStopped(replacementToken);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, 72u);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, DelayedSupersededStartCannotStealActiveWinner)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto oldAdmission = transaction.CommitAdmission(81);
  const auto replacementAdmission = transaction.CommitAdmission(82);
  ASSERT_TRUE(oldAdmission);
  ASSERT_TRUE(replacementAdmission);

  const auto replacementStart = transaction.CommitPlaybackStarted(82, replacementAdmission->token);
  ASSERT_TRUE(replacementStart);
  EXPECT_EQ(replacementAdmission->token, transaction.GetActiveToken());

  EXPECT_FALSE(transaction.CommitPlaybackStarted(82, oldAdmission->token));
  EXPECT_EQ(replacementAdmission->token, transaction.GetActiveToken());

  const auto replacementStop = transaction.CommitPlaybackStopped(replacementAdmission->token);
  ASSERT_TRUE(replacementStop);
  EXPECT_EQ(82u, replacementStop->generation);
  EXPECT_TRUE(transaction.CanMutateProfile());
}

TEST(TestJumpgatePlaybackAuthority, CanceledAdmissionTokenCannotRestart)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto admission = transaction.CommitAdmission(91);
  ASSERT_TRUE(admission);
  ASSERT_TRUE(transaction.CancelPendingAdmissionByToken(admission->token));

  EXPECT_FALSE(transaction.CommitPlaybackStarted(91, admission->token));
  EXPECT_EQ(0u, transaction.GetActiveToken());
  EXPECT_TRUE(transaction.CanMutateProfile());
}

TEST(TestJumpgatePlaybackAuthority, ExactStoppedTokenCanContinueOnlyItsGeneration)
{
  CJumpgatePlaybackAuthority authority;
  auto transaction = authority.BeginTransaction();
  const auto admission = transaction.CommitAdmission(101);
  ASSERT_TRUE(admission);
  ASSERT_TRUE(transaction.CommitPlaybackStarted(101, admission->token));
  ASSERT_TRUE(transaction.CommitPlaybackStopped(admission->token));

  EXPECT_FALSE(transaction.CommitPlaybackStarted(102, admission->token));
  const auto continuation = transaction.CommitPlaybackStarted(101, admission->token);
  ASSERT_TRUE(continuation);
  EXPECT_EQ(101u, continuation->generation);
  EXPECT_EQ(admission->token, continuation->token);
}

TEST(TestJumpgatePlaybackAuthority, StartedButUnannouncedTerminalClearsReplacedPlayback)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Event replacement;
  {
    auto transaction = authority.BeginTransaction();
    const auto oldAdmission = transaction.CommitAdmission(111);
    ASSERT_TRUE(oldAdmission);
    ASSERT_TRUE(transaction.CommitPlaybackStarted(111, oldAdmission->token));

    const auto replacementAdmission = transaction.CommitAdmission(112);
    ASSERT_TRUE(replacementAdmission);
    replacement = *replacementAdmission;
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto terminal = transaction.CommitPlaybackTerminal(replacement.token, true);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->generation, replacement.generation);
    EXPECT_EQ(transaction.GetActiveToken(), 0u);
    EXPECT_FALSE(transaction.CommitPlaybackStarted(replacement.generation, replacement.token));
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}

TEST(TestJumpgatePlaybackAuthority, NeverStartedFailureCancelsOnlyExactPendingAdmission)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAuthority::Event oldPlayback;
  CJumpgatePlaybackAuthority::Event replacement;
  {
    auto transaction = authority.BeginTransaction();
    const auto oldAdmission = transaction.CommitAdmission(121);
    ASSERT_TRUE(oldAdmission);
    oldPlayback = *oldAdmission;
    ASSERT_TRUE(transaction.CommitPlaybackStarted(121, oldPlayback.token));

    const auto replacementAdmission = transaction.CommitAdmission(122);
    ASSERT_TRUE(replacementAdmission);
    replacement = *replacementAdmission;
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto failed = transaction.CommitPlaybackTerminal(replacement.token, false);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->generation, replacement.generation);
    EXPECT_EQ(transaction.GetActiveToken(), oldPlayback.token);
    EXPECT_FALSE(transaction.CanMutateProfile());
  }
  {
    auto transaction = authority.BeginTransaction();
    const auto oldTerminal = transaction.CommitPlaybackTerminal(oldPlayback.token, true);
    ASSERT_TRUE(oldTerminal);
    EXPECT_EQ(oldTerminal->generation, oldPlayback.generation);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
}
