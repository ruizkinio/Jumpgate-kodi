/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackResultState.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{

constexpr const char* OLD_REQUEST_ID = "11111111-1111-4111-8111-111111111111";
constexpr const char* NEW_REQUEST_ID = "22222222-2222-4222-8222-222222222222";

} // namespace

TEST(TestJumpgatePlaybackResultState, EmitsCurrentExternalGenerationExactlyOnce)
{
  CJumpgatePlaybackResultState state;
  state.Begin(7);

  const auto result = state.Finish(7, 1234567890123LL, 2234567890123LL, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->generation, 7u);
  EXPECT_EQ(result->positionMs, 1234567890123LL);
  EXPECT_EQ(result->durationMs, 2234567890123LL);
  EXPECT_TRUE(result->completed);
  EXPECT_FALSE(state.Finish(7, 1, 2, false));
}

TEST(TestJumpgatePlaybackResultState, ReplacementRejectsDelayedOldTerminalCallback)
{
  CJumpgatePlaybackResultState state;
  state.Begin(1);
  state.Begin(2);

  EXPECT_FALSE(state.Finish(1, 100, 1000, false));
  EXPECT_TRUE(state.Finish(2, 200, 1000, false));
}

TEST(TestJumpgatePlaybackResultState, ReplacementSupersedesQueuedOldResultBeforeDelivery)
{
  CJumpgatePlaybackResultState state;
  state.Begin(1);
  const auto oldResult = state.Finish(1, 100, 1000, false);
  ASSERT_TRUE(oldResult);
  EXPECT_TRUE(state.IsCurrent(oldResult->generation));

  state.Begin(2);
  EXPECT_FALSE(state.IsCurrent(oldResult->generation));
  EXPECT_TRUE(state.IsCurrent(2));
  EXPECT_FALSE(state.TakeFinished());
}

TEST(TestJumpgatePlaybackResultState, StandaloneAndResetNeverEmitResults)
{
  CJumpgatePlaybackResultState state;
  EXPECT_FALSE(state.Finish(0, 100, 1000, false));
  EXPECT_FALSE(state.Finish(1, 100, 1000, false));
  state.Begin(1);
  state.Reset();
  EXPECT_FALSE(state.Finish(1, 100, 1000, false));
}

TEST(TestJumpgatePlaybackResultState, RejectedAdmissionReturnsCanceledZeroResult)
{
  CJumpgatePlaybackResultState state;
  state.Begin(3);
  const auto result = state.Finish(3, -1, -1, false);

  ASSERT_TRUE(result);
  EXPECT_EQ(result->positionMs, 0);
  EXPECT_EQ(result->durationMs, 0);
  EXPECT_FALSE(result->completed);
}

TEST(TestJumpgatePlaybackResultState, ContinuationReplacesProvisionalResultWithFinalSnapshot)
{
  CJumpgatePlaybackResultState state;
  state.Begin(4);
  const auto firstPart = state.Finish(4, 900, 1000, true);
  ASSERT_TRUE(firstPart);
  EXPECT_EQ(state.CurrentGeneration(), 4u);

  state.Begin(state.CurrentGeneration());
  const auto finalPart = state.Finish(4, 1900, 2000, true);
  ASSERT_TRUE(finalPart);
  EXPECT_EQ(finalPart->positionMs, 1900);
  EXPECT_EQ(finalPart->durationMs, 2000);
  EXPECT_FALSE(state.Finish(4, 1, 2, false));
}

TEST(TestJumpgatePlaybackResultState, CapturedClockIsGenerationBoundAndTerminalExactlyOnce)
{
  CJumpgatePlaybackResultState state;
  state.Begin(10);
  ASSERT_TRUE(state.Capture(10, 1234567890123LL, 2234567890123LL));
  state.Begin(11);

  EXPECT_FALSE(state.Capture(10, 1, 2));
  EXPECT_FALSE(state.Finish(10, false));
  ASSERT_TRUE(state.Capture(11, 3334567890123LL, 4334567890123LL));
  const auto result = state.Finish(11, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->positionMs, 3334567890123LL);
  EXPECT_EQ(result->durationMs, 4334567890123LL);
  EXPECT_TRUE(result->completed);
  EXPECT_FALSE(state.Finish(11, false));
}

TEST(TestJumpgatePlaybackResultState, FailedOpenProducesCanceledZeroSnapshot)
{
  CJumpgatePlaybackResultState state;
  state.Begin(12);
  ASSERT_TRUE(state.Capture(12, 0, 0));
  const auto result = state.Finish(12, false);

  ASSERT_TRUE(result);
  EXPECT_EQ(result->generation, 12u);
  EXPECT_EQ(result->positionMs, 0);
  EXPECT_EQ(result->durationMs, 0);
  EXPECT_FALSE(result->completed);
}

