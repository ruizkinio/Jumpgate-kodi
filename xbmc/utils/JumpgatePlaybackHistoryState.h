/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgatePlaybackHistory.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace KODI::JUMPGATE
{

struct JumpgatePlaybackHistoryIdentity
{
  uint64_t generation{0};
  std::string profileId;
  std::string contentKey;
  std::optional<JumpgateCanonicalIdentity> canonicalIdentity;
  JumpgatePlaybackHistoryDisplay display;
};

struct JumpgatePlaybackResumeToken
{
  uint64_t generation{0};
  uint64_t serial{0};
  std::string profileId;
  std::string contentKey;
};

std::optional<int64_t> ParseJumpgatePositiveInt64(std::string_view value);

class CJumpgatePlaybackHistoryState final
{
public:
  bool AdvanceGeneration(uint64_t generation);
  bool Activate(JumpgatePlaybackHistoryIdentity identity, int64_t activatedAtMs);
  bool UpdateProgress(uint64_t generation,
                      int64_t positionMs,
                      int64_t durationMs,
                      int64_t observedAtMs);
  std::optional<JumpgatePlaybackHistoryEntry> Finalize(uint64_t generation,
                                                       bool explicitEnd,
                                                       int64_t observedAtMs);

  std::optional<JumpgatePlaybackResumeToken> BeginResume(uint64_t generation) const;
  bool ApplyResume(const JumpgatePlaybackResumeToken& token,
                   int64_t positionMs,
                   const std::function<void(int64_t)>& apply);

private:
  struct ActiveState
  {
    JumpgatePlaybackHistoryIdentity identity;
    int64_t positionMs{0};
    int64_t durationMs{0};
    int64_t updatedAtMs{0};
    bool resumeApplied{false};
    std::optional<JumpgatePlaybackHistoryEntry> finalSnapshot;
  };

  mutable std::mutex m_mutex;
  uint64_t m_generation{0};
  uint64_t m_resumeSerial{0};
  std::optional<ActiveState> m_active;
};

} // namespace KODI::JUMPGATE
