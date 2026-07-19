/*
 *  Copyright (C) 2026 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"
#include "utils/JumpgateHistoryEventState.h"
#include "utils/JumpgateWatchedTimeState.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace KODI::JUMPGATE
{
class CJumpgateHistoryEventDispatcher;
}

class TraktScrobbler final
{
public:
  TraktScrobbler();
  ~TraktScrobbler();

  void Initialize();
  void Deinitialize(bool drainHistory = true);
  void ProcessSlow();

  void OnPlaybackStarted(bool resumed);
  void OnPlaybackPaused();
  void SetBackgrounded(bool backgrounded);
  KODI::JUMPGATE::JumpgateHistoryTerminalResult StopForReplacement(bool completed = false);

  void SetContentInfo(
      const std::string& imdbId, const std::string& title, int year, int season, int episode);
  void SetPlaybackGeneration(std::uint64_t generation, std::uint64_t attemptToken);
  void CancelPlaybackGeneration(std::uint64_t generation, std::uint64_t attemptToken);
  bool SetClaimedContentInfo(std::uint64_t generation,
                             const std::string& profileId,
                             const std::string& deviceId,
                             const std::string& bridgeOrigin,
                             const std::string& deviceToken,
                             const std::string& sessionId,
                             const std::string& historyGrant,
                             const std::string& historyGrantKind,
                             std::uint64_t sessionRevision,
                             const std::string& provider,
                             const std::string& id,
                             const std::string& mediaType,
                             const std::string& title,
                             const std::string& logoUrl,
                             int year,
                             int season,
                             int episode,
                             bool traktEligible);
  void SetMediaUrl(const std::string& url);
  void ClearContentInfo();

  std::string GetImdbId() const;
  std::string GetCanonicalProvider() const;
  std::string GetCanonicalId() const;
  std::string GetTitle() const;
  std::string GetEpisodeTitle() const;
  std::string GetLogoUrl() const;
  int GetYear() const;
  int GetSeason() const;
  int GetEpisode() const;
  bool IsContentIdentified() const;

  std::string GetBridgeOrigin() const;
  void SetBridgeProfile(const std::string& profileId,
                        const std::string& deviceId,
                        const std::string& bridgeOrigin,
                        bool sourceBacked,
                        bool credentialsValid,
                        bool traktEnabled);
  void ClearBridgeProfile();
  bool IsBridgeProfileBacked() const;

private:
  void EnsureDispatcher();
  bool IdentifyContent();
  bool IsTraktIdentityAuthorized() const;
  std::string NormalizeBridgeOrigin(const std::string& url) const;
  KODI::JUMPGATE::JumpgateHistorySnapshot GetPlaybackSnapshot(std::int64_t nowMs = 0);
  static std::int64_t GetMonotonicTimeMs();

  bool m_initialized{false};
  bool m_playbackActive{false};
  bool m_playbackPaused{false};
  bool m_backgrounded{false};

  std::string m_imdbId;
  std::string m_canonicalProvider;
  std::string m_canonicalId;
  std::string m_canonicalMediaType;
  std::string m_title;
  std::string m_episodeTitle;
  std::string m_logoUrl;
  int m_year{0};
  int m_season{-1};
  int m_episode{-1};
  bool m_contentIdentified{false};
  bool m_sourceClaimResolved{false};
  bool m_sourceClaimAuthorized{false};
  std::uint64_t m_playbackGeneration{0};
  std::uint64_t m_playbackAttemptToken{0};

  std::string m_mediaUrl;
  std::string m_resolvedUrl;
  std::int64_t m_playbackStartTimeMs{0};
  KODI::JUMPGATE::CJumpgateWatchedTimeState m_watchedTime;
  bool m_identifyFailed{false};

  std::string m_bridgeOrigin;
  std::string m_bridgeProfileId;
  std::string m_bridgeDeviceId;
  bool m_bridgeProfileBacked{false};
  bool m_bridgeCredentialsValid{false};
  bool m_bridgeTraktEnabled{false};

  static constexpr std::int64_t HISTORY_UPDATE_INTERVAL_MS = 10000;
  static constexpr std::int64_t IDENTIFY_RETRY_MS = 25000;

  std::mutex m_lifecycleMutex;
  std::mutex m_dispatcherMutex;
  mutable CCriticalSection m_critSection;
  std::unique_ptr<KODI::JUMPGATE::CJumpgateHistoryEventDispatcher> m_dispatcher;
};