TEST(TestJumpgatePlaybackResultState, TransientZeroClockCannotEraseCapturedProgress)
{
  CJumpgatePlaybackResultState state;
  state.Begin(13);
  ASSERT_TRUE(state.Capture(13, 900, 1000));
  EXPECT_FALSE(state.Capture(13, 0, 0));
  const auto result = state.Finish(13, false);

  ASSERT_TRUE(result);
  EXPECT_EQ(result->positionMs, 900);
  EXPECT_EQ(result->durationMs, 1000);
}

TEST(TestJumpgatePlaybackResultState, DeliverySerializesAgainstNewIntentAdmission)
{
  CJumpgatePlaybackResultState state;
  state.Begin(20);
  ASSERT_TRUE(state.Finish(20, 200, 1000, false));

  std::mutex admissionMutex;
  std::condition_variable admissionCondition;
  bool admissionAttempting = false;
  std::atomic<bool> admissionCommitted{false};
  std::atomic<int> completedDeliverySideEffects{0};
  std::atomic<int> sideEffectsObservedByAdmission{0};
  std::thread admissionThread;

  {
    auto deliveryOperation = state.BeginLifecycleOperation();
    const auto oldResult = state.TakeFinished(deliveryOperation);
    ASSERT_TRUE(oldResult);
    ASSERT_EQ(oldResult->generation, 20u);
    EXPECT_TRUE(deliveryOperation.OwnsGeneration(20));

    admissionThread = std::thread(
        [&]
        {
          {
            std::lock_guard<std::mutex> lock(admissionMutex);
            admissionAttempting = true;
          }
          admissionCondition.notify_one();

          auto admissionOperation = state.BeginLifecycleOperation();
          sideEffectsObservedByAdmission.store(
              completedDeliverySideEffects.load(std::memory_order_acquire),
              std::memory_order_release);
          admissionCommitted.store(state.Begin(admissionOperation, 21), std::memory_order_release);
        });

    {
      std::unique_lock<std::mutex> lock(admissionMutex);
      admissionCondition.wait(lock, [&] { return admissionAttempting; });
    }
    EXPECT_FALSE(admissionCommitted.load(std::memory_order_acquire));
    EXPECT_FALSE(state.TryBeginLifecycleOperation());
    // Model the generation-owned native exit, claim release, Java finish, and
    // result reset that all run while CXBMCApp holds this same operation.
    completedDeliverySideEffects.fetch_add(1, std::memory_order_relaxed);
    completedDeliverySideEffects.fetch_add(1, std::memory_order_relaxed);
    completedDeliverySideEffects.fetch_add(1, std::memory_order_relaxed);
    EXPECT_TRUE(state.Reset(deliveryOperation, oldResult->generation));
    completedDeliverySideEffects.fetch_add(1, std::memory_order_release);
  }

  admissionThread.join();
  EXPECT_TRUE(admissionCommitted.load(std::memory_order_acquire));
  EXPECT_EQ(sideEffectsObservedByAdmission.load(std::memory_order_acquire), 4);
  EXPECT_TRUE(state.IsCurrent(21));
  EXPECT_FALSE(state.TakeFinished());
  EXPECT_FALSE(state.Finish(20, 1, 2, false));
}

TEST(TestJumpgatePlaybackResultState, NewIntentAdmissionWinnerSuppressesOldResultDelivery)
{
  CJumpgatePlaybackResultState state;
  ASSERT_TRUE(state.Begin(25));
  ASSERT_TRUE(state.Finish(25, 250, 1000, false));

  std::mutex deliveryMutex;
  std::condition_variable deliveryCondition;
  bool deliveryWaiting = false;
  std::atomic<bool> oldResultDelivered{false};
  std::thread delivery;

  {
    auto admissionOperation = state.BeginLifecycleOperation();
    ASSERT_TRUE(state.Begin(admissionOperation, 26));
    ASSERT_TRUE(admissionOperation.OwnsGeneration(26));

    delivery = std::thread(
        [&]
        {
          {
            std::lock_guard<std::mutex> lock(deliveryMutex);
            deliveryWaiting = true;
          }
          deliveryCondition.notify_one();

          auto deliveryOperation = state.BeginLifecycleOperation();
          oldResultDelivered.store(state.TakeFinished(deliveryOperation).has_value(),
                                   std::memory_order_release);
        });

    std::unique_lock<std::mutex> lock(deliveryMutex);
    deliveryCondition.wait(lock, [&] { return deliveryWaiting; });
    EXPECT_FALSE(oldResultDelivered.load(std::memory_order_acquire));
    EXPECT_FALSE(state.TryBeginLifecycleOperation());
  }

  delivery.join();
  EXPECT_FALSE(oldResultDelivered.load(std::memory_order_acquire));
  EXPECT_TRUE(state.IsCurrent(26));
  EXPECT_FALSE(state.Finish(25, false));
}

