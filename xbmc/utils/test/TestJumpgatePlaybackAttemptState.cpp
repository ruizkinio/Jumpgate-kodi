/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/PlayerOpenPublication.h"
#include "utils/JumpgatePlaybackAttemptState.h"
#include "utils/JumpgatePlaybackAuthority.h"
#include "utils/JumpgatePlaybackResultState.h"

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{

CJumpgatePlaybackAuthority::Event Admit(CJumpgatePlaybackAuthority& authority, uint64_t generation)
{
  auto transaction = authority.BeginTransaction();
  const auto admission = transaction.CommitAdmission(generation);
  EXPECT_TRUE(admission);
  return admission.value_or(CJumpgatePlaybackAuthority::Event{});
}

} // namespace

TEST(TestJumpgatePlaybackAttemptState, DeferredOpenNextFailureCancelsBoundAdmissionExactlyOnce)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAttemptState attempts;
  CJumpgatePlaybackResultState results;

  const auto admission = Admit(authority, 41);
  ASSERT_TRUE(results.Begin(admission.generation));

  // CApplicationPlayer performs this transition before closing the active player.
  ASSERT_TRUE(attempts.Bind(admission.token, JumpgatePlaybackOpenMode::Deferred));
  EXPECT_EQ(attempts.PendingAttemptCount(), 1u);
  EXPECT_FALSE(attempts.MarkStarted(admission.token));

  // CApplicationPlayer::OpenNext uses this production helper to publish the exact deferred
  // transition before OpenFileInternal can report failure.
  int transitionStep = 0;
  const bool opened = KODI::PLAYER::BeginDeferredOpenAndOpen(
      [&]
      {
        EXPECT_EQ(++transitionStep, 1);
        EXPECT_TRUE(attempts.BeginOpenNext(admission.token));
      },
      [&]
      {
        EXPECT_EQ(++transitionStep, 2);
        EXPECT_TRUE(attempts.CancelOpen(admission.token));
        return false;
      });
  EXPECT_FALSE(opened);
  EXPECT_FALSE(attempts.BeginOpenNext(admission.token));
  EXPECT_FALSE(attempts.CancelOpen(admission.token));
  EXPECT_EQ(attempts.PendingAttemptCount(), 0u);

  {
    auto transaction = authority.BeginTransaction();
    const auto canceled = transaction.CancelPendingAdmissionByToken(admission.token);
    ASSERT_TRUE(canceled);
    EXPECT_EQ(canceled->generation, admission.generation);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }

  ASSERT_TRUE(results.Finish(admission.generation, 0, 0, false));
  const auto delivery = results.TakeFinished();
  ASSERT_TRUE(delivery);
  EXPECT_EQ(delivery->generation, admission.generation);
  EXPECT_FALSE(results.TakeFinished());
}

TEST(TestJumpgatePlaybackAttemptState, UpnpPreparationFailureVerifiesRollbackBeforeExactOpenFailure)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAttemptState attempts;
  CJumpgatePlaybackResultState results;
  const auto admission = Admit(authority, 42);
  ASSERT_TRUE(results.Begin(admission.generation));
  ASSERT_TRUE(attempts.Bind(admission.token));

  int transportQueries = 0;
  int stopCalls = 0;
  int verifyCalls = 0;
  int retainedItems = 0;
  int startedCallbacks = 0;
  int avStartedCallbacks = 0;
  bool remotePlaying = true;
  const bool opened = KODI::PLAYER::PrepareOpenAndPublishStarted(
      admission.token, true,
      [&]
      {
        ++transportQueries;
        return true;
      },
      [&]
      {
        ++transportQueries;
        return false;
      },
      [&]
      {
        ++stopCalls;
        remotePlaying = false;
        return true;
      },
      [&]
      {
        ++verifyCalls;
        return !remotePlaying;
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(token, admission.token);
        ++retainedItems;
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(token, admission.token);
        ++startedCallbacks;
        attempts.MarkStarted(token);
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(token, admission.token);
        ++avStartedCallbacks;
      });

  EXPECT_FALSE(opened);
  EXPECT_EQ(transportQueries, 2);
  EXPECT_EQ(stopCalls, 1);
  EXPECT_EQ(verifyCalls, 1);
  EXPECT_FALSE(remotePlaying);
  EXPECT_EQ(retainedItems, 0);
  EXPECT_EQ(startedCallbacks, 0);
  EXPECT_EQ(avStartedCallbacks, 0);
  ASSERT_TRUE(attempts.CancelOpen(admission.token));
  {
    auto transaction = authority.BeginTransaction();
    const auto canceled = transaction.CancelPendingAdmissionByToken(admission.token);
    ASSERT_TRUE(canceled);
    EXPECT_EQ(canceled->generation, admission.generation);
  }
  ASSERT_TRUE(results.Finish(admission.generation, false));
  EXPECT_FALSE(attempts.MarkStarted(admission.token));
}

