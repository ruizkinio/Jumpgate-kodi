/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "JumpgatePlaybackContext.h"
#include "JumpgateProfileStore.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

constexpr std::size_t JUMPGATE_HISTORY_MAX_ENTRIES = 256;
constexpr std::size_t JUMPGATE_HISTORY_MAX_BYTES = 512 * 1024;

struct JumpgatePlaybackHistoryDisplay
{
  std::optional<std::string> title;
  std::optional<int> year;
  std::optional<int> season;
  std::optional<int> episode;
};

struct JumpgatePlaybackHistoryEntry
{
  std::string profileId;
  std::string contentKey;
  std::optional<JumpgateCanonicalIdentity> canonicalIdentity;
  JumpgatePlaybackHistoryDisplay display;
  int64_t positionMs{0};
  int64_t durationMs{0};
  bool completed{false};
  bool watched{false};
  int64_t updatedAtMs{0};
};

struct JumpgatePlaybackHistoryDocument
{
  std::vector<JumpgatePlaybackHistoryEntry> entries;
  std::vector<std::string> blockedProfiles;
  std::vector<std::string> forgottenProfiles;
};

bool IsValidJumpgateHistoryProfileId(const std::string& profileId);
bool IsValidJumpgateHistoryContentKey(const std::string& contentKey);
bool IsValidJumpgateHistoryCanonicalIdentity(const JumpgateCanonicalIdentity& identity);
bool IsJumpgatePlaybackThresholdReached(int64_t positionMs, int64_t durationMs, int percentage);
int64_t GetJumpgatePlaybackResumePosition(const JumpgatePlaybackHistoryEntry& entry);

bool ParseJumpgatePlaybackHistory(const std::string& json,
                                  JumpgatePlaybackHistoryDocument& document,
                                  std::string& error);
bool SerializeJumpgatePlaybackHistory(const JumpgatePlaybackHistoryDocument& document,
                                      std::string& json,
                                      std::string& error);

class CJumpgatePlaybackHistoryStore final
{
public:
  explicit CJumpgatePlaybackHistoryStore(IJumpgateProfileStorage& storage);

  bool Get(const std::string& profileId,
           const std::string& contentKey,
           std::optional<JumpgatePlaybackHistoryEntry>& entry,
           std::string& error) const;
  bool Save(const std::string& expectedProfileId,
            JumpgatePlaybackHistoryEntry entry,
            std::string& error);
  bool ClearProfile(const std::string& profileId, std::string& error);
  bool BlockProfile(const std::string& profileId, std::string& error);
  bool IsProfileBlocked(const std::string& profileId, bool& blocked, std::string& error) const;
  bool GetProfileProtection(const std::string& profileId,
                            bool& blocked,
                            bool& forgotten,
                            std::string& error) const;
  bool UnblockProfile(const std::string& profileId, std::string& error);
  bool PurgeBlockedProfile(const std::string& profileId, std::string& error);
  bool CompleteProfileRepair(const std::string& profileId, std::string& error);
  bool ResetProfile(const std::string& profileId, std::string& error);

private:
  bool Load(JumpgatePlaybackHistoryDocument& document, std::string& error) const;
  bool WriteCandidate(JumpgatePlaybackHistoryDocument& document,
                      const std::string& retainedProfileId,
                      const std::string& retainedContentKey,
                      std::string& error);
  bool WriteExact(const JumpgatePlaybackHistoryDocument& document, std::string& error);

  IJumpgateProfileStorage& m_storage;
  mutable std::mutex m_mutex;
  mutable std::set<std::string> m_blockedProfiles;
  mutable std::set<std::string> m_forgottenProfiles;
};

} // namespace KODI::JUMPGATE