TEST(TestJumpgatePlaybackResultState, ResultOwnerIsCarriedExactlyWithItsGeneration)
{
  CJumpgatePlaybackResultState state;
  auto operation = state.BeginLifecycleOperation();
  ASSERT_TRUE(state.Begin(operation, 27, OLD_REQUEST_ID));
  const auto owner = state.CurrentOwner(operation);
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->generation, 27u);
  EXPECT_EQ(owner->requestId, OLD_REQUEST_ID);

  ASSERT_TRUE(state.Finish(27, 270, 1000, false));
  const auto result = state.TakeFinished(operation);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->requestId, OLD_REQUEST_ID);
  EXPECT_TRUE(state.Reset(operation, result->generation));
  EXPECT_FALSE(state.CurrentOwner(operation));
}

TEST(TestJumpgatePlaybackResultState, NewIntentOwnsResultIdBeforeOldDeliveryCanResume)
{
  CJumpgatePlaybackResultState state;
  {
    auto oldOperation = state.BeginLifecycleOperation();
    ASSERT_TRUE(state.Begin(oldOperation, 28, OLD_REQUEST_ID));
  }
  ASSERT_TRUE(state.Finish(28, 280, 1000, false));

  std::mutex mutex;
  std::condition_variable condition;
  bool deliveryWaiting = false;
  std::atomic<bool> oldDelivered{false};
  std::thread delivery;

  {
    auto admissionOperation = state.BeginLifecycleOperation();
    const auto supersededOwner = state.CurrentOwner(admissionOperation);
    ASSERT_TRUE(supersededOwner);
    ASSERT_EQ(supersededOwner->requestId, OLD_REQUEST_ID);
    ASSERT_TRUE(state.Begin(admissionOperation, 29, NEW_REQUEST_ID));

    delivery = std::thread(
        [&]
        {
          {
            std::lock_guard<std::mutex> lock(mutex);
            deliveryWaiting = true;
          }
          condition.notify_one();
          auto deliveryOperation = state.BeginLifecycleOperation();
          oldDelivered.store(state.TakeFinished(deliveryOperation).has_value(),
                             std::memory_order_release);
        });

    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return deliveryWaiting; });
    EXPECT_FALSE(oldDelivered.load(std::memory_order_acquire));
    const auto currentOwner = state.CurrentOwner(admissionOperation);
    ASSERT_TRUE(currentOwner);
    EXPECT_EQ(currentOwner->generation, 29u);
    EXPECT_EQ(currentOwner->requestId, NEW_REQUEST_ID);
  }

  delivery.join();
  EXPECT_FALSE(oldDelivered.load(std::memory_order_acquire));
  EXPECT_FALSE(state.Finish(28, false));
  const auto newResult = state.Finish(29, 290, 1000, true);
  ASSERT_TRUE(newResult);
  EXPECT_EQ(newResult->requestId, NEW_REQUEST_ID);
}

TEST(TestJumpgatePlaybackResultState, ColdDeliveryPermanentlyRejectsWaitingAdmission)
{
  CJumpgatePlaybackResultState state;
  ASSERT_TRUE(state.Begin(30));
  ASSERT_TRUE(state.Finish(30, 300, 1000, false));

  std::mutex admissionMutex;
  std::condition_variable admissionCondition;
  bool admissionWaiting = false;
  std::atomic<bool> admissionAccepted{true};
  std::thread admission;

  {
    auto deliveryOperation = state.BeginLifecycleOperation();
    const auto result = state.TakeFinished(deliveryOperation);
    ASSERT_TRUE(result);

    admission = std::thread(
        [&]
        {
          {
            std::lock_guard<std::mutex> lock(admissionMutex);
            admissionWaiting = true;
          }
          admissionCondition.notify_one();

          auto admissionOperation = state.BeginLifecycleOperation();
          admissionAccepted.store(state.Begin(admissionOperation, 31), std::memory_order_release);
        });

    {
      std::unique_lock<std::mutex> lock(admissionMutex);
      admissionCondition.wait(lock, [&] { return admissionWaiting; });
    }
    state.CloseAdmissions();
    EXPECT_TRUE(state.AdmissionsClosed());
    EXPECT_TRUE(state.Reset(deliveryOperation, result->generation));
  }

  admission.join();
  EXPECT_FALSE(admissionAccepted.load(std::memory_order_acquire));
  EXPECT_TRUE(state.AdmissionsClosed());
  EXPECT_EQ(state.CurrentGeneration(), 0u);
}
