/*
 *  Copyright (C) 2024 Team ModiKodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TraktScrobbler.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "URL.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "interfaces/AnnouncementManager.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

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
} // namespace

TraktScrobbler::TraktScrobbler() = default;

TraktScrobbler::~TraktScrobbler()
{
  Deinitialize();
}

void TraktScrobbler::Initialize()
{
  std::unique_lock lock(m_critSection);
  if (m_initialized)
    return;

  LoadTokens();
  m_bridgeUrl = BRIDGE_SERVER_URL;
  DetectBridgeUrl();
  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(this, ANNOUNCEMENT::Player);
  m_initialized = true;
  CLog::Log(LOGINFO, "TraktScrobbler: Initialized (bridge={})", m_bridgeUrl);
}

void TraktScrobbler::Deinitialize()
{
  std::unique_lock lock(m_critSection);
  if (!m_initialized)
    return;

  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);
  m_initialized = false;
  CLog::Log(LOGINFO, "TraktScrobbler: Deinitialized");
}

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

  if (message == "OnPlay" || message == "OnResume")
  {
    if (!m_contentIdentified)
    {
      IdentifyContent();
      // If identification failed, mark for deferred retry in ProcessSlow
      if (!m_contentIdentified && m_playbackStartTime == 0)
      {
        m_playbackStartTime = GetCurrentTimeSec();
        m_identifyFailed = false; // will retry in ProcessSlow
        CLog::Log(LOGDEBUG, "TraktScrobbler: Content not yet identified, will retry");
      }
    }

    if (!IsAuthenticated())
    {
      // Try refreshing expired token first
      if (!m_refreshToken.empty() && RefreshAccessToken())
      {
        // Token refreshed, continue to scrobble below
      }
      else if (!m_authInProgress)
      {
        StartDeviceCodeAuth();
        return;
      }
      else
      {
        return;
      }
    }

    if (m_contentIdentified)
    {
      float progress = GetPlaybackProgress();
      if (ScrobbleStart(progress))
      {
        m_scrobbleActive = true;
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
      ScrobblePause(progress);
      CLog::Log(LOGINFO, "TraktScrobbler: Scrobble paused at {:.1f}%", progress);
    }
  }
  else if (message == "OnStop")
  {
    if (m_scrobbleActive)
    {
      float progress = GetPlaybackProgress();
      ScrobbleStop(progress);
      m_scrobbleActive = false;
      CLog::Log(LOGINFO, "TraktScrobbler: Scrobble stopped at {:.1f}%", progress);

      // Sync to watch history if playback >80% complete
      if (progress > 80.0f && m_contentIdentified)
      {
        if (SyncWatchHistory())
          CLog::Log(LOGINFO, "TraktScrobbler: Watch history synced at {:.1f}%", progress);
      }
    }
  }
}

void TraktScrobbler::ProcessSlow()
{
  std::unique_lock lock(m_critSection);
  if (m_authInProgress)
    PollForToken();

  // Deferred content identification retry (gives URL/player time to settle)
  if (!m_contentIdentified && !m_identifyFailed && m_playbackStartTime > 0)
  {
    int64_t elapsed = GetCurrentTimeSec() - m_playbackStartTime;
    if (elapsed <= IDENTIFY_RETRY_SEC)
    {
      if (IdentifyContent())
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Content identified on retry after {}s", elapsed);
        // Start scrobbling now if authenticated
        if (IsAuthenticated())
        {
          float progress = GetPlaybackProgress();
          if (ScrobbleStart(progress))
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
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Warning, "Trakt",
          "Could not identify content for scrobbling", 5000, true);
    }
  }

  // Periodic scrobble progress update (keeps Trakt /users/me/watching fresh)
  if (m_scrobbleActive && m_contentIdentified && IsAuthenticated())
  {
    int64_t now = GetCurrentTimeSec();
    if ((now - m_lastScrobbleUpdateTime) >= SCROBBLE_UPDATE_INTERVAL_SEC)
    {
      float progress = GetPlaybackProgress();
      if (progress > 0.0f && ScrobbleStart(progress))
      {
        m_lastScrobbleUpdateTime = now;
        CLog::Log(LOGINFO, "TraktScrobbler: Periodic update at {:.1f}%", progress);
      }
    }
  }
}

void TraktScrobbler::SetContentInfo(const std::string& imdbId,
                                     const std::string& title,
                                     int year,
                                     int season,
                                     int episode)
{
  std::unique_lock lock(m_critSection);
  m_imdbId = imdbId;
  m_title = title;
  m_year = year;
  m_season = season;
  m_episode = episode;
  m_contentIdentified = !imdbId.empty() || !title.empty();

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
  CLog::Log(LOGDEBUG, "TraktScrobbler: Media URL set: {}", url);
}

void TraktScrobbler::ClearContentInfo()
{
  std::unique_lock lock(m_critSection);
  m_imdbId.clear();
  m_traktSlug.clear();
  m_title.clear();
  m_year = 0;
  m_season = -1;
  m_episode = -1;
  m_contentIdentified = false;
  m_scrobbleActive = false;
  m_mediaUrl.clear();
  m_resolvedUrl.clear();
  m_lastScrobbleUpdateTime = 0;
  m_playbackStartTime = 0;
  m_identifyFailed = false;
  m_bridgeResumePositionMs = 0;
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

void TraktScrobbler::ForceReAuth()
{
  std::unique_lock lock(m_critSection);

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

  // Start device code auth
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
  if (!url.empty())
  {
    m_bridgeUrl = url;
    CLog::Log(LOGINFO, "TraktScrobbler: Bridge URL overridden to {}", url);
  }
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
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to parse token file");
    return false;
  }

  m_accessToken = data["access_token"].asString();
  m_refreshToken = data["refresh_token"].asString();
  m_tokenExpiry = data["token_expiry"].asInteger(0);

  CLog::Log(LOGINFO, "TraktScrobbler: Tokens loaded successfully");

  // If token is expired, try to refresh
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

  std::string path = CSpecialProtocol::TranslatePath(TOKEN_FILE);

  XFILE::CFile file;
  if (!file.OpenForWrite(path, true))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to open token file for writing: {}", path);
    return false;
  }

  ssize_t written = file.Write(json.c_str(), json.size());
  file.Close();

  if (written != static_cast<ssize_t>(json.size()))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to write tokens");
    return false;
  }

  CLog::Log(LOGINFO, "TraktScrobbler: Tokens saved successfully");
  return true;
}

bool TraktScrobbler::RefreshAccessToken()
{
  if (m_refreshToken.empty())
    return false;

  CVariant body(CVariant::VariantTypeObject);
  body["refresh_token"] = m_refreshToken;
  body["client_id"] = std::string(TRAKT_CLIENT_ID);
  body["client_secret"] = std::string(TRAKT_CLIENT_SECRET);
  body["redirect_uri"] = "urn:ietf:wg:oauth:2.0:oob";
  body["grant_type"] = "refresh_token";

  std::string jsonBody;
  CJSONVariantWriter::Write(body, jsonBody, true);

  std::string response;
  if (!TraktPost("/oauth/token", jsonBody, response))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Token refresh request failed");
    return false;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
    return false;

  m_accessToken = result["access_token"].asString();
  m_refreshToken = result["refresh_token"].asString();
  int64_t expiresIn = result["expires_in"].asInteger(7776000); // 90 days default
  m_tokenExpiry = GetCurrentTimeSec() + expiresIn;

  if (m_accessToken.empty())
    return false;

  SaveTokens();
  CLog::Log(LOGINFO, "TraktScrobbler: Token refreshed successfully");
  return true;
}

void TraktScrobbler::StartDeviceCodeAuth()
{
  CVariant body(CVariant::VariantTypeObject);
  body["client_id"] = std::string(TRAKT_CLIENT_ID);

  std::string jsonBody;
  CJSONVariantWriter::Write(body, jsonBody, true);

  std::string response;
  if (!TraktPost("/oauth/device/code", jsonBody, response))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Device code request failed");
    return;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Failed to parse device code response");
    return;
  }

  m_deviceCode = result["device_code"].asString();
  std::string userCode = result["user_code"].asString();
  m_pollIntervalSec = result["interval"].asInteger(5);

  if (m_deviceCode.empty() || userCode.empty())
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Invalid device code response");
    return;
  }

  m_authInProgress = true;
  m_lastPollTime = GetCurrentTimeSec();

  // Show toast notification with the user code
  std::string message = "Visit trakt.tv/activate\nCode: " + userCode;
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Trakt", message, 10000, true);

  CLog::Log(LOGINFO, "TraktScrobbler: Device auth started - code: {}", userCode);
}

void TraktScrobbler::PollForToken()
{
  if (!m_authInProgress || m_deviceCode.empty())
    return;

  int64_t now = GetCurrentTimeSec();
  if ((now - m_lastPollTime) < m_pollIntervalSec)
    return;

  m_lastPollTime = now;

  CVariant body(CVariant::VariantTypeObject);
  body["code"] = m_deviceCode;
  body["client_id"] = std::string(TRAKT_CLIENT_ID);
  body["client_secret"] = std::string(TRAKT_CLIENT_SECRET);

  std::string jsonBody;
  CJSONVariantWriter::Write(body, jsonBody, true);

  std::string response;
  if (!TraktPost("/oauth/device/token", jsonBody, response))
  {
    // 400 = pending, 404 = invalid code, 409 = already approved, 410 = expired, 418 = denied
    // Only 200 is success, all others just return false from Post
    CLog::Log(LOGDEBUG, "TraktScrobbler: Auth poll - not yet authorized");
    return;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
    return;

  m_accessToken = result["access_token"].asString();
  m_refreshToken = result["refresh_token"].asString();
  int64_t expiresIn = result["expires_in"].asInteger(7776000);
  m_tokenExpiry = GetCurrentTimeSec() + expiresIn;

  if (!m_accessToken.empty())
  {
    m_authInProgress = false;
    m_deviceCode.clear();
    SaveTokens();

    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Trakt",
                                           "Authenticated!", 5000, true);
    CLog::Log(LOGINFO, "TraktScrobbler: Authentication successful!");

    // If content is identified, start scrobbling now
    if (m_contentIdentified)
    {
      float progress = GetPlaybackProgress();
      if (ScrobbleStart(progress))
      {
        m_scrobbleActive = true;
        m_lastScrobbleUpdateTime = GetCurrentTimeSec();
      }
    }
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
  if (!IsAuthenticated() || (m_imdbId.empty() && m_traktSlug.empty()))
    return false;

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
    // { "shows": [{ "ids": {...}, "seasons": [{ "number": S, "episodes": [{ "number": E, "watched_at": "..." }] }] }] }
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
    // { "movies": [{ "ids": {...}, "watched_at": "..." }] }
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
    return false;

  std::string response;
  return TraktPost("/sync/history", json, response);
}

// --- Content Identification ---

void TraktScrobbler::DetectBridgeUrl()
{
  // If user configured a URL via settings (SetBridgeUrl), keep it
  if (!m_bridgeUrl.empty() && m_bridgeUrl != BRIDGE_SERVER_URL)
  {
    CLog::Log(LOGINFO, "TraktScrobbler: Using user-configured Bridge URL: {}", m_bridgeUrl);
    return;
  }

  // Try localhost (ADB reverse or local Bridge)
  const std::string localUrl = "http://127.0.0.1:7515";
  XFILE::CCurlFile curl;
  curl.SetTimeout(2);
  std::string response;

  if (curl.Get(localUrl + "/manifest.json", response))
  {
    CVariant data;
    if (CJSONVariantParser::Parse(response, data) &&
        data["id"].asString() == "com.modikodi.bridge")
    {
      m_bridgeUrl = localUrl;
      CLog::Log(LOGINFO, "TraktScrobbler: Bridge auto-detected at {}", m_bridgeUrl);
      return;
    }
  }

  // No Bridge found — keep default but log
  CLog::Log(LOGDEBUG, "TraktScrobbler: Bridge not detected, using default: {}", m_bridgeUrl);
}

bool TraktScrobbler::QueryBridgeServer()
{
  if (m_bridgeUrl.empty())
    return false;

  std::string url = m_bridgeUrl + "/identify";

  // Retry up to 3 times with 2s delay — handles race condition where ModiKodi
  // queries /identify before Stremio's stream request arrives at Bridge
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

    if (!curl.Get(url, response))
    {
      CLog::Log(LOGDEBUG, "TraktScrobbler: Bridge server query failed (attempt {})", attempt + 1);
      continue;
    }

    CVariant data;
    if (!CJSONVariantParser::Parse(response, data))
      continue;

    if (!data["found"].asBoolean())
      continue;

    std::string imdb = data["imdb"].asString();
    if (imdb.empty())
      continue;

    m_imdbId = imdb;

    std::string season = data["season"].asString();
    std::string episode = data["episode"].asString();
    if (!season.empty())
      m_season = std::atoi(season.c_str());
    if (!episode.empty())
      m_episode = std::atoi(episode.c_str());

    // Capture resume data if present in Bridge response
    if (data.isMember("resume") && data["resume"].isMember("position"))
    {
      int resumePos = static_cast<int>(data["resume"]["position"].asInteger(0));
      int resumeDur = static_cast<int>(data["resume"]["duration"].asInteger(0));
      // Only use if not completed (>95% = already watched)
      if (resumeDur > 0 && resumePos > 0 &&
          static_cast<float>(resumePos) / static_cast<float>(resumeDur) < 0.95f)
      {
        m_bridgeResumePositionMs = resumePos;
        CLog::Log(LOGINFO, "TraktScrobbler: Bridge resume data - pos={} dur={}", resumePos, resumeDur);
      }
    }

    CLog::Log(LOGINFO, "TraktScrobbler: Bridge identified - {} S{}E{} (attempt {})",
              m_imdbId, m_season, m_episode, attempt + 1);
    return true;
  }

  CLog::Log(LOGDEBUG, "TraktScrobbler: Bridge server returned not found after 3 attempts");
  return false;
}

bool TraktScrobbler::IdentifyContent()
{
  // Layer 1: Already set via SetContentInfo (_mk_* params or intent extras)
  // Highest priority — _mk_* params are injected by Bridge wrapper mode (100% reliable)
  if (!m_imdbId.empty())
  {
    m_contentIdentified = true;
    CLog::Log(LOGINFO, "TraktScrobbler: Content identified via intent/Bridge params - IMDB: {}", m_imdbId);
    return true;
  }

  // Layer 2: Parse IMDB ID directly from URL (fast, no network, reliable)
  if (!m_mediaUrl.empty() && ParseImdbFromUrl(m_mediaUrl))
  {
    m_contentIdentified = true;
    CLog::Log(LOGINFO, "TraktScrobbler: Content identified via URL parsing - IMDB: {}", m_imdbId);
    return true;
  }

  // Layer 3: Bridge server side-channel query (zero-config mode, 3s timeout)
  // Comes after URL parsing (local+fast) but before heavy HTTP probing
  if (m_imdbId.empty() && QueryBridgeServer())
  {
    m_contentIdentified = true;
    CLog::Log(LOGINFO, "TraktScrobbler: Content identified via Bridge server - IMDB: {}", m_imdbId);
    return true;
  }

  // Layer 4: Follow redirects and check response headers on the media URL.
  // Debrid proxy URLs (e.g. debridmediamanager.com/.../HASH) may redirect to
  // CDN URLs with filenames, or return Content-Disposition with the filename.
  if (!m_mediaUrl.empty() && m_resolvedUrl.empty())
  {
    XFILE::CCurlFile curl;
    curl.SetTimeout(8);

    if (curl.Open(CURL(m_mediaUrl)))
    {
      m_resolvedUrl = curl.GetRedirectURL();

      // Check Content-Disposition header for filename
      std::string contentDisp = curl.GetHttpHeader().GetValue("Content-Disposition");
      if (!contentDisp.empty())
      {
        CLog::Log(LOGINFO, "TraktScrobbler: Content-Disposition: {}", contentDisp);
        // Extract filename from: attachment; filename="Movie.Name.2024.mkv"
        std::regex fnPattern("filename[*]?=[\"']?([^\"';]+)[\"']?");
        std::smatch fnMatch;
        if (std::regex_search(contentDisp, fnMatch, fnPattern))
        {
          std::string cdFilename = fnMatch[1].str();
          CLog::Log(LOGINFO, "TraktScrobbler: Filename from header: {}", cdFilename);
          // Use the filename as a synthetic URL for title extraction in Layer 4
          if (m_resolvedUrl.empty() || m_resolvedUrl == m_mediaUrl)
            m_resolvedUrl = "http://header/" + cdFilename;
        }
      }

      curl.Close();
    }
    else
    {
      CLog::Log(LOGWARNING, "TraktScrobbler: Failed to probe media URL");
    }

    if (m_resolvedUrl.empty())
      m_resolvedUrl = m_mediaUrl; // no redirect, no header filename

    if (!m_resolvedUrl.empty() && m_resolvedUrl != m_mediaUrl)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Resolved URL: {}", m_resolvedUrl);
      if (ParseImdbFromUrl(m_resolvedUrl))
      {
        m_contentIdentified = true;
        CLog::Log(LOGINFO, "TraktScrobbler: Content identified via resolved URL - IMDB: {}", m_imdbId);
        return true;
      }
    }
  }

  // Layer 5: Search Trakt by title (from intent extras)
  if (!m_title.empty() && IsAuthenticated())
  {
    if (SearchTrakt(m_title))
    {
      m_contentIdentified = true;
      CLog::Log(LOGINFO, "TraktScrobbler: Content identified via Trakt search - IMDB: {}",
                m_imdbId);
      return true;
    }
  }

  // Layer 6: Extract title from URL path and search Trakt
  // Try the original URL first, then the resolved URL (after redirects)
  for (const auto& tryUrl : {m_mediaUrl, m_resolvedUrl})
  {
    if (tryUrl.empty() || !IsAuthenticated())
      continue;

    std::string extracted = ExtractTitleFromUrl(tryUrl);
    if (!extracted.empty())
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Extracted title from URL: '{}'", extracted);
      if (SearchTrakt(extracted))
      {
        m_contentIdentified = true;
        CLog::Log(LOGINFO, "TraktScrobbler: Content identified via URL title extraction - IMDB: {}",
                  m_imdbId);
        return true;
      }
    }
  }

  CLog::Log(LOGDEBUG, "TraktScrobbler: Content not yet identified (url={}, title={})",
            m_mediaUrl, m_title);
  return false;
}

bool TraktScrobbler::ParseImdbFromUrl(const std::string& url)
{
  // Look for IMDB ID pattern: tt followed by 7+ digits
  std::regex imdbPattern("(tt\\d{7,})");
  std::smatch match;

  if (std::regex_search(url, match, imdbPattern))
  {
    m_imdbId = match[1].str();

    // Try to extract season:episode from Stremio/Torrentio format: tt1234567:S:E
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
  // URL-decode percent-encoded characters (%20 -> space, etc.)
  auto urlDecode = [](const std::string& encoded) -> std::string {
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

  // Find the longest path segment (likely the release/file name)
  // Split URL by '/' and find the segment that looks like a release name
  std::string bestSegment;
  std::string decoded = urlDecode(url);

  // Split by '/'
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

  // Find the segment most likely to be a release name:
  // - Contains spaces or dots (word separators)
  // - Contains a year (4 digits like 2025)
  // - Longer than typical path components
  for (const auto& seg : segments)
  {
    // Skip short segments, query strings, file extensions
    if (seg.size() < 5)
      continue;

    // Check if it looks like a release name (has year pattern or multiple words)
    bool hasYear = std::regex_search(seg, std::regex("(?:19|20)\\d{2}"));
    bool hasWords = (seg.find(' ') != std::string::npos || seg.find('.') != std::string::npos);

    if (hasYear && (seg.size() > bestSegment.size()))
      bestSegment = seg;
    else if (hasWords && seg.size() > 20 && bestSegment.empty())
      bestSegment = seg;
  }

  if (bestSegment.empty())
    return "";

  // Replace dots with spaces (common in release names)
  StringUtils::Replace(bestSegment, ".", " ");

  // Extract title and year: take words before the year or before quality tags
  // Pattern: "THE HOUSEMAID 2025 UHD AMZN 2160p English" -> "THE HOUSEMAID"
  std::regex titleYearPattern("^(.+?)\\s+((?:19|20)\\d{2})\\b");
  std::smatch match;
  if (std::regex_search(bestSegment, match, titleYearPattern))
  {
    m_title = match[1].str();
    m_year = std::stoi(match[2].str());
    // Trim trailing quality/source tags that snuck in
    // Remove common tags: S01E01, WEB-DL, BluRay, etc.
    std::regex tagsPattern("\\s+(?:S\\d{2}|WEB|BluRay|BDRip|HDRip|DVDRip|HDTV|REMUX).*$",
                           std::regex::icase);
    m_title = std::regex_replace(m_title, tagsPattern, "");
    CLog::Log(LOGDEBUG, "TraktScrobbler: Extracted from URL - title='{}' year={}", m_title, m_year);
    return m_title;
  }

  // No year found in any segment — we can't reliably extract a title
  // (opaque debrid URLs produce garbage matches without a year anchor)
  return "";
}

bool TraktScrobbler::SearchTrakt(const std::string& query)
{
  // Determine if we're searching for a movie or show
  bool isEpisode = (m_season >= 0 && m_episode >= 0);
  std::string type = isEpisode ? "show" : "movie";

  std::string encodedQuery = query;
  // Simple URL encoding for spaces
  StringUtils::Replace(encodedQuery, " ", "+");

  std::string endpoint = "/search/" + type + "?query=" + encodedQuery;
  if (m_year > 0)
    endpoint += "&years=" + std::to_string(m_year);

  std::string response;
  if (!TraktGet(endpoint, response))
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

  // Use the top result
  const CVariant& topResult = results[0];
  const CVariant& item = topResult[type];

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

bool TraktScrobbler::TraktPost(const std::string& endpoint,
                                const std::string& jsonBody,
                                std::string& response)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  if (!m_accessToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + m_accessToken);
  curl.SetTimeout(10);

  std::string url = std::string(TRAKT_API_URL) + endpoint;

  CLog::Log(LOGDEBUG, "TraktScrobbler: POST {}", endpoint);

  if (!curl.Post(url, jsonBody, response))
  {
    CLog::Log(LOGERROR, "TraktScrobbler: POST {} failed", endpoint);

    // If we had a token, it may have expired — try refreshing and retry once
    if (!m_accessToken.empty() && !m_authInProgress)
    {
      CLog::Log(LOGINFO, "TraktScrobbler: Attempting token refresh after POST failure");
      if (RefreshAccessToken())
      {
        // Retry with refreshed token
        XFILE::CCurlFile curl2;
        curl2.SetRequestHeader("Content-Type", "application/json");
        curl2.SetRequestHeader("trakt-api-version", "2");
        curl2.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
        curl2.SetRequestHeader("Authorization", "Bearer " + m_accessToken);
        curl2.SetTimeout(10);
        if (curl2.Post(url, jsonBody, response))
        {
          CLog::Log(LOGINFO, "TraktScrobbler: POST {} succeeded after token refresh", endpoint);
          return true;
        }
      }

      // Refresh failed or retry failed — trigger full re-auth
      CLog::Log(LOGWARNING, "TraktScrobbler: Token refresh failed, forcing re-auth");
      m_accessToken.clear();
      m_refreshToken.clear();
      m_tokenExpiry = 0;
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Warning, "Trakt",
          "Session expired, re-authenticating...", 5000, true);
      StartDeviceCodeAuth();
    }
    return false;
  }

  return true;
}

bool TraktScrobbler::TraktGet(const std::string& endpoint, std::string& response)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("trakt-api-version", "2");
  curl.SetRequestHeader("trakt-api-key", TRAKT_CLIENT_ID);
  if (!m_accessToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + m_accessToken);
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

std::string TraktScrobbler::BuildScrobbleJson(float progress)
{
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
  if (!IsAuthenticated() || !m_contentIdentified)
    return 0;

  // Query Trakt for saved playback position
  std::string type = (m_season >= 0 && m_episode >= 0) ? "episodes" : "movies";
  std::string endpoint = "/sync/playback/" + type;

  std::string response;
  if (!TraktGet(endpoint, response))
    return 0;

  CVariant results;
  if (!CJSONVariantParser::Parse(response, results) || !results.isArray())
    return 0;

  // Find matching entry
  for (unsigned int i = 0; i < results.size(); ++i)
  {
    const CVariant& item = results[i];

    if (type == "movies")
    {
      const CVariant& movie = item["movie"];
      if (movie.isMember("ids"))
      {
        std::string itemImdb = movie["ids"]["imdb"].asString();
        if (itemImdb == m_imdbId)
        {
          float progress = item["progress"].asFloat(0.0f);
          // Trakt stores progress as 0-100 percentage
          // We need to convert to ms using total duration from player
          const auto& components = CServiceBroker::GetAppComponents();
          const auto appPlayer = components.GetComponent<CApplicationPlayer>();
          int64_t totalMs = appPlayer->GetTotalTime();
          if (totalMs > 0)
          {
            int posMs = static_cast<int>(progress / 100.0f * static_cast<float>(totalMs));
            CLog::Log(LOGINFO, "TraktScrobbler: Trakt resume for {} - {:.1f}% = {} ms",
                      m_imdbId, progress, posMs);
            return posMs;
          }
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

        if (itemImdb == m_imdbId && itemSeason == m_season && itemEpisode == m_episode)
        {
          float progress = item["progress"].asFloat(0.0f);
          const auto& components = CServiceBroker::GetAppComponents();
          const auto appPlayer = components.GetComponent<CApplicationPlayer>();
          int64_t totalMs = appPlayer->GetTotalTime();
          if (totalMs > 0)
          {
            int posMs = static_cast<int>(progress / 100.0f * static_cast<float>(totalMs));
            CLog::Log(LOGINFO, "TraktScrobbler: Trakt resume for {} S{}E{} - {:.1f}% = {} ms",
                      m_imdbId, m_season, m_episode, progress, posMs);
            return posMs;
          }
        }
      }
    }
  }

  return 0;
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