TEST(TestJumpgatePlaybackAttemptState, UpnpSuccessfulPreparationPublishesExactAttemptOnce)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAttemptState attempts;
  const auto admission = Admit(authority, 43);
  ASSERT_TRUE(attempts.Bind(admission.token));

  int transitionStep = 0;
  const bool opened = KODI::PLAYER::PrepareOpenAndPublishStarted(
      admission.token, true,
      [&]
      {
        EXPECT_EQ(++transitionStep, 1);
        return true;
      },
      [&]
      {
        EXPECT_EQ(++transitionStep, 2);
        return true;
      },
      [&]
      {
        ADD_FAILURE() << "normal admission must not issue Stop";
        return false;
      },
      [&]
      {
        ADD_FAILURE() << "normal admission must not verify Stop";
        return false;
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(++transitionStep, 3);
        EXPECT_EQ(token, admission.token);
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(++transitionStep, 4);
        EXPECT_TRUE(attempts.MarkStarted(token));
      },
      [&](const uint64_t& token)
      {
        EXPECT_EQ(++transitionStep, 5);
        EXPECT_EQ(token, admission.token);
      });

  ASSERT_TRUE(opened);
  EXPECT_EQ(transitionStep, 5);
  const auto emitted = attempts.EmitTerminal(admission.token, false);
  ASSERT_TRUE(emitted);
  EXPECT_EQ(emitted->token, admission.token);
  EXPECT_TRUE(emitted->started);
  EXPECT_FALSE(emitted->superseded);
  EXPECT_FALSE(attempts.EmitTerminal(admission.token, false));

  const auto acknowledged = attempts.AcknowledgeTerminal(admission.token, false);
  ASSERT_TRUE(acknowledged);
  EXPECT_EQ(acknowledged->token, admission.token);
  EXPECT_TRUE(acknowledged->started);
  EXPECT_FALSE(attempts.AcknowledgeTerminal(admission.token, false));
}

TEST(TestJumpgatePlaybackAttemptState, PreStartAsyncFailureAndOldErrorCannotConsumeReplacement)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAttemptState attempts;
  CJumpgatePlaybackResultState results;

  const auto oldAdmission = Admit(authority, 51);
  ASSERT_TRUE(attempts.Bind(oldAdmission.token));
  ASSERT_TRUE(attempts.MarkStarted(oldAdmission.token));
  EXPECT_FALSE(attempts.CancelOpen(oldAdmission.token));
  {
    auto transaction = authority.BeginTransaction();
    ASSERT_TRUE(transaction.CommitPlaybackStarted(oldAdmission.generation, oldAdmission.token));
  }

  const auto replacement = Admit(authority, 52);
  ASSERT_TRUE(results.Begin(replacement.generation));
  ASSERT_TRUE(attempts.Bind(replacement.token));

  const auto oldError = attempts.EmitTerminal(oldAdmission.token, false);
  ASSERT_TRUE(oldError);
  EXPECT_TRUE(oldError->started);
  EXPECT_TRUE(oldError->superseded);
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackTerminal(oldError->token, oldError->started);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->generation, oldAdmission.generation);
    EXPECT_FALSE(transaction.CanMutateProfile());
  }
  EXPECT_FALSE(results.Finish(oldAdmission.generation, false));
  EXPECT_EQ(attempts.PendingAttemptCount(), 1u);

  const auto replacementFailure = attempts.EmitTerminal(replacement.token, false);
  ASSERT_TRUE(replacementFailure);
  EXPECT_FALSE(replacementFailure->started);
  EXPECT_FALSE(replacementFailure->superseded);
  {
    auto transaction = authority.BeginTransaction();
    const auto canceled =
        transaction.CommitPlaybackTerminal(replacementFailure->token, replacementFailure->started);
    ASSERT_TRUE(canceled);
    EXPECT_EQ(canceled->generation, replacement.generation);
    EXPECT_TRUE(transaction.CanMutateProfile());
  }
  ASSERT_TRUE(results.Finish(replacement.generation, false));
}

