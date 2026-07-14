/*
 *  Copyright (C) 2024 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "interfaces/IAnnouncer.h"
#include "threads/CriticalSection.h"
#include "utils/JumpgatePlaybackAuthority.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace KODI::JUMPGATE
{
class CJumpgateScrobbleDispatcher;
class CJumpgateScrobbleStartCoordinator;
} // namespace KODI::JUMPGATE

class TraktScrobbler : public ANNOUNCEMENT::IAnnouncer
{
public:
  TraktScrobbler();
  ~TraktScrobbler() override;

  // IAnnouncer
  void Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                const std::string& sender,
                const std::string& message,
                const CVariant& data) override;

  // Lifecycle
  void Initialize();
  void Deinitialize(bool drainScrobble = true);

  // Called from ProcessSlow() for deferred identification and token renewal.
  void ProcessSlow();

  // Content identification (set before playback starts)
  void SetContentInfo(
      const std::string& imdbId, const std::string& title, int year, int season, int episode);
  void SetPlaybackGeneration(uint64_t generation, uint64_t attemptToken);
  void CancelPlaybackGeneration(uint64_t generation, uint64_t attemptToken);
  bool SetClaimedContentInfo(uint64_t generation,
                             const std::string& provider,
                             const std::string& id,
                             const std::string& mediaType,
                             const std::string& title,
                             const std::string& logoUrl,
                             int year,
                             int season,
                             int episode,
                             bool traktEligible);
  void StopForReplacement();
  void SetMediaUrl(const std::string& url);
  void ClearContentInfo();

  // Content ID getters (for resume store integration)
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

  // Re-authentication
  void ForceReAuth();
  bool IsAuthenticatedPublic() const;

  // Bridge resume data (populated by QueryBridgeServer when available)
  // Uses std::atomic for lock-free cross-thread access (F-009 fix)
  int64_t GetBridgeResumePosition() const
  {
    return m_bridgeResumePositionMs.load(std::memory_order_relaxed);
  }
  void ClearBridgeResume() { m_bridgeResumePositionMs.store(0, std::memory_order_relaxed); }

  // Trakt playback sync (cross-device resume)
  int64_t GetTraktResumePosition();

  // Bridge URL
  std::string GetBridgeUrl() const;
  void SetBridgeUrl(const std::string& url);
  void SetBridgeProfile(const std::string& profileId,
                        const std::string& bridgeOrigin,
                        const std::string& bridgeBaseUrl,
                        const std::string& deviceToken,
                        bool traktEnabled);
  void ClearBridgeProfile();
  bool IsBridgeProfileBacked() const;

private:
  // Auth
  bool IsAuthenticated() const;
  bool FetchAccessTokenFromBridge();
  std::string NormalizeBridgeUrl(const std::string& url) const;
  std::string BuildBridgeEndpoint(const std::string& bridgeUrl, const std::string& endpoint) const;

  // Scrobble API
  bool SyncWatchHistory();
  std::string BuildSyncHistoryJson();

  // Content identification
  bool IdentifyContent();
  bool IsTraktIdentityAuthorized() const;
  bool StartScrobbleIfReady();
  bool QueueCompensatingStop(std::string cleanupKey,
                             std::string jsonBody,
                             std::string accessToken,
                             std::string clientId,
                             uint64_t cleanupId);
  bool HydrateFromTraktPublic(const std::string& id,
                              int season,
                              int episode,
                              const std::string& mediaUrlSnapshot);
  bool QueryBridgeServer();
  bool FetchLogoFromBridge(const std::string& imdbId, const std::string& mediaUrlSnapshot);
  // Bridge auto-detect
  void DetectBridgeUrl();

  // HTTP helpers (lock-free: accept token as parameter, no member access)
  bool TraktPostWithToken(const std::string& endpoint,
                          const std::string& jsonBody,
                          std::string& response,
                          const std::string& accessToken,
                          const std::string& clientId);
  bool TraktGetWithToken(const std::string& endpoint,
                         std::string& response,
                         const std::string& accessToken,
                         const std::string& clientId);
  std::string BuildScrobbleJson(float progress);

  // Get current playback progress (0-100)
  float GetPlaybackProgress() const;

  // State
  bool m_initialized{false};
  bool m_announcerRegistered{false};
  bool m_scrobbleActive{false};
  bool m_scrobblePaused{false};

  // Auth tokens
  std::string m_accessToken;
  std::string m_traktClientId;
  int64_t m_tokenExpiry{0};

  // Content info
  std::string m_imdbId;
  std::string m_traktSlug;
  std::string m_canonicalProvider;
  std::string m_canonicalId;
  std::string m_canonicalMediaType;
  std::string m_title;
  std::string m_episodeTitle;
  std::string m_logoUrl;
  std::string m_logoFetchedForImdb;
  int m_year{0};
  int m_season{-1};
  int m_episode{-1};
  bool m_contentIdentified{false};
  bool m_sourceClaimResolved{false};
  bool m_sourceClaimAuthorized{false};
  bool m_sourceClaimStartPending{false};
  bool m_playbackActive{false};
  KODI::JUMPGATE::CJumpgatePlaybackAuthority m_playbackCallbackAuthority;
  uint64_t m_contentAuthorityGeneration{0};
  uint64_t m_playbackGeneration{0};
  uint64_t m_playbackAttemptToken{0};
  uint64_t m_playbackCallbackAuthorityToken{0};
  int64_t m_lastSourceClaimStartAttemptTime{0};

  // URL from intent for URL-based identification
  std::string m_mediaUrl;
  std::string m_resolvedUrl; // after following redirects

  // Bridge server URL (resolved at startup via auto-detect)
  std::string m_bridgeUrl;
  std::string m_bridgeOrigin;
  std::string m_bridgeProfileId;
  std::string m_bridgeDeviceToken;
  bool m_bridgeProfileBacked{false};
  bool m_bridgeTraktEnabled{false};
  bool m_profileRuntimeApplied{false};
  uint64_t m_authAuthorityGeneration{0};

  // Bridge resume data (from /identify response)
  // std::atomic for lock-free access from XBMCApp (F-009 fix)
  std::atomic<int64_t> m_bridgeResumePositionMs{0};

  // Periodic scrobble progress update (keeps Trakt /users/me/watching fresh)
  static constexpr int SCROBBLE_UPDATE_INTERVAL_SEC = 10;

  // Deferred content identification retry
  int64_t m_playbackStartTime{0};
  bool m_identifyFailed{false};
  bool m_bridgeDetected{false}; // Deferred bridge URL detection (curl unsafe during early init)
  int64_t m_lastConfiguredTokenFetchTime{0};
  static constexpr int IDENTIFY_RETRY_SEC = 25;

  // Public metadata hydration throttle (Trakt public endpoints)
  int64_t m_lastPublicHydrateAttemptTime{0};
  std::string m_lastPublicHydrateKey;

  // Trakt API constants
  static constexpr const char* TRAKT_API_URL = "https://api.trakt.tv";

  // Jumpgate Bridge server URLs
  static constexpr const char* BRIDGE_CLOUD_URL = "https://jumpgate-bridge.fly.dev";
  static constexpr const char* BRIDGE_LOCAL_URL = "http://127.0.0.1:7515";

  std::shared_ptr<std::recursive_mutex> m_serviceIoMutex;
  std::mutex m_lifecycleMutex;
  std::mutex m_dispatcherMutex;
  mutable CCriticalSection m_critSection;
  std::shared_ptr<KODI::JUMPGATE::CJumpgateScrobbleStartCoordinator> m_scrobbleStartCoordinator;
  std::unique_ptr<KODI::JUMPGATE::CJumpgateScrobbleDispatcher> m_scrobbleDispatcher;
};
