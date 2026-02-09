/*
 *  Copyright (C) 2024 Team ModiKodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "interfaces/IAnnouncer.h"
#include "threads/CriticalSection.h"

#include <string>

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
  void Deinitialize();

  // Called from ProcessSlow() for device code auth polling
  void ProcessSlow();

  // Content identification (set before playback starts)
  void SetContentInfo(const std::string& imdbId,
                      const std::string& title,
                      int year,
                      int season,
                      int episode);
  void SetMediaUrl(const std::string& url);
  void ClearContentInfo();

  // Content ID getters (for resume store integration)
  std::string GetImdbId() const;
  std::string GetTitle() const;
  int GetYear() const;
  int GetSeason() const;
  int GetEpisode() const;
  bool IsContentIdentified() const;

  // Re-authentication
  void ForceReAuth();
  bool IsAuthenticatedPublic() const;

  // Bridge resume data (populated by QueryBridgeServer when available)
  int GetBridgeResumePosition() const { return m_bridgeResumePositionMs; }
  void ClearBridgeResume() { m_bridgeResumePositionMs = 0; }

  // Trakt playback sync (cross-device resume)
  int GetTraktResumePosition();

  // Bridge URL
  std::string GetBridgeUrl() const;
  void SetBridgeUrl(const std::string& url);

private:
  // Auth
  bool IsAuthenticated() const;
  bool LoadTokens();
  bool SaveTokens();
  bool RefreshAccessToken();
  void StartDeviceCodeAuth();
  void PollForToken();

  // Scrobble API
  bool ScrobbleStart(float progress);
  bool ScrobbleStop(float progress);
  bool ScrobblePause(float progress);
  bool SyncWatchHistory();

  // Content identification
  bool IdentifyContent();
  bool QueryBridgeServer();
  bool SearchTrakt(const std::string& query);
  bool ParseImdbFromUrl(const std::string& url);
  std::string ExtractTitleFromUrl(const std::string& url);

  // Bridge auto-detect
  void DetectBridgeUrl();

  // HTTP helpers
  bool TraktPost(const std::string& endpoint,
                 const std::string& jsonBody,
                 std::string& response);
  bool TraktGet(const std::string& endpoint, std::string& response);
  std::string BuildScrobbleJson(float progress);

  // Get current playback progress (0-100)
  float GetPlaybackProgress() const;

  // State
  bool m_initialized{false};
  bool m_scrobbleActive{false};

  // Auth tokens
  std::string m_accessToken;
  std::string m_refreshToken;
  int64_t m_tokenExpiry{0};
  std::string m_deviceCode;
  bool m_authInProgress{false};
  int m_pollIntervalSec{5};
  int64_t m_lastPollTime{0};

  // Content info
  std::string m_imdbId;
  std::string m_traktSlug;
  std::string m_title;
  int m_year{0};
  int m_season{-1};
  int m_episode{-1};
  bool m_contentIdentified{false};

  // URL from intent for URL-based identification
  std::string m_mediaUrl;
  std::string m_resolvedUrl; // after following redirects

  // Bridge server URL (resolved at startup via auto-detect)
  std::string m_bridgeUrl;

  // Bridge resume data (from /identify response)
  int m_bridgeResumePositionMs{0};

  // Periodic scrobble progress update (keeps Trakt /users/me/watching fresh)
  int64_t m_lastScrobbleUpdateTime{0};
  static constexpr int SCROBBLE_UPDATE_INTERVAL_SEC = 10;

  // Deferred content identification retry
  int64_t m_playbackStartTime{0};
  bool m_identifyFailed{false};
  static constexpr int IDENTIFY_RETRY_SEC = 15;

  // Trakt API constants
  static constexpr const char* TRAKT_API_URL = "https://api.trakt.tv";
  static constexpr const char* TRAKT_CLIENT_ID = "d4161a7a106424551add171e5470112e4afdaf2438e6ef2fe0548edc75924868";
  static constexpr const char* TRAKT_CLIENT_SECRET = "b5fcd7cb5d9bb963784d11bbf8535bc0d25d46225016191eb48e50792d2155c0";

  // ModiKodi Bridge server URL for zero-config content identification
  // Change this to your deployment URL (e.g., https://your-bridge.onrender.com)
  static constexpr const char* BRIDGE_SERVER_URL = "http://127.0.0.1:7515";

  mutable CCriticalSection m_critSection;
};