TEST(TestJumpgatePlaybackAttemptState,
     OlderPreStartOpenFailureCannotFinishSameGenerationReplacement)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackAttemptState attempts;
  CJumpgatePlaybackResultState results;

  const auto oldAdmission = Admit(authority, 53);
  ASSERT_TRUE(attempts.Bind(oldAdmission.token, JumpgatePlaybackOpenMode::Deferred));
  const auto replacement = Admit(authority, oldAdmission.generation);
  ASSERT_TRUE(results.Begin(replacement.generation));
  ASSERT_TRUE(attempts.Bind(replacement.token));

  ASSERT_TRUE(attempts.CancelOpen(oldAdmission.token));
  bool superseded = false;
  {
    auto transaction = authority.BeginTransaction();
    const auto canceled = transaction.CancelPendingAdmissionByToken(oldAdmission.token);
    ASSERT_TRUE(canceled);
    superseded = transaction.HasNewerPlayback(canceled->token);
    if (!superseded)
      results.Finish(canceled->generation, false);
  }

  EXPECT_TRUE(superseded);
  EXPECT_FALSE(results.TakeFinished());
  EXPECT_TRUE(results.IsCurrent(replacement.generation));
  EXPECT_EQ(attempts.PendingAttemptCount(), 1u);
  EXPECT_TRUE(attempts.MarkStarted(replacement.token));
}

TEST(TestJumpgatePlaybackAttemptState, RemovedOldTerminalRecoveryAcknowledgesOnlyItsToken)
{
  CJumpgatePlaybackAuthority authority;
  CJumpgatePlaybackResultState results;
  CJumpgatePlaybackAttemptState attempts;
  const auto oldAdmission = Admit(authority, 61);
  ASSERT_TRUE(results.Begin(oldAdmission.generation));
  ASSERT_TRUE(attempts.Bind(oldAdmission.token));
  ASSERT_TRUE(attempts.MarkStarted(oldAdmission.token));
  {
    auto transaction = authority.BeginTransaction();
    ASSERT_TRUE(transaction.CommitPlaybackStarted(oldAdmission.generation, oldAdmission.token));
  }

  const auto replacementAdmission = Admit(authority, oldAdmission.generation);
  ASSERT_TRUE(results.Begin(replacementAdmission.generation));
  ASSERT_TRUE(attempts.Bind(replacementAdmission.token));

  const auto removedOldMessage = DecodeJumpgateRemovedPlaybackTerminal(1, 1, 2, oldAdmission.token);
  ASSERT_TRUE(removedOldMessage);
  const auto removedOld =
      attempts.EmitTerminal(removedOldMessage->token, removedOldMessage->completed);
  ASSERT_TRUE(removedOld);

  const auto recovered = attempts.AcknowledgeTerminal(removedOld->token, removedOld->completed);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->token, oldAdmission.token);
  EXPECT_TRUE(recovered->started);
  EXPECT_TRUE(recovered->superseded);
  EXPECT_FALSE(attempts.AcknowledgeTerminal(oldAdmission.token, false));
  EXPECT_FALSE(DecodeJumpgateRemovedPlaybackTerminal(3, 1, 2, replacementAdmission.token));
  EXPECT_FALSE(DecodeJumpgateRemovedPlaybackTerminal(2, 1, 2, 0));

  bool oldWasSuperseded = false;
  {
    auto transaction = authority.BeginTransaction();
    const auto stopped = transaction.CommitPlaybackTerminal(recovered->token, recovered->started);
    ASSERT_TRUE(stopped);
    oldWasSuperseded = transaction.HasNewerPlayback(stopped->token);
  }
  EXPECT_TRUE(oldWasSuperseded);
  if (!oldWasSuperseded)
    results.Finish(oldAdmission.generation, false);
  EXPECT_FALSE(results.TakeFinished());
  EXPECT_TRUE(results.IsCurrent(replacementAdmission.generation));

  ASSERT_TRUE(attempts.MarkStarted(replacementAdmission.token));
  {
    auto transaction = authority.BeginTransaction();
    ASSERT_TRUE(transaction.CommitPlaybackStarted(replacementAdmission.generation,
                                                  replacementAdmission.token));
  }
  const auto queuedReplacement = attempts.EmitTerminal(replacementAdmission.token, true);
  ASSERT_TRUE(queuedReplacement);

  // The newer message was not removed, so exact recovery must leave it queued.
  const auto replacement = attempts.AcknowledgeTerminal(replacementAdmission.token, true);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(replacement->token, replacementAdmission.token);
  EXPECT_TRUE(replacement->started);
  EXPECT_FALSE(replacement->superseded);
  {
    auto transaction = authority.BeginTransaction();
    const auto terminal =
        transaction.CommitPlaybackTerminal(replacement->token, replacement->started);
    ASSERT_TRUE(terminal);
    EXPECT_FALSE(transaction.HasNewerPlayback(terminal->token));
    ASSERT_TRUE(results.Finish(terminal->generation, true));
  }
  ASSERT_TRUE(results.TakeFinished());
}

