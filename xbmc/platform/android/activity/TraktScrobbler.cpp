/*
 *  Copyright (C) 2024 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TraktScrobbler.h"

#include "JumpgateThresholds.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "interfaces/AnnouncementManager.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

using namespace ANNOUNCEMENT;

namespace
{
constexpr const char* TOKEN_FILE = "special://profile/trakt.json";

int64_t GetCurrentTimeSec()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ClearSensitive(std::string& value)
{
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
}

// Avoid leaking full URLs (which may include user identifiers or signed tokens) into logs.
std::string RedactUrlForLog(const std::string& rawUrl)
{
  if (rawUrl.empty())
    return "<empty>";

  std::string s = rawUrl;

  // Strip query/fragment.
  size_t q = s.find('?');
  if (q != std::string::npos)
    s.resize(q);
  size_t f = s.find('#');
  if (f != std::string::npos)
    s.resize(f);

  // Keep origin only: scheme://host[:port]/<redacted>
  size_t scheme = s.find("://");
  if (scheme == std::string::npos)
    return "<redacted>";

  size_t hostStart = scheme + 3;
  size_t pathStart = s.find('/', hostStart);
  if (pathStart == std::string::npos)
    return s + "/<redacted>";
  return s.substr(0, pathStart) + "/<redacted>";
}

} // namespace

TraktScrobbler::TraktScrobbler() = default;

TraktScrobbler::~TraktScrobbler()
{
  Deinitialize();
}

void TraktScrobbler::Initialize()
{
  std::scoped_lock lifecycleLock(m_lifecycleMutex);
  std::unique_lock lock(m_critSection);
  if (m_initialized)
    return;

  // Once the Jumpgate profile runtime has applied a mode, it is authoritative.
  // Never revive an unrelated legacy plaintext token after a profile clear.
  if (!m_profileRuntimeApplied && !m_bridgeProfileBacked)
    LoadTokens();
  if (m_bridgeUrl.empty())
    m_bridgeUrl = BRIDGE_CLOUD_URL;

  m_initialized = true;
  // Bridge detection deferred to first ProcessSlow call.
  // CCurlFile crashes (SIGABRT) when called from onNewIntent during early init because
  // Kodi's native subsystems (including curl) aren't fully initialized yet.
  // ProcessSlow runs on the Kodi main thread after full initialization.
  m_bridgeDetected = false;
  lock.unlock();

  // Never call the announcement manager while holding m_critSection. Its dispatch path holds
  // the manager lock while entering Announce(), so doing so would create an AB/BA lock order.
  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(this, ANNOUNCEMENT::Player);

  CLog::Log(LOGINFO, "TraktScrobbler: Initialized (bridge detection deferred to ProcessSlow)");
}

void TraktScrobbler::Deinitialize()
{
  std::scoped_lock lifecycleLock(m_lifecycleMutex);
  std::unique_lock lock(m_critSection);
  if (!m_initialized)
    return;

  m_initialized = false;
  ++m_authAuthorityGeneration;
  m_authInProgress = false;
  ClearSensitive(m_deviceCode);
  lock.unlock();

  // Removal waits for any callback already copied by DoAnnounce(). Keep m_critSection released
  // so such a callback can observe m_initialized == false and leave without a lock inversion.
  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);
  lock.lock();

  // Check if we need to send a ScrobbleStop (F-008: clear "currently watching" on exit)
  bool shouldScrobbleStop = m_scrobbleActive && m_contentIdentified;

  if (shouldScrobbleStop)
  {
    // Capture state by value before releasing lock
    float progress = GetPlaybackProgress();
    std::string scrobbleJson = BuildScrobbleJson(progress);
    std::string accessToken = m_accessToken;
    m_scrobbleActive = false; // Prevent concurrent ProcessSlow from sending updates

    lock.unlock();

    // Synchronous ScrobbleStop with tight 2s timeout (process may die any moment)
    if (!scrobbleJson.empty() && !accessToken.empty())
    {
      XFILE::CCurlFile curl;
      curl.SetRequestHeader("Content-Type", "application/json");
      curl.SetRequestHeader("trakt-api-version", "2");
      curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
      curl.SetRequestHeader("Authorization", "Bearer " + accessToken);
      curl.SetTimeout(2);

      std::string url = std::string(TRAKT_API_URL) + "/scrobble/stop";
      std::string response;
      bool ok = curl.Post(url, scrobbleJson, response);
      CLog::Log(LOGINFO, "TraktScrobbler: Deinitialize ScrobbleStop {} at {:.1f}%",
                ok ? "succeeded" : "failed", progress);
    }
    else
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Deinitialize ScrobbleStop skipped (empty json or token)");
    }
    ClearSensitive(accessToken);
  }
  else
  {
    lock.unlock();
  }

  CLog::Log(LOGINFO, "TraktScrobbler: Deinitialized");
}

// ---------------------------------------------------------------------------
// Announce: All HTTP I/O is performed outside m_critSection.
// Pattern: copy state -> unlock -> I/O -> lock -> check-and-abort
// ---------------------------------------------------------------------------
void TraktScrobbler::Announce(AnnouncementFlag flag,
                              const std::string& sender,
                              const std::string& message,
                              const CVariant& data)
{
  if (sender != CAnnouncementManager::ANNOUNCEMENT_SENDER)
    return;

  if (!(flag & Player))
    return;

  std::unique_lock lock(m_critSection);

  if (!m_initialized)
    return;

  if (message == "OnPlay" || message == "OnResume")
  {
    m_scrobblePaused = false;

    // --- Content identification (may require HTTP) ---
    if (!m_contentIdentified)
    {
      // Snapshot for staleness check
      std::string urlSnapshot = m_mediaUrl;

      lock.unlock();
      bool identified = IdentifyContent();
      lock.lock();

      // Check-and-abort: if content changed during identification I/O
      if (!m_initialized || m_mediaUrl != urlSnapshot)
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale identification (content changed)");
        return;
      }

      if (identified)
      {
        m_contentIdentified = true;
      }
      else if (m_playbackStartTime == 0)
      {
        // Mark for deferred retry in ProcessSlow
        m_playbackStartTime = GetCurrentTimeSec();
        m_identifyFailed = false;
        CLog::Log(LOGDEBUG, "TraktScrobbler: Content not yet identified, will retry");
      }
    }

    // A paired profile has one authenticated Bridge identity. It never falls
    // back to local Trakt credentials or another configured URL.
    const bool profileBacked = m_bridgeProfileBacked;
    if (profileBacked)
    {
      if (!m_bridgeTraktEnabled)
      {
        CLog::Log(LOGDEBUG, "TraktScrobbler: Trakt is disabled for the active Jumpgate profile");
        return;
      }

      std::string urlSnapshot = m_mediaUrl;
      const std::string profileIdSnapshot = m_bridgeProfileId;
      const uint64_t authorityGeneration = m_authAuthorityGeneration;
      lock.unlock();
      bool fetched = FetchAccessTokenFromBridge();
      lock.lock();

      if (!m_initialized || m_mediaUrl != urlSnapshot ||
          m_authAuthorityGeneration != authorityGeneration ||
          m_bridgeProfileId != profileIdSnapshot)
      {
        CLog::Log(LOGINFO,
                  "TraktScrobbler: Discarding stale Bridge token fetch after state change");
        return;
      }

      if (fetched)
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Using active-profile Trakt token from Bridge");
      }
      else
      {
        ClearSensitive(m_accessToken);
        ClearSensitive(m_refreshToken);
        m_tokenExpiry = 0;
        CLog::Log(LOGWARNING, "TraktScrobbler: Active-profile token fetch failed; scrobbling is "
                              "disabled for this playback");
        return;
      }
    }

    // --- Authentication (may require HTTP for token refresh or device code) ---
    if (!IsAuthenticated())
    {
      if (m_profileRuntimeApplied)
      {
        CLog::Log(LOGWARNING, "TraktScrobbler: Profile runtime supplied no usable token; legacy "
                              "local auth is disabled");
        return;
      }
      if (!m_refreshToken.empty())
      {
        std::string urlSnapshot = m_mediaUrl;

        lock.unlock();
        bool refreshed = RefreshAccessToken();
        lock.lock();

        // Check-and-abort
        if (!m_initialized || m_mediaUrl != urlSnapshot)
        {
          CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale auth refresh (content changed)");
          return;
        }

        if (!refreshed)
        {
          if (!m_authInProgress)
          {
            std::string urlSnapshot2 = m_mediaUrl;
            lock.unlock();
            StartDeviceCodeAuth();
            lock.lock();
            if (!m_initialized || m_mediaUrl != urlSnapshot2)
              return;
          }
          return;
        }
        // Token refreshed, fall through to scrobble
      }
      else if (!m_authInProgress)
      {
        std::string urlSnapshot = m_mediaUrl;
        lock.unlock();
        StartDeviceCodeAuth();
        lock.lock();
        if (!m_initialized || m_mediaUrl != urlSnapshot)
          return;
        return;
      }
      else
      {
        return;
      }
    }

    // --- Start scrobble (HTTP) ---
    if (m_contentIdentified)
    {
      float progress = GetPlaybackProgress();
      std::string scrobbleJson = BuildScrobbleJson(progress);
      std::string accessToken = m_accessToken;
      std::string imdbSnapshot = m_imdbId;
      const uint64_t authorityGeneration = m_authAuthorityGeneration;

      lock.unlock();
      bool ok = false;
      if (!scrobbleJson.empty())
      {
        std::string response;
        ok = TraktPostWithToken("/scrobble/start", scrobbleJson, response, accessToken);
      }
      lock.lock();

      // Check-and-abort
      ClearSensitive(accessToken);
      if (!m_initialized || m_imdbId != imdbSnapshot ||
          m_authAuthorityGeneration != authorityGeneration)
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale scrobble start after state change");
        return;
      }

      if (ok)
      {
        m_scrobbleActive = true;
        m_scrobblePaused = false;
        m_lastScrobbleUpdateTime = GetCurrentTimeSec();
        CLog::Log(LOGINFO, "TraktScrobbler: Scrobble started at {:.1f}%", progress);
      }
    }
  }
  else if (message == "OnPause")
  {
    if (m_scrobbleActive)
    {
      float progress = GetPlaybackProgress();
      std::string scrobbleJson = BuildScrobbleJson(progress);
      std::string accessToken = m_accessToken;
      const uint64_t authorityGeneration = m_authAuthorityGeneration;
      m_scrobblePaused = true;

      // Fire-and-forget pause: no state update needed after
      lock.unlock();
      if (!scrobbleJson.empty())
      {
        std::string response;
        TraktPostWithToken("/scrobble/pause", scrobbleJson, response, accessToken);
      }
      ClearSensitive(accessToken);
      // No re-lock needed -- we don't update state after pause
      lock.lock();
      const bool current = m_initialized && m_authAuthorityGeneration == authorityGeneration;
      lock.unlock();
      if (current)
        CLog::Log(LOGINFO, "TraktScrobbler: Scrobble paused at {:.1f}%", progress);
    }
  }
  else if (message == "OnStop")
  {
    if (m_scrobbleActive)
    {
      float progress = GetPlaybackProgress();
      bool wasIdentified = m_contentIdentified;
      std::string scrobbleJson = BuildScrobbleJson(progress);
      std::string accessToken = m_accessToken;
      std::string imdbSnapshot = m_imdbId;
      const uint64_t authorityGeneration = m_authAuthorityGeneration;

      // Build watch history JSON under lock (reads content fields)
      std::string historyJson;
      bool shouldSyncHistory = (progress > Jumpgate::TRAKT_HISTORY_SYNC_PCT && wasIdentified);
      if (shouldSyncHistory)
        historyJson = BuildSyncHistoryJson();

      m_scrobbleActive = false; // Set before unlock -- stop is final
      m_scrobblePaused = false;

      lock.unlock();

      // ScrobbleStop HTTP
      if (!scrobbleJson.empty())
      {
        std::string response;
        TraktPostWithToken("/scrobble/stop", scrobbleJson, response, accessToken);
      }
      lock.lock();
      const bool current = m_initialized && m_authAuthorityGeneration == authorityGeneration &&
                           m_imdbId == imdbSnapshot;
      lock.unlock();
      if (current)
        CLog::Log(LOGINFO, "TraktScrobbler: Scrobble stopped at {:.1f}%", progress);

      // Sync watch history if >80% complete
      if (current && shouldSyncHistory && !historyJson.empty())
      {
        std::string response;
        if (TraktPostWithToken("/sync/history", historyJson, response, accessToken))
          CLog::Log(LOGINFO, "TraktScrobbler: Watch history synced at {:.1f}%", progress);
      }
      ClearSensitive(accessToken);
      // No re-lock needed -- m_scrobbleActive already set to false
    }
  }
}

// ---------------------------------------------------------------------------
// ProcessSlow: All HTTP I/O performed outside m_critSection.
// ---------------------------------------------------------------------------
void TraktScrobbler::ProcessSlow()
{
  std::unique_lock lock(m_critSection);

  if (!m_initialized)
    return;

  // Deferred bridge URL detection (curl isn't safe during early onNewIntent init)
  if (!m_bridgeDetected)
  {
    m_bridgeDetected = true;
    lock.unlock();
    DetectBridgeUrl();
    lock.lock();
    if (!m_initialized)
      return;
    std::string bridgeUrlSnapshot = m_bridgeUrl;
    CLog::Log(LOGINFO, "TraktScrobbler: Bridge detection complete (bridge={})",
              RedactUrlForLog(bridgeUrlSnapshot));
  }

  // --- Auth polling section ---
  if (m_authInProgress)
  {
    std::string urlSnapshot = m_mediaUrl;
    lock.unlock();
    PollForToken();
    lock.lock();
    if (!m_initialized)
      return;

    // Check-and-abort (content may have changed during poll HTTP)
    if (m_mediaUrl != urlSnapshot)
    {
      CLog::Log(LOGDEBUG, "TraktScrobbler: Content changed during auth poll, continuing");
      // Don't return -- auth state is independent of content, continue processing
    }
  }

  // --- Deferred content identification retry ---
  if (!m_contentIdentified && !m_identifyFailed && m_playbackStartTime > 0)
  {
    int64_t elapsed = GetCurrentTimeSec() - m_playbackStartTime;
    if (elapsed <= IDENTIFY_RETRY_SEC)
    {
      std::string urlSnapshot = m_mediaUrl;

      lock.unlock();
      bool identified = IdentifyContent();
      lock.lock();

      // Check-and-abort
      if (!m_initialized || m_mediaUrl != urlSnapshot)
      {
        CLog::Log(LOGINFO,
                  "TraktScrobbler: Discarding stale retry identification (content changed)");
        return;
      }

      if (identified)
      {
        m_contentIdentified = true;
        CLog::Log(LOGINFO, "TraktScrobbler: Content identified on retry after {}s", elapsed);

        // Start scrobbling if authenticated
        if (IsAuthenticated())
        {
          float progress = GetPlaybackProgress();
          std::string scrobbleJson = BuildScrobbleJson(progress);
          std::string accessToken = m_accessToken;
          std::string imdbSnapshot = m_imdbId;
          const uint64_t authorityGeneration = m_authAuthorityGeneration;

          lock.unlock();
          bool ok = false;
          if (!scrobbleJson.empty())
          {
            std::string response;
            ok = TraktPostWithToken("/scrobble/start", scrobbleJson, response, accessToken);
          }
          lock.lock();

          ClearSensitive(accessToken);
          if (!m_initialized || m_imdbId != imdbSnapshot ||
              m_authAuthorityGeneration != authorityGeneration)
            return;

          if (ok)
          {
            m_scrobbleActive = true;
            m_lastScrobbleUpdateTime = GetCurrentTimeSec();
          }
        }
      }
    }
    else
    {
      // Give up after IDENTIFY_RETRY_SEC
      m_identifyFailed = true;
      CLog::Log(LOGWARNING, "TraktScrobbler: Content identification failed after {}s", elapsed);

      std::string identifyToast = "Could not identify content for scrobbling";
      {
        std::unique_lock lock(m_critSection);
        if (m_bridgeUrl.find("/_c/") != std::string::npos)
          identifyToast += " (ensure Configured addon is installed, not Quick)";
      }
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Trakt", identifyToast,
                                            5000, true);
    }
  }

  // --- Public metadata hydration (best-effort, no auth required) ---
  // If we already have an IMDB id but lack a human title (common for Bridge/URL-identification),
  // hydrate title/year and for episodes also fetch episode title.
  if (m_contentIdentified && !m_imdbId.empty())
  {
    bool isEpisode = (m_season >= 0 && m_episode >= 0);
    bool needHydrate = m_title.empty() || (isEpisode && m_episodeTitle.empty()) || (m_year == 0);
    if (needHydrate)
    {
      int64_t now = GetCurrentTimeSec();
      std::string key = m_imdbId + ":" + std::to_string(m_season) + ":" + std::to_string(m_episode);

      if (key != m_lastPublicHydrateKey || (now - m_lastPublicHydrateAttemptTime) >= 20)
      {
        std::string urlSnapshot = m_mediaUrl;
        std::string idSnapshot = m_imdbId;
        int seasonSnapshot = m_season;
        int episodeSnapshot = m_episode;

        m_lastPublicHydrateAttemptTime = now;
        m_lastPublicHydrateKey = key;

        lock.unlock();
        HydrateFromTraktPublic(idSnapshot, seasonSnapshot, episodeSnapshot, urlSnapshot);
        lock.lock();
        if (!m_initialized)
          return;
      }
    }
  }

  // --- Active-profile token refresh ---
  if (m_scrobbleActive && !m_scrobblePaused && m_contentIdentified && !IsAuthenticated())
  {
    int64_t now = GetCurrentTimeSec();
    if (m_bridgeProfileBacked && m_bridgeTraktEnabled && !m_authInProgress &&
        (now - m_lastConfiguredTokenFetchTime) >= SCROBBLE_UPDATE_INTERVAL_SEC)
    {
      std::string urlSnapshot = m_mediaUrl;
      m_lastConfiguredTokenFetchTime = now;
      lock.unlock();
      bool fetched = FetchAccessTokenFromBridge();
      lock.lock();

      if (!m_initialized || m_mediaUrl != urlSnapshot)
      {
        CLog::Log(LOGINFO,
                  "TraktScrobbler: Discarding stale configured token refresh (content changed)");
        return;
      }

      if (fetched)
        CLog::Log(LOGINFO, "TraktScrobbler: Refreshed active-profile Trakt token from Bridge");
    }
  }

  // --- Periodic scrobble progress update ---
  if (m_scrobbleActive && !m_scrobblePaused && m_contentIdentified && IsAuthenticated())
  {
    int64_t now = GetCurrentTimeSec();
    if ((now - m_lastScrobbleUpdateTime) >= SCROBBLE_UPDATE_INTERVAL_SEC)
    {
      float progress = GetPlaybackProgress();
      if (progress > 0.0f)
      {
        std::string scrobbleJson = BuildScrobbleJson(progress);
        std::string accessToken = m_accessToken;
        std::string imdbSnapshot = m_imdbId;
        const uint64_t authorityGeneration = m_authAuthorityGeneration;

        lock.unlock();
        bool ok = false;
        if (!scrobbleJson.empty())
        {
          std::string response;
          ok = TraktPostWithToken("/scrobble/start", scrobbleJson, response, accessToken);
        }
        lock.lock();

        // Check-and-abort
        ClearSensitive(accessToken);
        if (!m_initialized || m_imdbId != imdbSnapshot ||
            m_authAuthorityGeneration != authorityGeneration)
          return;

        if (ok)
        {
          m_lastScrobbleUpdateTime = GetCurrentTimeSec();
          CLog::Log(LOGINFO, "TraktScrobbler: Periodic update at {:.1f}%", progress);
        }
      }
    }
  }
}

void TraktScrobbler::SetContentInfo(
    const std::string& imdbId, const std::string& title, int year, int season, int episode)
{
  std::unique_lock lock(m_critSection);
  m_imdbId = imdbId;
  m_title = title;
  m_episodeTitle.clear();
  m_logoUrl.clear();
  m_logoFetchedForImdb.clear();
  m_year = year;
  m_season = season;
  m_episode = episode;
  m_contentIdentified = !imdbId.empty() || !title.empty();
  m_lastPublicHydrateAttemptTime = 0;
  m_lastPublicHydrateKey.clear();

  if (m_contentIdentified)
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Content info set - imdb={}, title={}, year={}, S{}E{}",
              imdbId, title, year, season, episode);
  }
}

void TraktScrobbler::SetMediaUrl(const std::string& url)
{
  std::unique_lock lock(m_critSection);
  m_mediaUrl = url;
  CLog::Log(LOGDEBUG, "TraktScrobbler: Media URL set: {}", RedactUrlForLog(url));
}

void TraktScrobbler::ClearContentInfo()
{
  std::unique_lock lock(m_critSection);
  m_imdbId.clear();
  m_traktSlug.clear();
  m_title.clear();
  m_episodeTitle.clear();
  m_logoUrl.clear();
  m_logoFetchedForImdb.clear();
  m_year = 0;
  m_season = -1;
  m_episode = -1;
  m_contentIdentified = false;
  m_scrobbleActive = false;
  m_scrobblePaused = false;
  m_mediaUrl.clear();
  m_resolvedUrl.clear();
  m_lastScrobbleUpdateTime = 0;
  m_playbackStartTime = 0;
  m_identifyFailed = false;
  m_bridgeResumePositionMs.store(0, std::memory_order_relaxed);
  m_lastPublicHydrateAttemptTime = 0;
  m_lastPublicHydrateKey.clear();
}

std::string TraktScrobbler::GetImdbId() const
{
  std::unique_lock lock(m_critSection);
  return m_imdbId;
}

std::string TraktScrobbler::GetTitle() const
{
  std::unique_lock lock(m_critSection);
  return m_title;
}

std::string TraktScrobbler::GetEpisodeTitle() const
{
  std::unique_lock lock(m_critSection);
  return m_episodeTitle;
}

std::string TraktScrobbler::GetLogoUrl() const
{
  std::unique_lock lock(m_critSection);
  return m_logoUrl;
}

int TraktScrobbler::GetYear() const
{
  std::unique_lock lock(m_critSection);
  return m_year;
}

int TraktScrobbler::GetSeason() const
{
  std::unique_lock lock(m_critSection);
  return m_season;
}

int TraktScrobbler::GetEpisode() const
{
  std::unique_lock lock(m_critSection);
  return m_episode;
}

bool TraktScrobbler::IsContentIdentified() const
{
  std::unique_lock lock(m_critSection);
  return m_contentIdentified;
}

std::string TraktScrobbler::GetAccessToken() const
{
  std::unique_lock lock(m_critSection);
  return m_accessToken;
}

std::string TraktScrobbler::GetTraktSlug() const
{
  std::unique_lock lock(m_critSection);
  return m_traktSlug;
}

bool TraktScrobbler::IsScrobbleActive() const
{
  std::unique_lock lock(m_critSection);
  return m_scrobbleActive;
}

void TraktScrobbler::ForceReAuth()
{
  std::unique_lock lock(m_critSection);
  ++m_authAuthorityGeneration;

  if (m_profileRuntimeApplied)
  {
    ClearSensitive(m_accessToken);
    ClearSensitive(m_refreshToken);
    ClearSensitive(m_deviceCode);
    m_tokenExpiry = 0;
    m_authInProgress = false;
    m_lastConfiguredTokenFetchTime = 0;
    CLog::Log(LOGINFO, "TraktScrobbler: Cleared runtime token; Jumpgate profile configuration "
                       "remains authoritative");
    return;
  }

  // Delete token file
  std::string path = CSpecialProtocol::TranslatePath(TOKEN_FILE);
  XFILE::CFile::Delete(path);

  // Reset token state
  m_accessToken.clear();
  m_refreshToken.clear();
  m_tokenExpiry = 0;
  m_deviceCode.clear();
  m_authInProgress = false;

  CLog::Log(LOGINFO, "TraktScrobbler: Forced re-auth - tokens cleared");

  // Start device code auth (does HTTP -- release lock)
  lock.unlock();
  StartDeviceCodeAuth();
}

bool TraktScrobbler::IsAuthenticatedPublic() const
{
  std::unique_lock lock(m_critSection);
  return IsAuthenticated();
}

std::string TraktScrobbler::GetBridgeUrl() const
{
  std::unique_lock lock(m_critSection);
  return m_bridgeUrl;
}

void TraktScrobbler::SetBridgeUrl(const std::string& url)
{
  std::unique_lock lock(m_critSection);
  std::string normalized = NormalizeBridgeUrl(url);
  if (!normalized.empty())
  {
    m_bridgeUrl = normalized;
    CLog::Log(LOGINFO, "TraktScrobbler: Bridge URL overridden to {}", RedactUrlForLog(normalized));
  }
}

void TraktScrobbler::SetBridgeProfile(const std::string& profileId,
                                      const std::string& bridgeOrigin,
                                      const std::string& bridgeBaseUrl,
                                      const std::string& deviceToken,
                                      bool traktEnabled)
{
  const std::string normalizedBase = NormalizeBridgeUrl(bridgeBaseUrl);
  const std::string normalizedOrigin = NormalizeBridgeUrl(bridgeOrigin);
  std::unique_lock lock(m_critSection);
  m_profileRuntimeApplied = true;

  const bool sameProfile = m_bridgeProfileBacked && m_bridgeProfileId == profileId &&
                           m_bridgeOrigin == normalizedOrigin && m_bridgeUrl == normalizedBase &&
                           m_bridgeDeviceToken == deviceToken;
  if (sameProfile)
  {
    const bool enabled = traktEnabled && !m_bridgeDeviceToken.empty();
    if (enabled != m_bridgeTraktEnabled)
    {
      ++m_authAuthorityGeneration;
      ClearSensitive(m_accessToken);
      ClearSensitive(m_refreshToken);
      m_tokenExpiry = 0;
      m_authInProgress = false;
      ClearSensitive(m_deviceCode);
      m_scrobbleActive = false;
      m_scrobblePaused = false;
    }
    m_bridgeTraktEnabled = enabled;
    return;
  }

  ++m_authAuthorityGeneration;
  ClearSensitive(m_bridgeDeviceToken);
  ClearSensitive(m_accessToken);
  ClearSensitive(m_refreshToken);
  m_tokenExpiry = 0;
  m_authInProgress = false;
  ClearSensitive(m_deviceCode);
  m_scrobbleActive = false;
  m_scrobblePaused = false;

  m_bridgeProfileId = profileId;
  m_bridgeOrigin = normalizedOrigin;
  m_bridgeUrl = normalizedBase;
  m_bridgeDeviceToken = deviceToken;
  m_bridgeProfileBacked = !m_bridgeProfileId.empty();
  const bool credentialsReady = m_bridgeProfileBacked && !m_bridgeOrigin.empty() &&
                                !m_bridgeUrl.empty() && !m_bridgeDeviceToken.empty();
  m_bridgeTraktEnabled = credentialsReady && traktEnabled;
  m_bridgeDetected = m_bridgeProfileBacked;

  if (!m_bridgeProfileBacked)
  {
    ClearSensitive(m_bridgeDeviceToken);
    m_bridgeProfileId.clear();
    m_bridgeOrigin.clear();
    m_bridgeUrl = BRIDGE_CLOUD_URL;
    m_bridgeDetected = false;
  }
}

void TraktScrobbler::ClearBridgeProfile()
{
  std::unique_lock lock(m_critSection);
  ++m_authAuthorityGeneration;
  m_profileRuntimeApplied = true;
  ClearSensitive(m_bridgeDeviceToken);
  ClearSensitive(m_accessToken);
  ClearSensitive(m_refreshToken);
  m_bridgeProfileId.clear();
  m_bridgeOrigin.clear();
  m_bridgeProfileBacked = false;
  m_bridgeTraktEnabled = false;
  m_tokenExpiry = 0;
  m_authInProgress = false;
  ClearSensitive(m_deviceCode);
  m_scrobbleActive = false;
  m_scrobblePaused = false;
  m_bridgeUrl = BRIDGE_CLOUD_URL;
  m_bridgeDetected = false;
}

bool TraktScrobbler::IsBridgeProfileBacked() const
{
  std::unique_lock lock(m_critSection);
  return m_bridgeProfileBacked;
}

std::string TraktScrobbler::NormalizeBridgeUrl(const std::string& url) const
{
  std::string normalized = url;
  StringUtils::Trim(normalized);
  if (normalized.empty())
    return normalized;

  if (StringUtils::StartsWithNoCase(normalized, "stremio://"))
    normalized = "https://" + normalized.substr(10);

  size_t cut = normalized.find_first_of("?#");
  if (cut != std::string::npos)
    normalized = normalized.substr(0, cut);

  while (normalized.size() > 1 && normalized.back() == '/')
    normalized.pop_back();

  const std::string manifestSuffix = "/manifest.json";
  if (StringUtils::EndsWithNoCase(normalized, manifestSuffix))
    normalized = normalized.substr(0, normalized.size() - manifestSuffix.size());

  while (normalized.size() > 1 && normalized.back() == '/')
    normalized.pop_back();

  return normalized;
}

std::string TraktScrobbler::BuildBridgeEndpoint(const std::string& bridgeUrl,
                                                const std::string& endpoint) const
{
  std::string base = NormalizeBridgeUrl(bridgeUrl);
  if (base.empty())
    return "";

  if (endpoint.empty())
    return base;

  if (endpoint.front() == '/')
    return base + endpoint;

  return base + "/" + endpoint;
}

// --- Auth ---

bool TraktScrobbler::IsAuthenticated() const
{
  if (m_accessToken.empty())
    return false;

  // Check if token is expired (with 5 min buffer)
  if (m_tokenExpiry > 0 && GetCurrentTimeSec() >= (m_tokenExpiry - 300))
  {
    // Token expired or about to expire
    return false;
  }

  return true;
}

bool TraktScrobbler::LoadTokens()
{
  // Caller holds m_critSection. Profile authority permanently disables the
  // legacy plaintext token store for this scrobbler instance.
  if (m_profileRuntimeApplied)
    return false;

  std::string path = CSpecialProtocol::TranslatePath(TOKEN_FILE);

  XFILE::CFile file;
  std::vector<uint8_t> buffer;
  ssize_t bytesRead = file.LoadFile(path, buffer);
  if (bytesRead <= 0)
  {
    CLog::Log(LOGDEBUG, "TraktScrobbler: No token file found at {}", path);
    return false;
  }

  std::string json(buffer.begin(), buffer.end());
  CVariant data;
  if (!CJSONVariantParser::Parse(json, data))
  {
    ClearSensitive(json);
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to parse token file");
    return false;
  }
  ClearSensitive(json);

  m_accessToken = data["access_token"].asString();
  m_refreshToken = data["refresh_token"].asString();
  m_tokenExpiry = data["token_expiry"].asInteger(0);

  CLog::Log(LOGINFO, "TraktScrobbler: Tokens loaded successfully");

  // If token is expired, try to refresh (happens during Initialize under lock,
  // RefreshAccessToken will manage its own lock internally -- but we hold the
  // lock here from Initialize, and CCriticalSection is recursive, so this is safe)
  if (!m_accessToken.empty() && m_tokenExpiry > 0 && GetCurrentTimeSec() >= m_tokenExpiry)
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Token expired, attempting refresh");
    if (!RefreshAccessToken())
    {
      CLog::Log(LOGWARNING, "TraktScrobbler: Token refresh failed, re-auth needed");
      m_accessToken.clear();
      m_refreshToken.clear();
      return false;
    }
  }

  return !m_accessToken.empty();
}

bool TraktScrobbler::SaveTokens()
{
  // Caller must hold m_critSection or ensure exclusive access
  if (m_profileRuntimeApplied)
  {
    CLog::Log(LOGWARNING,
              "TraktScrobbler: Refused legacy token persistence under profile authority");
    return false;
  }

  CVariant data(CVariant::VariantTypeObject);
  data["access_token"] = m_accessToken;
  data["refresh_token"] = m_refreshToken;
  data["token_expiry"] = m_tokenExpiry;

  std::string json;
  if (!CJSONVariantWriter::Write(data, json, true))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to serialize tokens");
    return false;
  }
  data = CVariant{};

  std::string path = CSpecialProtocol::TranslatePath(TOKEN_FILE);

  XFILE::CFile file;
  if (!file.OpenForWrite(path, true))
  {
    ClearSensitive(json);
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to open token file for writing: {}", path);
    return false;
  }

  ssize_t written = file.Write(json.c_str(), json.size());
  file.Close();
  const size_t expected = json.size();
  ClearSensitive(json);

  if (written != static_cast<ssize_t>(expected))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to write tokens");
    return false;
  }

  CLog::Log(LOGINFO, "TraktScrobbler: Tokens saved successfully");
  return true;
}

bool TraktScrobbler::FetchAccessTokenFromBridge()
{
  std::unique_lock lock(m_critSection);
  const std::string bridgeOrigin = NormalizeBridgeUrl(m_bridgeOrigin);
  const std::string profileId = m_bridgeProfileId;
  std::string deviceToken = m_bridgeDeviceToken;
  const bool profileBacked = m_bridgeProfileBacked && m_bridgeTraktEnabled;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;
  lock.unlock();

  if (!profileBacked || bridgeOrigin.empty() || profileId.empty() || deviceToken.empty())
  {
    ClearSensitive(deviceToken);
    return false;
  }

  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Authorization", "Bearer " + deviceToken);
  curl.SetTimeout(5);
  std::string response;
  const std::string url = BuildBridgeEndpoint(bridgeOrigin, "/v1/trakt/token");
  CURL requestUrl(url);
  requestUrl.SetProtocolOption("redirect-limit", "0");

  const bool requested = curl.Get(requestUrl.Get(), response);
  ClearSensitive(deviceToken);
  if (!requested)
    return false;

  CVariant data;
  if (!CJSONVariantParser::Parse(response, data) || !data.isObject())
  {
    ClearSensitive(response);
    return false;
  }
  ClearSensitive(response);

  if (!data.isMember("profile_id") || data["profile_id"].asString() != profileId)
  {
    data = CVariant{};
    return false;
  }

  std::string accessToken = data.isMember("access_token") ? data["access_token"].asString() : "";
  if (accessToken.empty())
  {
    data = CVariant{};
    return false;
  }

  const int64_t tokenExpiry = data.isMember("token_expiry") ? data["token_expiry"].asInteger(0) : 0;
  data = CVariant{};
  if (tokenExpiry <= GetCurrentTimeSec() + 300)
  {
    ClearSensitive(accessToken);
    return false;
  }

  lock.lock();
  if (!m_bridgeProfileBacked || !m_bridgeTraktEnabled ||
      m_authAuthorityGeneration != authorityGeneration || m_bridgeProfileId != profileId ||
      m_bridgeOrigin != bridgeOrigin)
  {
    lock.unlock();
    ClearSensitive(accessToken);
    return false;
  }
  ClearSensitive(m_accessToken);
  m_accessToken = accessToken;
  m_tokenExpiry = tokenExpiry;
  ClearSensitive(m_refreshToken);
  m_lastConfiguredTokenFetchTime = GetCurrentTimeSec();
  lock.unlock();
  ClearSensitive(accessToken);

  return true;
}

// ---------------------------------------------------------------------------
// RefreshAccessToken: Manages its own locking. Reads token under lock,
// does HTTP without lock, writes result under lock.
// ---------------------------------------------------------------------------
bool TraktScrobbler::RefreshAccessToken()
{
  std::unique_lock lock(m_critSection);

  if (m_profileRuntimeApplied || m_refreshToken.empty())
    return false;

  // Copy state needed for HTTP
  std::string refreshToken = m_refreshToken;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;

  // Build request body under lock (uses member)
  CVariant body(CVariant::VariantTypeObject);
  body["refresh_token"] = refreshToken;
  body["client_id"] = std::string(TRAKT_CLIENT_ID);
  body["client_secret"] = std::string(TRAKT_CLIENT_SECRET);
  body["redirect_uri"] = "urn:ietf:wg:oauth:2.0:oob";
  body["grant_type"] = "refresh_token";

  std::string jsonBody;
  if (!CJSONVariantWriter::Write(body, jsonBody, true))
  {
    ClearSensitive(refreshToken);
    return false;
  }
  body = CVariant{};

  // Release lock for HTTP
  lock.unlock();

  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + "/oauth/token";
  std::string response;

  CLog::Log(LOGDEBUG, "TraktScrobbler: POST /oauth/token (refresh)");

  if (!curl.Post(url, jsonBody, response))
  {
    ClearSensitive(refreshToken);
    ClearSensitive(jsonBody);
    CLog::Log(LOGERROR, "TraktScrobbler: Token refresh request failed");
    return false;
  }
  ClearSensitive(jsonBody);

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
  {
    ClearSensitive(refreshToken);
    ClearSensitive(response);
    return false;
  }
  ClearSensitive(response);

  std::string newAccessToken = result["access_token"].asString();
  std::string newRefreshToken = result["refresh_token"].asString();
  int64_t expiresIn = result["expires_in"].asInteger(7776000); // 90 days default
  result = CVariant{};

  if (newAccessToken.empty())
  {
    ClearSensitive(refreshToken);
    ClearSensitive(newRefreshToken);
    return false;
  }

  // Re-acquire lock to write results
  lock.lock();
  if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration ||
      m_refreshToken != refreshToken)
  {
    lock.unlock();
    ClearSensitive(refreshToken);
    ClearSensitive(newAccessToken);
    ClearSensitive(newRefreshToken);
    CLog::Log(LOGDEBUG,
              "TraktScrobbler: Discarded stale legacy token refresh after authority change");
    return false;
  }

  ClearSensitive(m_accessToken);
  ClearSensitive(m_refreshToken);
  m_accessToken = std::move(newAccessToken);
  m_refreshToken = std::move(newRefreshToken);
  m_tokenExpiry = GetCurrentTimeSec() + expiresIn;

  const bool saved = SaveTokens();
  lock.unlock();
  ClearSensitive(refreshToken);
  if (!saved)
  {
    lock.lock();
    if (!m_profileRuntimeApplied && m_authAuthorityGeneration == authorityGeneration)
    {
      ClearSensitive(m_accessToken);
      ClearSensitive(m_refreshToken);
      m_tokenExpiry = 0;
    }
    lock.unlock();
    CLog::Log(LOGERROR, "TraktScrobbler: Refreshed token could not be persisted");
    return false;
  }
  CLog::Log(LOGINFO, "TraktScrobbler: Token refreshed successfully");
  return true;
}

// ---------------------------------------------------------------------------
// StartDeviceCodeAuth: Manages its own locking. Does HTTP without lock.
// ---------------------------------------------------------------------------
void TraktScrobbler::StartDeviceCodeAuth()
{
  std::unique_lock lock(m_critSection);
  if (m_profileRuntimeApplied || m_authInProgress)
    return;

  const uint64_t authorityGeneration = m_authAuthorityGeneration;
  m_authInProgress = true;
  lock.unlock();

  auto abandonRequest = [this, authorityGeneration]()
  {
    std::unique_lock stateLock(m_critSection);
    if (!m_profileRuntimeApplied && m_authAuthorityGeneration == authorityGeneration &&
        m_deviceCode.empty())
      m_authInProgress = false;
  };

  // Build request body (no member access needed)
  CVariant body(CVariant::VariantTypeObject);
  body["client_id"] = std::string(TRAKT_CLIENT_ID);

  std::string jsonBody;
  if (!CJSONVariantWriter::Write(body, jsonBody, true))
  {
    abandonRequest();
    return;
  }
  body = CVariant{};

  // HTTP without lock
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + "/oauth/device/code";
  std::string response;

  CLog::Log(LOGDEBUG, "TraktScrobbler: POST /oauth/device/code");

  if (!curl.Post(url, jsonBody, response))
  {
    ClearSensitive(jsonBody);
    abandonRequest();
    CLog::Log(LOGERROR, "TraktScrobbler: Device code request failed");
    return;
  }
  ClearSensitive(jsonBody);

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
  {
    ClearSensitive(response);
    abandonRequest();
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to parse device code response");
    return;
  }
  ClearSensitive(response);

  std::string deviceCode = result["device_code"].asString();
  std::string userCode = result["user_code"].asString();
  int pollInterval = result["interval"].asInteger(5);
  result = CVariant{};

  if (deviceCode.empty() || userCode.empty())
  {
    ClearSensitive(deviceCode);
    ClearSensitive(userCode);
    abandonRequest();
    CLog::Log(LOGERROR, "TraktScrobbler: Invalid device code response");
    return;
  }
  // Acquire lock to write auth state
  lock.lock();
  if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration ||
      !m_authInProgress)
  {
    lock.unlock();
    ClearSensitive(deviceCode);
    ClearSensitive(userCode);
    CLog::Log(LOGDEBUG,
              "TraktScrobbler: Discarded stale device-code response after authority change");
    return;
  }
  ClearSensitive(m_deviceCode);
  m_deviceCode = std::move(deviceCode);
  m_pollIntervalSec = pollInterval;
  m_lastPollTime = GetCurrentTimeSec();
  lock.unlock();

  // Show toast notification with the user code (no lock needed)
  std::string message = "Visit trakt.tv/activate\nCode: " + userCode;
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Trakt", message, 10000, true);
  ClearSensitive(userCode);
  ClearSensitive(message);

  CLog::Log(LOGINFO, "TraktScrobbler: Device auth started (code shown on screen)");
}

// ---------------------------------------------------------------------------
// PollForToken: Manages its own locking. Does HTTP without lock.
// ---------------------------------------------------------------------------
void TraktScrobbler::PollForToken()
{
  std::unique_lock lock(m_critSection);

  if (m_profileRuntimeApplied || !m_authInProgress || m_deviceCode.empty())
    return;

  int64_t now = GetCurrentTimeSec();
  if ((now - m_lastPollTime) < m_pollIntervalSec)
    return;

  m_lastPollTime = now;

  // Copy state for HTTP
  std::string deviceCode = m_deviceCode;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;

  // Build request body
  CVariant body(CVariant::VariantTypeObject);
  body["code"] = deviceCode;
  body["client_id"] = std::string(TRAKT_CLIENT_ID);
  body["client_secret"] = std::string(TRAKT_CLIENT_SECRET);

  std::string jsonBody;
  if (!CJSONVariantWriter::Write(body, jsonBody, true))
  {
    ClearSensitive(deviceCode);
    return;
  }
  body = CVariant{};

  // Release lock for HTTP
  lock.unlock();

  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + "/oauth/device/token";
  std::string response;

  CLog::Log(LOGDEBUG, "TraktScrobbler: POST /oauth/device/token (poll)");

  if (!curl.Post(url, jsonBody, response))
  {
    ClearSensitive(deviceCode);
    ClearSensitive(jsonBody);
    CLog::Log(LOGDEBUG, "TraktScrobbler: Auth poll - not yet authorized");
    return;
  }
  ClearSensitive(jsonBody);

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
  {
    ClearSensitive(deviceCode);
    ClearSensitive(response);
    return;
  }
  ClearSensitive(response);

  std::string newAccessToken = result["access_token"].asString();
  std::string newRefreshToken = result["refresh_token"].asString();
  int64_t expiresIn = result["expires_in"].asInteger(7776000);
  result = CVariant{};

  if (newAccessToken.empty())
  {
    ClearSensitive(deviceCode);
    ClearSensitive(newRefreshToken);
    return;
  }

  // Re-acquire lock to write auth result
  lock.lock();

  // Check if auth was cancelled while we were doing HTTP
  if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration ||
      !m_authInProgress || m_deviceCode != deviceCode)
  {
    lock.unlock();
    ClearSensitive(deviceCode);
    ClearSensitive(newAccessToken);
    ClearSensitive(newRefreshToken);
    CLog::Log(LOGDEBUG, "TraktScrobbler: Auth state changed during poll, discarding");
    return;
  }

  ClearSensitive(m_accessToken);
  ClearSensitive(m_refreshToken);
  m_accessToken = std::move(newAccessToken);
  m_refreshToken = std::move(newRefreshToken);
  m_tokenExpiry = GetCurrentTimeSec() + expiresIn;
  m_authInProgress = false;
  ClearSensitive(m_deviceCode);
  const bool saved = SaveTokens();
  ClearSensitive(deviceCode);
  if (!saved)
  {
    ClearSensitive(m_accessToken);
    ClearSensitive(m_refreshToken);
    m_tokenExpiry = 0;
    CLog::Log(LOGERROR, "TraktScrobbler: Device token could not be persisted");
    return;
  }

  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Trakt", "Authenticated!", 5000,
                                        true);
  CLog::Log(LOGINFO, "TraktScrobbler: Authentication successful!");

  // If content is identified, start scrobbling now
  if (m_contentIdentified)
  {
    float progress = GetPlaybackProgress();
    std::string scrobbleJson = BuildScrobbleJson(progress);
    std::string accessToken = m_accessToken;
    std::string imdbSnapshot = m_imdbId;

    lock.unlock();
    bool ok = false;
    if (!scrobbleJson.empty())
    {
      std::string resp;
      ok = TraktPostWithToken("/scrobble/start", scrobbleJson, resp, accessToken);
    }
    lock.lock();

    if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration ||
        m_imdbId != imdbSnapshot)
    {
      ClearSensitive(accessToken);
      return;
    }

    if (ok)
    {
      m_scrobbleActive = true;
      m_lastScrobbleUpdateTime = GetCurrentTimeSec();
    }
    ClearSensitive(accessToken);
  }
}

// --- Scrobble API ---

bool TraktScrobbler::ScrobbleStart(float progress)
{
  std::string json = BuildScrobbleJson(progress);
  if (json.empty())
    return false;

  std::string response;
  return TraktPost("/scrobble/start", json, response);
}

bool TraktScrobbler::ScrobbleStop(float progress)
{
  std::string json = BuildScrobbleJson(progress);
  if (json.empty())
    return false;

  std::string response;
  return TraktPost("/scrobble/stop", json, response);
}

bool TraktScrobbler::ScrobblePause(float progress)
{
  std::string json = BuildScrobbleJson(progress);
  if (json.empty())
    return false;

  std::string response;
  return TraktPost("/scrobble/pause", json, response);
}

bool TraktScrobbler::SyncWatchHistory()
{
  std::unique_lock lock(m_critSection);

  if (!IsAuthenticated() || (m_imdbId.empty() && m_traktSlug.empty()))
    return false;

  std::string json = BuildSyncHistoryJson();
  std::string accessToken = m_accessToken;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;

  lock.unlock();

  if (json.empty())
  {
    ClearSensitive(accessToken);
    return false;
  }

  std::string response;
  const bool posted = TraktPostWithToken("/sync/history", json, response, accessToken);
  ClearSensitive(accessToken);
  lock.lock();
  const bool current = m_authAuthorityGeneration == authorityGeneration;
  lock.unlock();
  return posted && current;
}

// ---------------------------------------------------------------------------
// BuildSyncHistoryJson: Builds JSON for /sync/history from current member state.
// Caller MUST hold m_critSection.
// ---------------------------------------------------------------------------
std::string TraktScrobbler::BuildSyncHistoryJson()
{
  if (m_imdbId.empty() && m_traktSlug.empty())
    return "";

  // Build ISO 8601 timestamp for watched_at
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  struct tm utc;
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  char timeBuf[32];
  std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
  std::string watchedAt = timeBuf;

  CVariant root(CVariant::VariantTypeObject);

  bool isEpisode = (m_season >= 0 && m_episode >= 0);

  if (isEpisode)
  {
    CVariant ids(CVariant::VariantTypeObject);
    if (!m_imdbId.empty())
      ids["imdb"] = m_imdbId;
    if (!m_traktSlug.empty())
      ids["slug"] = m_traktSlug;

    CVariant ep(CVariant::VariantTypeObject);
    ep["number"] = m_episode;
    ep["watched_at"] = watchedAt;

    CVariant episodes(CVariant::VariantTypeArray);
    episodes.push_back(ep);

    CVariant seasonObj(CVariant::VariantTypeObject);
    seasonObj["number"] = m_season;
    seasonObj["episodes"] = episodes;

    CVariant seasons(CVariant::VariantTypeArray);
    seasons.push_back(seasonObj);

    CVariant show(CVariant::VariantTypeObject);
    show["ids"] = ids;
    show["seasons"] = seasons;

    CVariant shows(CVariant::VariantTypeArray);
    shows.push_back(show);

    root["shows"] = shows;
  }
  else
  {
    CVariant ids(CVariant::VariantTypeObject);
    if (!m_imdbId.empty())
      ids["imdb"] = m_imdbId;
    if (!m_traktSlug.empty())
      ids["slug"] = m_traktSlug;

    CVariant movie(CVariant::VariantTypeObject);
    movie["ids"] = ids;
    movie["watched_at"] = watchedAt;

    CVariant movies(CVariant::VariantTypeArray);
    movies.push_back(movie);

    root["movies"] = movies;
  }

  std::string json;
  if (!CJSONVariantWriter::Write(root, json, true))
    return "";

  return json;
}

// --- Content Identification ---

void TraktScrobbler::DetectBridgeUrl()
{
  // Called without lock from Initialize (lock released before this call).
  // Only touches m_bridgeUrl which we'll write under lock at the end.

  // Check if user configured a URL via settings
  std::unique_lock lock(m_critSection);
  std::string currentBridgeUrl = m_bridgeUrl;
  const bool profileBacked = m_bridgeProfileBacked;
  lock.unlock();

  if (profileBacked)
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Using the active authenticated Bridge profile");
    return;
  }

  if (!currentBridgeUrl.empty() && currentBridgeUrl != BRIDGE_CLOUD_URL)
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Using explicit development Bridge URL: {}",
              RedactUrlForLog(currentBridgeUrl));
    return;
  }

  // Try localhost first (ADB reverse or local Bridge -- useful for development)
  XFILE::CCurlFile curl;
  curl.SetTimeout(2);
  std::string response;

  if (curl.Get(std::string(BRIDGE_LOCAL_URL) + "/manifest.json", response))
  {
    CVariant data;
    if (CJSONVariantParser::Parse(response, data))
    {
      const std::string addonId = data["id"].asString();
      if (addonId == "com.jumpgate.bridge")
      {
        lock.lock();
        m_bridgeUrl = BRIDGE_LOCAL_URL;
        lock.unlock();
        CLog::Log(LOGINFO, "TraktScrobbler: Bridge auto-detected locally at {}", BRIDGE_LOCAL_URL);
        return;
      }
    }
  }

  // Fall back to cloud URL (default for end users)
  lock.lock();
  m_bridgeUrl = BRIDGE_CLOUD_URL;
  lock.unlock();
  CLog::Log(LOGINFO, "TraktScrobbler: Using cloud Bridge at {}", BRIDGE_CLOUD_URL);
}

// ---------------------------------------------------------------------------
// QueryBridgeServer: Called WITHOUT the lock held (from IdentifyContent).
// Reads bridge URL under brief lock, does HTTP without lock, writes results
// into output parameters. Caller is responsible for writing to member state.
// ---------------------------------------------------------------------------
bool TraktScrobbler::QueryBridgeServer()
{
  std::unique_lock lock(m_critSection);
  const std::string bridgeUrl = NormalizeBridgeUrl(m_bridgeUrl);
  lock.unlock();

  if (bridgeUrl.empty())
    return false;

  const std::string identifyUrl = BuildBridgeEndpoint(bridgeUrl, "/identify");
  if (identifyUrl.empty())
    return false;

  // Retry a short-lived stream-registration race, but never search another
  // profile. The selected profile is the only allowed identity boundary.
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    if (attempt > 0)
    {
      CLog::Log(LOGDEBUG, "TraktScrobbler: Bridge retry {}/3 after 2s delay", attempt + 1);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    XFILE::CCurlFile curl;
    curl.SetTimeout(3);
    std::string response;
    if (!curl.Get(identifyUrl, response))
    {
      CLog::Log(LOGDEBUG, "TraktScrobbler: Bridge server query failed (attempt {})", attempt + 1);
      continue;
    }

    CVariant data;
    if (!CJSONVariantParser::Parse(response, data) || !data.isObject())
      continue;
    if (!data.isMember("found") || !data["found"].asBoolean())
      continue;

    const std::string imdb = data["imdb"].asString();
    if (imdb.empty())
      continue;

    int season = -1;
    int episode = -1;
    const std::string seasonStr = data["season"].asString();
    const std::string episodeStr = data["episode"].asString();
    if (!seasonStr.empty())
      season = std::atoi(seasonStr.c_str());
    if (!episodeStr.empty())
      episode = std::atoi(episodeStr.c_str());
    const std::string logoUrl = data["logo"].asString();

    lock.lock();
    if (!StringUtils::EqualsNoCase(m_bridgeUrl, bridgeUrl))
    {
      lock.unlock();
      CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale Bridge identification");
      return false;
    }
    m_imdbId = imdb;
    if (season >= 0)
      m_season = season;
    if (episode >= 0)
      m_episode = episode;
    if (!logoUrl.empty())
    {
      m_logoUrl = logoUrl;
      m_logoFetchedForImdb = imdb;
    }

    if (data.isMember("resume") && data["resume"].isMember("position"))
    {
      const int resumePos = static_cast<int>(data["resume"]["position"].asInteger(0));
      const int resumeDur = static_cast<int>(data["resume"]["duration"].asInteger(0));
      if (resumeDur > 0 && resumePos > 0 &&
          static_cast<float>(resumePos) / static_cast<float>(resumeDur) <
              Jumpgate::RESUME_DISCARD_RATIO)
      {
        m_bridgeResumePositionMs.store(resumePos, std::memory_order_relaxed);
        CLog::Log(LOGINFO, "TraktScrobbler: Bridge resume data - pos={} dur={}", resumePos,
                  resumeDur);
      }
    }
    lock.unlock();

    CLog::Log(LOGINFO, "TraktScrobbler: Bridge identified - {} S{}E{} (attempt {}/3)", imdb, season,
              episode, attempt + 1);
    return true;
  }

  CLog::Log(LOGDEBUG, "TraktScrobbler: Active Bridge returned no identification");
  return false;
}

bool TraktScrobbler::FetchLogoFromBridge(const std::string& imdbId,
                                         const std::string& mediaUrlSnapshot)
{
  if (imdbId.empty())
    return false;

  std::unique_lock lock(m_critSection);
  if (m_logoFetchedForImdb == imdbId)
    return false;

  // Mark as attempted to avoid hammering the Bridge if it has no TMDB key or no logo.
  m_logoFetchedForImdb = imdbId;
  std::string bridgeUrl = m_bridgeUrl;
  lock.unlock();

  if (bridgeUrl.empty())
    return false;

  std::string url = BuildBridgeEndpoint(bridgeUrl, "/meta/" + imdbId);
  if (url.empty())
    return false;

  XFILE::CCurlFile curl;
  // Logo fetch is non-critical but can require a second TMDB hop on the bridge.
  // Keep this slightly above identify timeout so metadata can land without blocking playback start.
  curl.SetTimeout(5);
  std::string response;

  if (!curl.Get(url, response))
    return false;

  CVariant data;
  if (!CJSONVariantParser::Parse(response, data))
    return false;

  std::string logoUrl = data["logo"].asString();
  if (logoUrl.empty())
    return false;

  lock.lock();
  if (m_mediaUrl != mediaUrlSnapshot || m_imdbId != imdbId)
  {
    lock.unlock();
    return false;
  }

  m_logoUrl = logoUrl;
  lock.unlock();

  CLog::Log(LOGINFO, "TraktScrobbler: Logo URL set via Bridge meta endpoint");
  return true;
}

// ---------------------------------------------------------------------------
// IdentifyContent: Called WITHOUT the outer lock held. Manages its own locking
// for member field access. All HTTP I/O happens without the lock.
// ---------------------------------------------------------------------------
bool TraktScrobbler::IdentifyContent()
{
  // Read needed state under lock
  std::unique_lock lock(m_critSection);
  std::string imdbId = m_imdbId;
  std::string mediaUrl = m_mediaUrl;
  std::string resolvedUrl = m_resolvedUrl;
  std::string title = m_title;
  int year = m_year;
  int season = m_season;
  int episode = m_episode;
  std::string accessToken = m_accessToken;
  int64_t tokenExpiry = m_tokenExpiry;
  std::string mediaUrlSnapshot = m_mediaUrl; // for staleness check
  lock.unlock();

  // Layer 1: Already set via SetContentInfo (_mk_* params or intent extras)
  if (!imdbId.empty())
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Content identified via intent/Bridge params - IMDB: {}",
              imdbId);
    // Kick logo fetch first so overlay can update as early as possible.
    FetchLogoFromBridge(imdbId, mediaUrlSnapshot);
    if (title.empty() || year == 0)
      HydrateFromTraktPublic(imdbId, season, episode, mediaUrlSnapshot);
    return true;
  }

  // Layer 2: Parse IMDB ID directly from URL (fast, no network, reliable)
  if (!mediaUrl.empty())
  {
    std::regex imdbPattern("(tt\\d{7,})");
    std::smatch match;
    if (std::regex_search(mediaUrl, match, imdbPattern))
    {
      std::string parsedImdb = match[1].str();
      int parsedSeason = season;
      int parsedEpisode = episode;

      // Try to extract season:episode from Stremio/Torrentio format
      std::regex episodePattern("tt\\d{7,}:(\\d+):(\\d+)");
      std::smatch epMatch;
      if (std::regex_search(mediaUrl, epMatch, episodePattern))
      {
        parsedSeason = std::stoi(epMatch[1].str());
        parsedEpisode = std::stoi(epMatch[2].str());
        CLog::Log(LOGDEBUG, "TraktScrobbler: Parsed S{}E{} from URL", parsedSeason, parsedEpisode);
      }

      // Write results under lock with staleness check
      lock.lock();
      if (m_mediaUrl != mediaUrlSnapshot)
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale URL parse (content changed)");
        lock.unlock();
        return false;
      }
      m_imdbId = parsedImdb;
      m_season = parsedSeason;
      m_episode = parsedEpisode;
      lock.unlock();

      CLog::Log(LOGINFO, "TraktScrobbler: Content identified via URL parsing - IMDB: {}",
                parsedImdb);
      FetchLogoFromBridge(parsedImdb, mediaUrlSnapshot);
      HydrateFromTraktPublic(parsedImdb, parsedSeason, parsedEpisode, mediaUrlSnapshot);
      return true;
    }
  }

  // Layer 3: Bridge server side-channel query (zero-config mode, HTTP with retries)
  // QueryBridgeServer manages its own locking
  if (imdbId.empty())
  {
    bool bridgeResult = QueryBridgeServer();

    // Check staleness after bridge query (which may have taken seconds)
    lock.lock();
    if (m_mediaUrl != mediaUrlSnapshot)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale Bridge result (content changed)");
      return false;
    }
    lock.unlock();

    if (bridgeResult)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Content identified via Bridge server");
      // Hydrate title/year/episode title via Trakt public endpoints now that we have IMDB.
      lock.lock();
      std::string curImdb = m_imdbId;
      int curSeason = m_season;
      int curEpisode = m_episode;
      lock.unlock();
      FetchLogoFromBridge(curImdb, mediaUrlSnapshot);
      HydrateFromTraktPublic(curImdb, curSeason, curEpisode, mediaUrlSnapshot);
      return true;
    }
  }

  // Layer 4: Follow redirects and check response headers on the media URL.
  if (!mediaUrl.empty() && resolvedUrl.empty())
  {
    // HTTP probe without lock
    XFILE::CCurlFile curl;
    curl.SetTimeout(8);

    std::string newResolvedUrl;
    if (curl.Open(CURL(mediaUrl)))
    {
      newResolvedUrl = curl.GetRedirectURL();

      std::string contentDisp = curl.GetHttpHeader().GetValue("Content-Disposition");
      if (!contentDisp.empty())
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Content-Disposition: {}", contentDisp);
        std::regex fnPattern("filename[*]?=[\"']?([^\"';]+)[\"']?");
        std::smatch fnMatch;
        if (std::regex_search(contentDisp, fnMatch, fnPattern))
        {
          std::string cdFilename = fnMatch[1].str();
          CLog::Log(LOGINFO, "TraktScrobbler: Filename from header: {}", cdFilename);
          if (newResolvedUrl.empty() || newResolvedUrl == mediaUrl)
            newResolvedUrl = "http://header/" + cdFilename;
        }
      }
      curl.Close();
    }
    else
    {
      CLog::Log(LOGWARNING, "TraktScrobbler: Failed to probe media URL");
    }

    if (newResolvedUrl.empty())
      newResolvedUrl = mediaUrl;

    // Write resolved URL under lock with staleness check
    lock.lock();
    if (m_mediaUrl != mediaUrlSnapshot)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Discarding stale URL probe (content changed)");
      lock.unlock();
      return false;
    }
    m_resolvedUrl = newResolvedUrl;
    resolvedUrl = newResolvedUrl; // update local for layers below
    lock.unlock();

    if (!newResolvedUrl.empty() && newResolvedUrl != mediaUrl)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Resolved URL: {}", RedactUrlForLog(newResolvedUrl));

      // Try IMDB parse on resolved URL (no HTTP, just regex)
      std::regex imdbPattern("(tt\\d{7,})");
      std::smatch match;
      if (std::regex_search(newResolvedUrl, match, imdbPattern))
      {
        std::string parsedImdb = match[1].str();

        lock.lock();
        if (m_mediaUrl != mediaUrlSnapshot)
        {
          lock.unlock();
          return false;
        }
        m_imdbId = parsedImdb;
        lock.unlock();

        CLog::Log(LOGINFO, "TraktScrobbler: Content identified via resolved URL - IMDB: {}",
                  parsedImdb);
        HydrateFromTraktPublic(parsedImdb, season, episode, mediaUrlSnapshot);
        FetchLogoFromBridge(parsedImdb, mediaUrlSnapshot);
        return true;
      }
    }
  }

  // Layer 5: Search Trakt by title (HTTP)
  bool isAuthenticated =
      !accessToken.empty() && (tokenExpiry <= 0 || GetCurrentTimeSec() < (tokenExpiry - 300));

  if (!title.empty() && isAuthenticated)
  {
    // Build search endpoint
    bool isEpisode = (season >= 0 && episode >= 0);
    std::string type = isEpisode ? "show" : "movie";
    std::string encodedQuery = title;
    StringUtils::Replace(encodedQuery, " ", "+");
    std::string endpoint = "/search/" + type + "?query=" + encodedQuery;
    if (year > 0)
      endpoint += "&years=" + std::to_string(year);

    // HTTP without lock
    std::string response;
    bool searchOk = TraktGetWithToken(endpoint, response, accessToken);

    if (searchOk)
    {
      CVariant results;
      if (CJSONVariantParser::Parse(response, results) && results.isArray() && results.size() > 0)
      {
        const CVariant& topResult = results[0];
        const CVariant& item = topResult[type];

        std::string foundImdb;
        std::string foundSlug;
        std::string foundTitle;
        int foundYear = 0;

        if (item.isMember("ids") && item["ids"].isMember("imdb"))
          foundImdb = item["ids"]["imdb"].asString();
        if (item.isMember("ids") && item["ids"].isMember("slug"))
          foundSlug = item["ids"]["slug"].asString();
        if (item.isMember("title"))
          foundTitle = item["title"].asString();
        if (item.isMember("year"))
          foundYear = item["year"].asInteger(0);

        if (!foundImdb.empty() || !foundSlug.empty())
        {
          lock.lock();
          if (m_mediaUrl != mediaUrlSnapshot)
            return false;
          if (!foundImdb.empty())
            m_imdbId = foundImdb;
          if (!foundSlug.empty())
            m_traktSlug = foundSlug;
          if (m_title.empty() && !foundTitle.empty())
            m_title = foundTitle;
          if (m_year == 0 && foundYear > 0)
            m_year = foundYear;
          lock.unlock();

          CLog::Log(LOGINFO, "TraktScrobbler: Content identified via Trakt search - IMDB: {}",
                    foundImdb);
          return true;
        }
      }
      else
      {
        CLog::Log(LOGWARNING, "TraktScrobbler: No search results for '{}'", title);
      }
    }
  }

  // Layer 6: Extract title from URL path and search Trakt
  for (const auto& tryUrl : {mediaUrl, resolvedUrl})
  {
    if (tryUrl.empty() || !isAuthenticated)
      continue;

    std::string extracted = ExtractTitleFromUrl(tryUrl);
    if (extracted.empty())
      continue;

    CLog::Log(LOGINFO, "TraktScrobbler: Extracted title from URL: '{}'", extracted);

    // Build search endpoint
    lock.lock();
    int curSeason = m_season;
    int curEpisode = m_episode;
    int curYear = m_year;
    lock.unlock();

    bool isEpisode = (curSeason >= 0 && curEpisode >= 0);
    std::string type = isEpisode ? "show" : "movie";
    std::string encodedQuery = extracted;
    StringUtils::Replace(encodedQuery, " ", "+");
    std::string endpoint = "/search/" + type + "?query=" + encodedQuery;
    if (curYear > 0)
      endpoint += "&years=" + std::to_string(curYear);

    // HTTP without lock
    std::string response;
    bool searchOk = TraktGetWithToken(endpoint, response, accessToken);

    if (searchOk)
    {
      CVariant results;
      if (CJSONVariantParser::Parse(response, results) && results.isArray() && results.size() > 0)
      {
        const CVariant& topResult = results[0];
        const CVariant& item = topResult[type];

        std::string foundImdb;
        std::string foundSlug;
        std::string foundTitle;
        int foundYear = 0;

        if (item.isMember("ids") && item["ids"].isMember("imdb"))
          foundImdb = item["ids"]["imdb"].asString();
        if (item.isMember("ids") && item["ids"].isMember("slug"))
          foundSlug = item["ids"]["slug"].asString();
        if (item.isMember("title"))
          foundTitle = item["title"].asString();
        if (item.isMember("year"))
          foundYear = item["year"].asInteger(0);

        if (!foundImdb.empty() || !foundSlug.empty())
        {
          lock.lock();
          if (m_mediaUrl != mediaUrlSnapshot)
            return false;
          if (!foundImdb.empty())
            m_imdbId = foundImdb;
          if (!foundSlug.empty())
            m_traktSlug = foundSlug;
          if (m_title.empty() && !foundTitle.empty())
            m_title = foundTitle;
          if (m_year == 0 && foundYear > 0)
            m_year = foundYear;
          lock.unlock();

          CLog::Log(LOGINFO,
                    "TraktScrobbler: Content identified via URL title extraction - IMDB: {}",
                    foundImdb);
          return true;
        }
      }
    }
  }

  CLog::Log(LOGDEBUG, "TraktScrobbler: Content not yet identified (url={}, title={})",
            RedactUrlForLog(mediaUrl), title);
  return false;
}

bool TraktScrobbler::HydrateFromTraktPublic(const std::string& id,
                                            int season,
                                            int episode,
                                            const std::string& mediaUrlSnapshot)
{
  if (id.empty())
    return false;

  bool isEpisode = (season >= 0 && episode >= 0);

  std::string hydratedTitle;
  int hydratedYear = 0;
  std::string hydratedEpisodeTitle;

  // Trakt public endpoints: client-id only (no bearer token).
  // NOTE: /shows/{id} and /movies/{id} do NOT accept IMDb ids directly. We must
  // resolve IMDb -> slug via /search/imdb/{tt}.
  if (isEpisode)
  {
    std::string searchResp;
    std::string searchEndpoint = "/search/imdb/" + id + "?type=show";
    if (TraktGetWithToken(searchEndpoint, searchResp, ""))
    {
      CVariant results;
      if (CJSONVariantParser::Parse(searchResp, results) && results.isArray() && results.size() > 0)
      {
        const CVariant& showWrap = results[0]["show"];
        hydratedTitle = showWrap["title"].asString();
        hydratedYear = showWrap["year"].asInteger(0);
        std::string slug = showWrap["ids"]["slug"].asString();

        if (!slug.empty())
        {
          std::string epResp;
          std::string epEndpoint = "/shows/" + slug + "/seasons/" + std::to_string(season) +
                                   "/episodes/" + std::to_string(episode) + "?extended=full";
          if (TraktGetWithToken(epEndpoint, epResp, ""))
          {
            CVariant ep;
            if (CJSONVariantParser::Parse(epResp, ep))
            {
              hydratedEpisodeTitle = ep["title"].asString();
            }
          }
        }
      }
    }
  }
  else
  {
    std::string searchResp;
    std::string searchEndpoint = "/search/imdb/" + id + "?type=movie";
    if (TraktGetWithToken(searchEndpoint, searchResp, ""))
    {
      CVariant results;
      if (CJSONVariantParser::Parse(searchResp, results) && results.isArray() && results.size() > 0)
      {
        const CVariant& movieWrap = results[0]["movie"];
        hydratedTitle = movieWrap["title"].asString();
        hydratedYear = movieWrap["year"].asInteger(0);
      }
    }
  }

  if (hydratedTitle.empty() && hydratedYear == 0 && hydratedEpisodeTitle.empty())
    return false;

  // Write results under lock with staleness check.
  std::unique_lock lock(m_critSection);
  if (m_mediaUrl != mediaUrlSnapshot)
    return false;
  if (m_imdbId != id)
    return false;

  bool updated = false;
  if (m_title.empty() && !hydratedTitle.empty())
  {
    m_title = hydratedTitle;
    updated = true;
  }
  if (m_year == 0 && hydratedYear > 0)
  {
    m_year = hydratedYear;
    updated = true;
  }
  if (isEpisode && m_episodeTitle.empty() && !hydratedEpisodeTitle.empty())
  {
    m_episodeTitle = hydratedEpisodeTitle;
    updated = true;
  }

  if (updated)
  {
    if (isEpisode)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Hydrated show/episode titles via Trakt public endpoints");
    }
    else
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Hydrated movie title/year via Trakt public endpoints");
    }
  }

  return updated;
}

bool TraktScrobbler::ParseImdbFromUrl(const std::string& url)
{
  // Pure computation, no lock needed. Writes to m_imdbId, m_season, m_episode
  // but only called from IdentifyContent which manages its own locking.
  std::regex imdbPattern("(tt\\d{7,})");
  std::smatch match;

  if (std::regex_search(url, match, imdbPattern))
  {
    m_imdbId = match[1].str();

    std::regex episodePattern("tt\\d{7,}:(\\d+):(\\d+)");
    std::smatch epMatch;
    if (std::regex_search(url, epMatch, episodePattern))
    {
      m_season = std::stoi(epMatch[1].str());
      m_episode = std::stoi(epMatch[2].str());
      CLog::Log(LOGDEBUG, "TraktScrobbler: Parsed S{}E{} from URL", m_season, m_episode);
    }

    return true;
  }

  return false;
}

std::string TraktScrobbler::ExtractTitleFromUrl(const std::string& url)
{
  // Pure computation on the URL string, no member access needed for the extraction.
  // May write to m_title and m_year, but IdentifyContent now manages those writes.

  auto urlDecode = [](const std::string& encoded) -> std::string
  {
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i)
    {
      if (encoded[i] == '%' && i + 2 < encoded.size())
      {
        int value = 0;
        std::string hex = encoded.substr(i + 1, 2);
        if (std::sscanf(hex.c_str(), "%x", &value) == 1)
        {
          decoded += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      decoded += (encoded[i] == '+') ? ' ' : encoded[i];
    }
    return decoded;
  };

  std::string bestSegment;
  std::string decoded = urlDecode(url);
  std::string decodedLower = decoded;
  StringUtils::ToLower(decodedLower);

  // Tokenized proxy URLs are noisy, but some include a clean filename at the tail
  // (e.g. /proxy/stream/Movie.Title.2025.1080p.mkv). Keep extraction enabled and
  // rely on strict year/SxxEyy anchors + confidence checks below.
  const bool tokenizedUrl = (decodedLower.find("/_token_") != std::string::npos ||
                             decodedLower.find("token=") != std::string::npos);

  std::vector<std::string> segments;
  std::string segment;
  for (char c : decoded)
  {
    if (c == '/')
    {
      if (!segment.empty())
        segments.push_back(segment);
      segment.clear();
    }
    else
      segment += c;
  }
  if (!segment.empty())
    segments.push_back(segment);

  const std::regex yearPattern("(?:19|20)\\d{2}");
  const std::regex seasonEpisodePattern("\\bS\\d{1,2}E\\d{1,2}\\b", std::regex::icase);
  for (const auto& seg : segments)
  {
    if (seg.size() < 5 || seg.size() > 220)
      continue;

    bool hasYear = std::regex_search(seg, yearPattern);
    bool hasSeasonEpisode = std::regex_search(seg, seasonEpisodePattern);

    std::string segLower = seg;
    StringUtils::ToLower(segLower);
    if (tokenizedUrl && !hasYear && !hasSeasonEpisode &&
        (segLower.find("_token_") != std::string::npos ||
         segLower.find("token") != std::string::npos))
    {
      continue;
    }

    if ((hasYear || hasSeasonEpisode) && (seg.size() > bestSegment.size()))
      bestSegment = seg;
  }

  if (bestSegment.empty())
  {
    if (tokenizedUrl)
      CLog::Log(LOGDEBUG, "TraktScrobbler: No anchored filename segment found in tokenized URL");
    return "";
  }

  StringUtils::Replace(bestSegment, ".", " ");

  std::string extractedTitle;
  std::smatch match;
  std::regex titleYearPattern("^(.+?)\\s+\\(?((?:19|20)\\d{2})\\)?\\b");
  if (std::regex_search(bestSegment, match, titleYearPattern))
    extractedTitle = match[1].str();
  else
  {
    std::regex titleEpisodePattern("^(.+?)\\s+S\\d{1,2}E\\d{1,2}\\b", std::regex::icase);
    if (std::regex_search(bestSegment, match, titleEpisodePattern))
      extractedTitle = match[1].str();
  }

  if (!extractedTitle.empty())
  {
    // Note: year is extracted but not written to m_year here.
    // The caller (IdentifyContent) handles member writes under lock.
    std::regex tagsPattern("\\s+(?:S\\d{2}|WEB|BluRay|BDRip|HDRip|DVDRip|HDTV|REMUX).*$",
                           std::regex::icase);
    extractedTitle = std::regex_replace(extractedTitle, tagsPattern, "");
    StringUtils::Trim(extractedTitle);

    // Reject low-confidence token-like strings to avoid wrong scrobbles.
    int letters = 0;
    int spaces = 0;
    bool hasDigit = false;
    for (char c : extractedTitle)
    {
      unsigned char uc = static_cast<unsigned char>(c);
      if (std::isalpha(uc))
        ++letters;
      else if (std::isdigit(uc))
        hasDigit = true;
      else if (c == ' ')
        ++spaces;
    }

    if (letters >= 4 && spaces >= 1 && !hasDigit)
    {
      CLog::Log(LOGDEBUG, "TraktScrobbler: Extracted from URL - title='{}'", extractedTitle);
      return extractedTitle;
    }
  }

  return "";
}

bool TraktScrobbler::SearchTrakt(const std::string& query)
{
  // This method is no longer called from the restructured IdentifyContent
  // (search logic is inlined there for lock management). Kept for backward
  // compatibility with any other callers.
  std::unique_lock lock(m_critSection);
  bool isEpisode = (m_season >= 0 && m_episode >= 0);
  std::string type = isEpisode ? "show" : "movie";
  std::string accessToken = m_accessToken;

  std::string encodedQuery = query;
  StringUtils::Replace(encodedQuery, " ", "+");

  std::string endpoint = "/search/" + type + "?query=" + encodedQuery;
  if (m_year > 0)
    endpoint += "&years=" + std::to_string(m_year);

  lock.unlock();

  std::string response;
  if (!TraktGetWithToken(endpoint, response, accessToken))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Trakt search request failed");
    return false;
  }

  CVariant results;
  if (!CJSONVariantParser::Parse(response, results) || !results.isArray() || results.size() == 0)
  {
    CLog::Log(LOGWARNING, "TraktScrobbler: No search results for '{}'", query);
    return false;
  }

  const CVariant& topResult = results[0];
  const CVariant& item = topResult[type];

  lock.lock();

  if (item.isMember("ids") && item["ids"].isMember("imdb"))
    m_imdbId = item["ids"]["imdb"].asString();

  if (item.isMember("ids") && item["ids"].isMember("slug"))
    m_traktSlug = item["ids"]["slug"].asString();

  if (m_title.empty() && item.isMember("title"))
    m_title = item["title"].asString();

  if (m_year == 0 && item.isMember("year"))
    m_year = item["year"].asInteger(0);

  return !m_imdbId.empty() || !m_traktSlug.empty();
}

// --- HTTP Helpers ---

// ---------------------------------------------------------------------------
// TraktPostWithToken: Performs a Trakt API POST using a provided access token.
// Does NOT access any member fields (except static constants). Thread-safe.
// ---------------------------------------------------------------------------
bool TraktScrobbler::TraktPostWithToken(const std::string& endpoint,
                                        const std::string& jsonBody,
                                        std::string& response,
                                        const std::string& accessToken)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  if (!accessToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + accessToken);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + endpoint;

  CLog::Log(LOGDEBUG, "TraktScrobbler: POST {}", endpoint);

  if (!curl.Post(url, jsonBody, response))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: POST {} failed", endpoint);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// TraktGetWithToken: Performs a Trakt API GET using a provided access token.
// Does NOT access any member fields (except static constants). Thread-safe.
// ---------------------------------------------------------------------------
bool TraktScrobbler::TraktGetWithToken(const std::string& endpoint,
                                       std::string& response,
                                       const std::string& accessToken)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  if (!accessToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + accessToken);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + endpoint;

  CLog::Log(LOGDEBUG, "TraktScrobbler: GET {}", endpoint);

  if (!curl.Get(url, response))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: GET {} failed", endpoint);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// TraktPost: Legacy wrapper that reads m_accessToken under lock, then calls
// TraktPostWithToken. Includes retry-on-failure with token refresh.
// ---------------------------------------------------------------------------
bool TraktScrobbler::TraktPost(const std::string& endpoint,
                               const std::string& jsonBody,
                               std::string& response)
{
  // Read access token under lock
  std::unique_lock lock(m_critSection);
  std::string accessToken = m_accessToken;
  bool authInProgress = m_authInProgress;
  const bool profileRuntimeApplied = m_profileRuntimeApplied;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;
  lock.unlock();

  if (TraktPostWithToken(endpoint, jsonBody, response, accessToken))
  {
    lock.lock();
    const bool current = m_authAuthorityGeneration == authorityGeneration;
    lock.unlock();
    ClearSensitive(accessToken);
    return current;
  }

  // POST failed -- try token refresh and retry once
  if (!profileRuntimeApplied && !accessToken.empty() && !authInProgress)
  {
    lock.lock();
    const bool mayRefresh = !m_profileRuntimeApplied &&
                            m_authAuthorityGeneration == authorityGeneration &&
                            m_accessToken == accessToken;
    lock.unlock();
    if (!mayRefresh)
    {
      ClearSensitive(accessToken);
      return false;
    }

    CLog::Log(LOGINFO, "TraktScrobbler: Attempting token refresh after POST failure");
    if (RefreshAccessToken())
    {
      // Re-read refreshed token
      lock.lock();
      if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration)
      {
        lock.unlock();
        ClearSensitive(accessToken);
        return false;
      }
      ClearSensitive(accessToken);
      accessToken = m_accessToken;
      lock.unlock();

      if (TraktPostWithToken(endpoint, jsonBody, response, accessToken))
      {
        lock.lock();
        const bool current =
            !m_profileRuntimeApplied && m_authAuthorityGeneration == authorityGeneration;
        lock.unlock();
        ClearSensitive(accessToken);
        if (!current)
          return false;
        CLog::Log(LOGINFO, "TraktScrobbler: POST {} succeeded after token refresh", endpoint);
        return true;
      }
    }

    // Refresh failed or retry failed -- trigger full re-auth
    lock.lock();
    if (m_profileRuntimeApplied || m_authAuthorityGeneration != authorityGeneration)
    {
      lock.unlock();
      ClearSensitive(accessToken);
      return false;
    }
    ClearSensitive(m_accessToken);
    ClearSensitive(m_refreshToken);
    m_tokenExpiry = 0;
    lock.unlock();
    ClearSensitive(accessToken);

    CLog::Log(LOGWARNING, "TraktScrobbler: Token refresh failed, forcing re-auth");

    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Trakt",
                                          "Session expired, re-authenticating...", 5000, true);
    StartDeviceCodeAuth();
  }

  ClearSensitive(accessToken);

  return false;
}

bool TraktScrobbler::TraktGet(const std::string& endpoint, std::string& response)
{
  // Read access token under lock
  std::unique_lock lock(m_critSection);
  std::string accessToken = m_accessToken;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;
  lock.unlock();

  const bool requested = TraktGetWithToken(endpoint, response, accessToken);
  ClearSensitive(accessToken);
  lock.lock();
  const bool current = m_authAuthorityGeneration == authorityGeneration;
  lock.unlock();
  return requested && current;
}

std::string TraktScrobbler::BuildScrobbleJson(float progress)
{
  // Reads content fields -- caller must hold m_critSection OR pass copies.
  // Currently called from contexts where lock is held.
  CVariant root(CVariant::VariantTypeObject);

  bool isEpisode = (m_season >= 0 && m_episode >= 0);

  if (isEpisode)
  {
    // TV Episode
    CVariant show(CVariant::VariantTypeObject);
    if (!m_imdbId.empty() || !m_traktSlug.empty())
    {
      CVariant ids(CVariant::VariantTypeObject);
      if (!m_imdbId.empty())
        ids["imdb"] = m_imdbId;
      if (!m_traktSlug.empty())
        ids["slug"] = m_traktSlug;
      show["ids"] = ids;
    }
    if (!m_title.empty())
      show["title"] = m_title;
    if (m_year > 0)
      show["year"] = m_year;

    CVariant ep(CVariant::VariantTypeObject);
    ep["season"] = m_season;
    ep["number"] = m_episode;

    root["show"] = show;
    root["episode"] = ep;
  }
  else
  {
    // Movie
    CVariant movie(CVariant::VariantTypeObject);
    if (!m_imdbId.empty() || !m_traktSlug.empty())
    {
      CVariant ids(CVariant::VariantTypeObject);
      if (!m_imdbId.empty())
        ids["imdb"] = m_imdbId;
      if (!m_traktSlug.empty())
        ids["slug"] = m_traktSlug;
      movie["ids"] = ids;
    }
    if (!m_title.empty())
      movie["title"] = m_title;
    if (m_year > 0)
      movie["year"] = m_year;

    root["movie"] = movie;
  }

  root["progress"] = static_cast<double>(progress);

  std::string json;
  if (!CJSONVariantWriter::Write(root, json, true))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to build scrobble JSON");
    return "";
  }

  return json;
}

int TraktScrobbler::GetTraktResumePosition()
{
  std::unique_lock lock(m_critSection);

  if (!IsAuthenticated() || !m_contentIdentified)
    return 0;

  const std::string type = (m_season >= 0 && m_episode >= 0) ? "episodes" : "movies";
  const std::string endpoint = "/sync/playback/" + type;
  std::string accessToken = m_accessToken;
  const std::string imdbId = m_imdbId;
  const int season = m_season;
  const int episode = m_episode;
  const std::string profileId = m_bridgeProfileId;
  const uint64_t authorityGeneration = m_authAuthorityGeneration;

  lock.unlock();

  std::string response;
  if (!TraktGetWithToken(endpoint, response, accessToken))
  {
    ClearSensitive(accessToken);
    ClearSensitive(response);
    return 0;
  }
  ClearSensitive(accessToken);

  CVariant results;
  if (!CJSONVariantParser::Parse(response, results) || !results.isArray())
  {
    ClearSensitive(response);
    return 0;
  }
  ClearSensitive(response);

  float progress = -1.0f;
  for (unsigned int i = 0; i < results.size(); ++i)
  {
    const CVariant& item = results[i];

    if (type == "movies")
    {
      const CVariant& movie = item["movie"];
      if (movie.isMember("ids"))
      {
        std::string itemImdb = movie["ids"]["imdb"].asString();
        if (itemImdb == imdbId)
        {
          progress = item["progress"].asFloat(0.0f);
          break;
        }
      }
    }
    else
    {
      const CVariant& show = item["show"];
      const CVariant& ep = item["episode"];
      if (show.isMember("ids"))
      {
        std::string itemImdb = show["ids"]["imdb"].asString();
        int itemSeason = ep["season"].asInteger(-1);
        int itemEpisode = ep["number"].asInteger(-1);

        if (itemImdb == imdbId && itemSeason == season && itemEpisode == episode)
        {
          progress = item["progress"].asFloat(0.0f);
          break;
        }
      }
    }
  }

  results = CVariant{};
  if (progress < 0.0f)
    return 0;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  const int64_t totalMs = appPlayer->GetTotalTime();
  if (totalMs <= 0)
    return 0;

  const int posMs = static_cast<int>(progress / 100.0f * static_cast<float>(totalMs));

  lock.lock();
  const bool current = m_initialized && m_authAuthorityGeneration == authorityGeneration &&
                       m_bridgeProfileId == profileId && m_imdbId == imdbId && m_season == season &&
                       m_episode == episode;
  lock.unlock();
  if (!current)
  {
    CLog::Log(LOGDEBUG,
              "TraktScrobbler: Discarded stale personalized resume after authority change");
    return 0;
  }

  if (type == "episodes")
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Trakt resume for {} S{}E{} - {:.1f}% = {} ms", imdbId,
              season, episode, progress, posMs);
  }
  else
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Trakt resume for {} - {:.1f}% = {} ms", imdbId, progress,
              posMs);
  }
  return posMs;
}

float TraktScrobbler::GetPlaybackProgress() const
{
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();

  int64_t time = appPlayer->GetTime();
  int64_t total = appPlayer->GetTotalTime();

  if (total <= 0)
    return 0.0f;

  return static_cast<float>(time) / static_cast<float>(total) * 100.0f;
}
