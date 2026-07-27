/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackResultState.h"

#include <algorithm>
#include <utility>

namespace KODI::JUMPGATE
{

CJumpgatePlaybackResultState::LifecycleOperation::LifecycleOperation(
    CJumpgatePlaybackResultState& owner)
  : m_owner(&owner),
    m_lock(owner.m_lifecycleMutex)
{
}

CJumpgatePlaybackResultState::LifecycleOperation::LifecycleOperation(
    CJumpgatePlaybackResultState& owner, std::try_to_lock_t tag)
  : m_owner(&owner),
    m_lock(owner.m_lifecycleMutex, tag)
{
}

CJumpgatePlaybackResultState::LifecycleOperation CJumpgatePlaybackResultState::
    BeginLifecycleOperation()
{
  return LifecycleOperation{*this};
}

std::optional<CJumpgatePlaybackResultState::LifecycleOperation> CJumpgatePlaybackResultState::
    TryBeginLifecycleOperation()
{
  LifecycleOperation operation{*this, std::try_to_lock};
  if (!operation.m_lock.owns_lock())
    return std::nullopt;
  return std::optional<LifecycleOperation>{std::move(operation)};
}

bool CJumpgatePlaybackResultState::Begin(LifecycleOperation& operation, uint64_t generation)
{
  if (operation.m_owner != this || !operation.m_lock.owns_lock() || !Begin(generation))
    return false;

  operation.m_generation = generation;
  return true;
}

bool CJumpgatePlaybackResultState::Begin(LifecycleOperation& operation,
                                         uint64_t generation,
                                         std::string requestId)
{
  if (operation.m_owner != this || !operation.m_lock.owns_lock())
    return false;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_admissionsClosed || generation == 0 || requestId.empty())
      return false;

    m_generation = generation;
    m_pending = true;
    m_positionMs = 0;
    m_durationMs = 0;
    m_requestId = std::move(requestId);
    m_finishedResult.reset();
  }

  operation.m_generation = generation;
  return true;
}

bool CJumpgatePlaybackResultState::Begin(uint64_t generation)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_admissionsClosed || generation == 0)
    return false;

  const bool sameGeneration = generation == m_generation;
  m_generation = generation;
  m_pending = true;
  m_positionMs = 0;
  m_durationMs = 0;
  if (!sameGeneration)
    m_requestId.clear();
  m_finishedResult.reset();
  return true;
}

void CJumpgatePlaybackResultState::CloseAdmissions()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_admissionsClosed = true;
}

bool CJumpgatePlaybackResultState::AdmissionsClosed() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_admissionsClosed;
}

std::optional<JumpgatePlaybackResult> CJumpgatePlaybackResultState::Finish(uint64_t generation,
                                                                           int64_t positionMs,
                                                                           int64_t durationMs,
                                                                           bool completed)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_pending || generation == 0 || generation != m_generation)
    return std::nullopt;

  m_pending = false;
  m_finishedResult =
      JumpgatePlaybackResult{generation, m_requestId, std::max<int64_t>(0, positionMs),
                             std::max<int64_t>(0, durationMs), completed};
  return m_finishedResult;
}

bool CJumpgatePlaybackResultState::Capture(uint64_t generation,
                                           int64_t positionMs,
                                           int64_t durationMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_pending || generation == 0 || generation != m_generation || positionMs < 0 ||
      durationMs < 0)
  {
    return false;
  }
  if (positionMs == 0 && durationMs == 0 && (m_positionMs > 0 || m_durationMs > 0))
    return false;
  m_positionMs = positionMs;
  m_durationMs = durationMs;
  return true;
}

std::optional<JumpgatePlaybackResult> CJumpgatePlaybackResultState::Finish(uint64_t generation,
                                                                           bool completed)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_pending || generation == 0 || generation != m_generation)
    return std::nullopt;

  m_pending = false;
  m_finishedResult =
      JumpgatePlaybackResult{generation, m_requestId, m_positionMs, m_durationMs, completed};
  return m_finishedResult;
}

std::optional<JumpgatePlaybackResult> CJumpgatePlaybackResultState::TakeFinished()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_finishedResult || m_finishedResult->generation != m_generation)
    return std::nullopt;

  std::optional<JumpgatePlaybackResult> result;
  result.swap(m_finishedResult);
  return result;
}

std::optional<JumpgatePlaybackResult> CJumpgatePlaybackResultState::TakeFinished(
    LifecycleOperation& operation)
{
  if (operation.m_owner != this || !operation.m_lock.owns_lock())
    return std::nullopt;

  auto result = TakeFinished();
  if (result)
    operation.m_generation = result->generation;
  return result;
}

std::optional<JumpgatePlaybackResultOwner> CJumpgatePlaybackResultState::CurrentOwner(
    const LifecycleOperation& operation) const
{
  if (operation.m_owner != this || !operation.m_lock.owns_lock())
    return std::nullopt;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_generation == 0 || m_requestId.empty())
    return std::nullopt;
  return JumpgatePlaybackResultOwner{m_generation, m_requestId};
}

uint64_t CJumpgatePlaybackResultState::CurrentGeneration() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_generation;
}

bool CJumpgatePlaybackResultState::IsCurrent(uint64_t generation) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return generation != 0 && generation == m_generation;
}

void CJumpgatePlaybackResultState::Reset()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_generation = 0;
  m_pending = false;
  m_positionMs = 0;
  m_durationMs = 0;
  m_requestId.clear();
  m_finishedResult.reset();
}

bool CJumpgatePlaybackResultState::Reset(uint64_t generation)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (generation == 0 || generation != m_generation)
    return false;

  m_generation = 0;
  m_pending = false;
  m_positionMs = 0;
  m_durationMs = 0;
  m_requestId.clear();
  m_finishedResult.reset();
  return true;
}

bool CJumpgatePlaybackResultState::Reset(LifecycleOperation& operation, uint64_t generation)
{
  if (operation.m_owner != this || !operation.m_lock.owns_lock() ||
      !operation.OwnsGeneration(generation) || !Reset(generation))
  {
    return false;
  }

  operation.m_generation = 0;
  return true;
}

} // namespace KODI::JUMPGATE