TEST(TestJumpgatePlaybackAttemptState, TokenlessAndStaleCallbacksCannotConsumeQueueHeads)
{
  CJumpgatePlaybackAttemptState attempts;
  ASSERT_TRUE(attempts.Bind(71));
  ASSERT_TRUE(attempts.Bind(72));
  EXPECT_FALSE(attempts.MarkStarted(71));
  EXPECT_TRUE(attempts.MarkStarted(72));
  EXPECT_FALSE(attempts.EmitTerminal(0, false));
  EXPECT_FALSE(attempts.EmitTerminal(999, false));
  EXPECT_EQ(attempts.PendingAttemptCount(), 2u);

  CJumpgatePlaybackAuthority authority;
  const auto admission = Admit(authority, 72);
  auto transaction = authority.BeginTransaction();
  EXPECT_FALSE(transaction.CommitPlaybackStarted());
  EXPECT_FALSE(transaction.CommitPlaybackStarted(72, 999));
  EXPECT_FALSE(transaction.CommitPlaybackStopped());
  const auto started = transaction.CommitPlaybackStarted(72, admission.token);
  ASSERT_TRUE(started);
  EXPECT_FALSE(transaction.CommitPlaybackStopped());
  EXPECT_EQ(transaction.GetActiveToken(), admission.token);
  EXPECT_TRUE(transaction.CommitPlaybackStopped(admission.token));
  EXPECT_TRUE(transaction.CanMutateProfile());
}

TEST(TestJumpgatePlaybackAttemptState, DelayedOldTerminalPreservesNewOpening)
{
  CJumpgatePlaybackAttemptState attempts;
  ASSERT_TRUE(attempts.Bind(81));
  ASSERT_TRUE(attempts.MarkStarted(81));
  ASSERT_TRUE(attempts.Bind(82));

  const auto oldTerminal = attempts.EmitTerminal(81, false);
  ASSERT_TRUE(oldTerminal);
  EXPECT_TRUE(oldTerminal->started);
  EXPECT_TRUE(oldTerminal->superseded);
  EXPECT_EQ(attempts.PendingAttemptCount(), 1u);

  ASSERT_TRUE(attempts.MarkStarted(82));
  const auto replacementTerminal = attempts.EmitTerminal(82, false);
  ASSERT_TRUE(replacementTerminal);
  EXPECT_TRUE(replacementTerminal->started);
  EXPECT_FALSE(replacementTerminal->superseded);
  EXPECT_EQ(attempts.PendingAttemptCount(), 0u);
}

TEST(TestJumpgatePlaybackAttemptState, DuplicateBindAndTerminalAreRejected)
{
  CJumpgatePlaybackAttemptState attempts;
  ASSERT_TRUE(attempts.Bind(91));
  EXPECT_FALSE(attempts.Bind(91));
  ASSERT_TRUE(attempts.MarkStarted(91));
  EXPECT_FALSE(attempts.MarkStarted(91));
  ASSERT_TRUE(attempts.EmitTerminal(91, true));
  EXPECT_FALSE(attempts.EmitTerminal(91, true));
}
