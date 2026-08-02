/*
 *  Copyright (C) 2026 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TraktScrobbler.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/JumpgateHistoryEventDispatcher.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <regex>
#include <utility>

namespace
{
constexpr const char* LEGACY_TOKEN_FILE = "special://profile/trakt.json";

int ParseHttpStatus(const std::string& protocolLine)
{
  const std::size_t separator = protocolLine.find(' ');
  if (separator == std::string::npos || separator + 4 > protocolLine.size())
    return 0;
  int status = 0;
  for (std::size_t index = separator + 1; index < separator + 4; ++index)
  {
    if (protocolLine[index] < '0' || protocolLine[index] > '9')
      return 0;
    status = status * 10 + protocolLine[index] - '0';
  }
  return status;
}

class CBridgeHistoryEventTransport final : public KODI::JUMPGATE::IJumpgatePlaybackClaimTransport
{
public:
  bool Post(const KODI::JUMPGATE::JumpgatePlaybackHttpRequest& request,
            KODI::JUMPGATE::JumpgatePlaybackHttpResponse& response) override
  {
    if (request.followRedirects || request.url.empty() || request.authorization.empty() ||
        request.contentType != "application/json")
    {
      return false;
    }

    XFILE::CCurlFile curl;
    curl.SetRequestHeader("Content-Type", request.contentType);
    curl.SetRequestHeader("Authorization", request.authorization);
    for (const auto& header : request.headers)
    {
      if (header.name.empty() || header.value.empty() ||
          header.name.find_first_of("\r\n:") != std::string::npos ||
          header.value.find_first_of("\r\n") != std::string::npos)
      {
        return false;
      }
      curl.SetRequestHeader(header.name, header.value);
    }
    curl.SetTimeout(1);
    curl.SetTotalTimeout(1);

    CURL requestUrl{request.url};
    requestUrl.SetProtocolOption("redirect-limit", "0");
    requestUrl.SetProtocolOption("failonerror", "false");
    const bool posted = curl.Post(requestUrl.Get(), request.body, response.body);
    response.statusCode = ParseHttpStatus(curl.GetProperty(XFILE::FileProperty::RESPONSE_PROTOCOL));
    return posted || response.statusCode != 0;
  }
};
} // namespace

TraktScrobbler::TraktScrobbler() = default;

TraktScrobbler::~TraktScrobbler()
{
  Deinitialize();
}

void TraktScrobbler::Initialize()
{
  std::scoped_lock lifecycleLock(m_lifecycleMutex);
  const std::string legacyTokenPath = CSpecialProtocol::TranslatePath(LEGACY_TOKEN_FILE);
  if (XFILE::CFile::Exists(legacyTokenPath))
  {
    if (XFILE::CFile::Delete(legacyTokenPath))
      CLog::Log(LOGINFO, "TraktScrobbler: Removed unsafe legacy local Trakt credentials");
    else
      CLog::Log(LOGWARNING, "TraktScrobbler: Could not remove unsafe legacy Trakt credentials");
  }

  {
    std::unique_lock lock(m_critSection);
    if (m_initialized)
      return;
    m_initialized = true;
  }
  EnsureDispatcher();
  CLog::Log(LOGINFO, "TraktScrobbler: Bridge-owned lifecycle initialized");
}

void TraktScrobbler::Deinitialize(bool drainHistory)
{
  std::scoped_lock lifecycleLock(m_lifecycleMutex);
  std::uint64_t generation = 0;
  bool initialized = false;
  {
    std::unique_lock lock(m_critSection);
    initialized = m_initialized;
    generation = m_playbackGeneration;
  }
  bool hasDispatcher = false;
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    hasDispatcher = static_cast<bool>(m_dispatcher);
  }
  if (!initialized && !hasDispatcher)
    return;

  if (generation != 0)
  {
    const auto result = StopForReplacement(false);
    if (result.status == KODI::JUMPGATE::JumpgateHistoryTerminalStatus::Rejected)
      CLog::Log(LOGWARNING, "TraktScrobbler: Terminal history event was rejected");
  }

  {
    std::unique_lock lock(m_critSection);
    m_initialized = false;
    m_playbackActive = false;
    m_playbackPaused = false;
    m_watchedTime.Reset();
    m_watchedTime.SetBackgrounded(m_backgrounded);
    m_sourceClaimAuthorized = false;
  }

  std::unique_ptr<KODI::JUMPGATE::CJumpgateHistoryEventDispatcher> dispatcher;
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    dispatcher = std::move(m_dispatcher);
  }
  if (dispatcher && !dispatcher->Stop(drainHistory))
    CLog::Log(LOGWARNING, "TraktScrobbler: Dispatcher moved to bounded shutdown registry");
}

void TraktScrobbler::ProcessSlow()
{
  bool identify = false;
  bool dispatch = false;
  {
    std::unique_lock lock(m_critSection);
    if (!m_initialized)
      return;
    const std::int64_t nowMs = GetMonotonicTimeMs();
    identify = !m_bridgeProfileBacked && m_playbackActive && !m_contentIdentified &&
               !m_identifyFailed &&
               (m_playbackStartTimeMs == 0 || nowMs - m_playbackStartTimeMs < IDENTIFY_RETRY_MS);
    if (identify && m_playbackStartTimeMs == 0)
      m_playbackStartTimeMs = nowMs;
    dispatch = m_sourceClaimResolved;
  }

  if (identify && !IdentifyContent())
  {
    std::unique_lock lock(m_critSection);
    if (m_playbackStartTimeMs != 0 &&
        GetMonotonicTimeMs() - m_playbackStartTimeMs >= IDENTIFY_RETRY_MS)
    {
      m_identifyFailed = true;
    }
  }

  if (dispatch)
  {
    const std::int64_t nowMs = GetMonotonicTimeMs();
    const KODI::JUMPGATE::JumpgateHistorySnapshot snapshot = GetPlaybackSnapshot(nowMs);
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    if (m_dispatcher)
      m_dispatcher->ProcessSlow(snapshot, nowMs, HISTORY_UPDATE_INTERVAL_MS);
  }
}

void TraktScrobbler::OnPlaybackStarted(bool resumed)
{
  std::uint64_t generation = 0;
  const std::int64_t nowMs = GetMonotonicTimeMs();
  {
    std::unique_lock lock(m_critSection);
    if (!m_initialized)
      return;
    m_playbackActive = true;
    m_playbackPaused = false;
    m_watchedTime.SetPlaying(true);
    m_watchedTime.SetPaused(false);
    m_watchedTime.SetBackgrounded(m_backgrounded);
    generation = m_playbackGeneration;
    if (!m_bridgeProfileBacked && !m_contentIdentified && m_playbackStartTimeMs == 0)
      m_playbackStartTimeMs = nowMs;
  }
  if (generation == 0)
    return;

  std::lock_guard<std::mutex> lock(m_dispatcherMutex);
  if (m_dispatcher)
    m_dispatcher->PlaybackStarted(resumed, GetPlaybackSnapshot(nowMs), nowMs);
}

void TraktScrobbler::OnPlaybackPaused()
{
  const std::int64_t nowMs = GetMonotonicTimeMs();
  {
    std::unique_lock lock(m_critSection);
    if (!m_initialized)
      return;
  }
  const KODI::JUMPGATE::JumpgateHistorySnapshot snapshot = GetPlaybackSnapshot(nowMs);
  {
    std::unique_lock lock(m_critSection);
    if (!m_initialized)
      return;
    m_playbackPaused = true;
    m_watchedTime.SetPaused(true);
  }
  std::lock_guard<std::mutex> lock(m_dispatcherMutex);
  if (m_dispatcher)
    m_dispatcher->PlaybackPaused(snapshot);
}

void TraktScrobbler::SetBackgrounded(bool backgrounded)
{
  const std::int64_t nowMs = GetMonotonicTimeMs();
  bool initialized = false;
  {
    std::unique_lock lock(m_critSection);
    initialized = m_initialized;
  }
  const KODI::JUMPGATE::JumpgateHistorySnapshot snapshot =
      initialized ? GetPlaybackSnapshot(nowMs) : KODI::JUMPGATE::JumpgateHistorySnapshot{};
  {
    std::unique_lock lock(m_critSection);
    m_backgrounded = backgrounded;
    m_watchedTime.SetBackgrounded(backgrounded);
    if (!m_initialized)
      return;
  }
  std::lock_guard<std::mutex> lock(m_dispatcherMutex);
  if (m_dispatcher)
    m_dispatcher->SetBackgrounded(backgrounded, snapshot);
}

KODI::JUMPGATE::JumpgateHistoryTerminalResult TraktScrobbler::StopForReplacement(bool completed)
{
  std::uint64_t generation = 0;
  const std::int64_t nowMs = GetMonotonicTimeMs();
  const KODI::JUMPGATE::JumpgateHistorySnapshot snapshot = GetPlaybackSnapshot(nowMs);
  {
    std::unique_lock lock(m_critSection);
    generation = m_playbackGeneration;
    m_playbackActive = false;
    m_playbackPaused = false;
    m_watchedTime.SetPlaying(false);
    m_watchedTime.SetPaused(false);
    m_sourceClaimResolved = false;
    m_sourceClaimAuthorized = false;
  }
  if (generation == 0)
    return {KODI::JUMPGATE::JumpgateHistoryTerminalStatus::NotRequired, generation};

  KODI::JUMPGATE::JumpgateHistoryTerminalResult result;
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    if (!m_dispatcher)
      return {KODI::JUMPGATE::JumpgateHistoryTerminalStatus::NotRequired, generation};
    result = m_dispatcher->FinishTerminal(generation, completed, snapshot);
  }
  return result;
}

void TraktScrobbler::SetContentInfo(
    const std::string& imdbId, const std::string& title, int year, int season, int episode)
{
  std::unique_lock lock(m_critSection);
  m_imdbId = imdbId;
  m_canonicalProvider = imdbId.empty() ? "" : "imdb";
  m_canonicalId = imdbId;
  m_canonicalMediaType = season >= 0 && episode >= 0 ? "episode" : "movie";
  m_title = title;
  m_episodeTitle.clear();
  m_logoUrl.clear();
  m_backgroundUrl.clear();
  m_year = year;
  m_season = season;
  m_episode = episode;
  m_sourceClaimResolved = false;
  m_sourceClaimAuthorized = false;
  m_contentIdentified = !m_bridgeProfileBacked && (!imdbId.empty() || !title.empty());
  m_playbackStartTimeMs = 0;
  m_identifyFailed = false;
}

void TraktScrobbler::SetPlaybackGeneration(std::uint64_t generation, std::uint64_t attemptToken)
{
  if (generation == 0 || attemptToken == 0)
    return;
  {
    std::unique_lock lock(m_critSection);
    if (!m_initialized)
      return;
  }
  EnsureDispatcher();
  bool advanced = false;
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    advanced = m_dispatcher && m_dispatcher->AdvanceGeneration(generation);
  }
  if (!advanced)
  {
    CLog::Log(LOGERROR, "TraktScrobbler: Playback generation rejected before terminal drain");
    return;
  }

  std::unique_lock lock(m_critSection);
  if (generation >= m_playbackGeneration)
  {
    m_playbackGeneration = generation;
    m_playbackAttemptToken = attemptToken;
    m_playbackActive = false;
    m_playbackPaused = false;
    m_watchedTime.Reset();
    m_watchedTime.SetBackgrounded(m_backgrounded);
    m_sourceClaimResolved = false;
    m_sourceClaimAuthorized = false;
  }
}

void TraktScrobbler::CancelPlaybackGeneration(std::uint64_t generation, std::uint64_t attemptToken)
{
  {
    std::unique_lock lock(m_critSection);
    if (generation != m_playbackGeneration || attemptToken == 0 ||
        attemptToken != m_playbackAttemptToken)
    {
      return;
    }
  }

  StopForReplacement(false);
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    if (m_dispatcher)
      m_dispatcher->CancelGeneration(generation);
  }
  std::unique_lock lock(m_critSection);
  if (generation == m_playbackGeneration && attemptToken == m_playbackAttemptToken)
  {
    m_playbackAttemptToken = 0;
    m_playbackActive = false;
    m_playbackPaused = false;
    m_watchedTime.Reset();
    m_watchedTime.SetBackgrounded(m_backgrounded);
    m_sourceClaimResolved = false;
    m_sourceClaimAuthorized = false;
  }
}

bool TraktScrobbler::SetClaimedContentInfo(std::uint64_t generation,
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
                                           const std::string& backgroundUrl,
                                           int year,
                                           int season,
                                           int episode,
                                           bool traktEligible)
{
  const std::string normalizedOrigin = NormalizeBridgeOrigin(bridgeOrigin);
  const bool grantKindValid = historyGrantKind == "canonical" || historyGrantKind == "local";
  bool informationalTraktAuthority = false;
  {
    std::unique_lock lock(m_critSection);
    const bool currentAuthority =
        m_initialized && generation != 0 && generation == m_playbackGeneration &&
        m_bridgeProfileBacked && m_bridgeCredentialsValid && profileId == m_bridgeProfileId &&
        deviceId == m_bridgeDeviceId && normalizedOrigin == m_bridgeOrigin &&
        !deviceToken.empty() && !sessionId.empty() && !historyGrant.empty() && grantKindValid &&
        sessionRevision > 0;
    if (!currentAuthority)
      return false;

    const bool mediaTypeValid =
        mediaType == "movie" || (mediaType == "episode" && season >= 0 && episode >= 0);
    informationalTraktAuthority =
        traktEligible && m_bridgeTraktEnabled && mediaTypeValid && historyGrantKind == "canonical";
  }

  KODI::JUMPGATE::JumpgateHistoryEventBinding binding;
  binding.generation = generation;
  binding.profileId = profileId;
  binding.deviceId = deviceId;
  binding.bridgeOrigin = normalizedOrigin;
  binding.deviceToken = deviceToken;
  binding.sessionId = sessionId;
  binding.historyGrant = historyGrant;
  binding.historyGrantKind = historyGrantKind;
  binding.sessionRevision = sessionRevision;

  EnsureDispatcher();
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    bound = m_dispatcher && m_dispatcher->BindClaim(std::move(binding), GetPlaybackSnapshot(),
                                                    GetMonotonicTimeMs());
  }
  if (!bound)
    return false;

  std::unique_lock lock(m_critSection);
  if (generation != m_playbackGeneration || profileId != m_bridgeProfileId ||
      deviceId != m_bridgeDeviceId || normalizedOrigin != m_bridgeOrigin)
  {
    lock.unlock();
    std::lock_guard<std::mutex> dispatcherLock(m_dispatcherMutex);
    if (m_dispatcher)
      m_dispatcher->FinishTerminal(generation, false, GetPlaybackSnapshot());
    return true;
  }
  const bool mediaTypeValid =
      mediaType == "movie" || (mediaType == "episode" && season >= 0 && episode >= 0);
  m_canonicalProvider = provider;
  m_canonicalId = id;
  m_canonicalMediaType = mediaTypeValid ? mediaType : "";
  m_imdbId = provider == "imdb" ? id : "";
  m_title = title;
  m_episodeTitle.clear();
  m_logoUrl = logoUrl;
  m_backgroundUrl = backgroundUrl;
  m_year = year;
  m_season = mediaType == "episode" ? season : -1;
  m_episode = mediaType == "episode" ? episode : -1;
  m_sourceClaimResolved = true;
  m_sourceClaimAuthorized = informationalTraktAuthority;
  m_contentIdentified = true;
  m_playbackStartTimeMs = 0;
  m_identifyFailed = false;
  if (informationalTraktAuthority)
    CLog::Log(LOGINFO, "TraktScrobbler: Canonical Bridge history claim bound");
  else
    CLog::Log(LOGINFO, "TraktScrobbler: Authenticated claim bound for local-only history");
  return true;
}

void TraktScrobbler::SetMediaUrl(const std::string& url)
{
  std::unique_lock lock(m_critSection);
  m_mediaUrl = url;
  CLog::Log(LOGDEBUG, "TraktScrobbler: Media source set");
}

void TraktScrobbler::ClearContentInfo()
{
  std::unique_lock lock(m_critSection);
  m_imdbId.clear();
  m_canonicalProvider.clear();
  m_canonicalId.clear();
  m_canonicalMediaType.clear();
  m_title.clear();
  m_episodeTitle.clear();
  m_logoUrl.clear();
  m_backgroundUrl.clear();
  m_year = 0;
  m_season = -1;
  m_episode = -1;
  m_contentIdentified = false;
  m_sourceClaimResolved = false;
  m_sourceClaimAuthorized = false;
  m_playbackActive = false;
  m_playbackPaused = false;
  m_mediaUrl.clear();
  m_resolvedUrl.clear();
  m_playbackStartTimeMs = 0;
  m_watchedTime.Reset();
  m_watchedTime.SetBackgrounded(m_backgrounded);
  m_identifyFailed = false;
}

std::string TraktScrobbler::GetImdbId() const
{
  std::unique_lock lock(m_critSection);
  return m_imdbId;
}

std::string TraktScrobbler::GetCanonicalProvider() const
{
  std::unique_lock lock(m_critSection);
  return m_canonicalProvider;
}

std::string TraktScrobbler::GetCanonicalId() const
{
  std::unique_lock lock(m_critSection);
  return m_canonicalId;
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

std::string TraktScrobbler::GetBackgroundUrl() const
{
  std::unique_lock lock(m_critSection);
  return m_backgroundUrl;
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

std::string TraktScrobbler::GetBridgeOrigin() const
{
  std::unique_lock lock(m_critSection);
  return m_bridgeOrigin;
}

void TraktScrobbler::SetBridgeProfile(const std::string& profileId,
                                      const std::string& deviceId,
                                      const std::string& bridgeOrigin,
                                      bool sourceBacked,
                                      bool credentialsValid,
                                      bool traktEnabled)
{
  const std::string normalizedOrigin = NormalizeBridgeOrigin(bridgeOrigin);
  std::unique_lock lock(m_critSection);
  const bool changed = m_bridgeProfileId != profileId || m_bridgeDeviceId != deviceId ||
                       m_bridgeOrigin != normalizedOrigin;
  m_bridgeProfileId = profileId;
  m_bridgeDeviceId = deviceId;
  m_bridgeOrigin = normalizedOrigin;
  m_bridgeProfileBacked =
      sourceBacked && !profileId.empty() && !deviceId.empty() && !normalizedOrigin.empty();
  m_bridgeCredentialsValid = m_bridgeProfileBacked && credentialsValid;
  m_bridgeTraktEnabled = m_bridgeCredentialsValid && traktEnabled;
  if (changed)
  {
    m_sourceClaimResolved = false;
    m_sourceClaimAuthorized = false;
    m_contentIdentified = false;
  }
}

void TraktScrobbler::ClearBridgeProfile()
{
  std::unique_lock lock(m_critSection);
  m_bridgeOrigin.clear();
  m_bridgeProfileId.clear();
  m_bridgeDeviceId.clear();
  m_bridgeProfileBacked = false;
  m_bridgeCredentialsValid = false;
  m_bridgeTraktEnabled = false;
  m_sourceClaimResolved = false;
  m_sourceClaimAuthorized = false;
  m_contentIdentified = false;
}

bool TraktScrobbler::IsBridgeProfileBacked() const
{
  std::unique_lock lock(m_critSection);
  return m_bridgeProfileBacked;
}

void TraktScrobbler::EnsureDispatcher()
{
  bool initialized = false;
  bool backgrounded = false;
  {
    std::unique_lock lock(m_critSection);
    initialized = m_initialized;
    backgrounded = m_backgrounded;
  }
  if (!initialized)
    return;

  std::lock_guard<std::mutex> lock(m_dispatcherMutex);
  if (!m_dispatcher)
  {
    m_dispatcher = std::make_unique<KODI::JUMPGATE::CJumpgateHistoryEventDispatcher>(
        std::make_shared<CBridgeHistoryEventTransport>());
    if (backgrounded)
      m_dispatcher->SetBackgrounded(true, {});
  }
}

bool TraktScrobbler::IdentifyContent()
{
  std::string mediaUrl;
  std::string mediaSnapshot;
  int season = -1;
  int episode = -1;
  {
    std::unique_lock lock(m_critSection);
    if (m_bridgeProfileBacked)
      return m_sourceClaimResolved;
    if (!m_imdbId.empty())
    {
      m_contentIdentified = true;
      return true;
    }
    mediaUrl = m_mediaUrl;
    mediaSnapshot = m_mediaUrl;
    season = m_season;
    episode = m_episode;
  }
  if (mediaUrl.empty())
    return false;

  std::string candidate = mediaUrl;
  std::regex imdbPattern("(tt\\d{7,})");
  std::smatch match;
  if (!std::regex_search(candidate, match, imdbPattern))
  {
    XFILE::CCurlFile curl;
    curl.SetTimeout(2);
    curl.SetTotalTimeout(3);
    if (curl.Open(CURL(mediaUrl)))
    {
      const std::string contentDisposition = curl.GetHttpHeader().GetValue("Content-Disposition");
      const std::string redirect = curl.GetRedirectURL();
      curl.Close();
      candidate = redirect.empty() ? contentDisposition : redirect;
    }
  }
  if (!std::regex_search(candidate, match, imdbPattern))
    return false;

  std::regex episodePattern("tt\\d{7,}:(\\d+):(\\d+)");
  std::smatch episodeMatch;
  if (std::regex_search(candidate, episodeMatch, episodePattern))
  {
    season = std::stoi(episodeMatch[1].str());
    episode = std::stoi(episodeMatch[2].str());
  }

  std::unique_lock lock(m_critSection);
  if (m_bridgeProfileBacked || m_mediaUrl != mediaSnapshot)
    return false;
  m_imdbId = match[1].str();
  m_canonicalProvider = "imdb";
  m_canonicalId = m_imdbId;
  m_canonicalMediaType = season >= 0 && episode >= 0 ? "episode" : "movie";
  m_season = season;
  m_episode = episode;
  m_contentIdentified = true;
  CLog::Log(LOGINFO, "TraktScrobbler: Local compatibility metadata identified");
  return true;
}

bool TraktScrobbler::IsTraktIdentityAuthorized() const
{
  return m_bridgeProfileBacked && m_bridgeCredentialsValid && m_bridgeTraktEnabled &&
         m_sourceClaimResolved && m_sourceClaimAuthorized;
}

std::string TraktScrobbler::NormalizeBridgeOrigin(const std::string& url) const
{
  std::string normalized = url;
  StringUtils::Trim(normalized);
  if (normalized.empty())
    return normalized;
  if (StringUtils::StartsWithNoCase(normalized, "stremio://"))
    normalized = "https://" + normalized.substr(10);
  while (normalized.size() > 1 && normalized.back() == '/')
    normalized.pop_back();
  const std::size_t schemeEnd = normalized.find("://");
  if (schemeEnd == std::string::npos ||
      normalized.find_first_of("/?#", schemeEnd + 3) != std::string::npos ||
      normalized.find('@', schemeEnd + 3) != std::string::npos)
    return {};
  return normalized;
}

KODI::JUMPGATE::JumpgateHistorySnapshot TraktScrobbler::GetPlaybackSnapshot(std::int64_t nowMs)
{
  const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  const std::int64_t durationMs = std::max<std::int64_t>(0, appPlayer->GetTotalTime());
  const std::int64_t positionMs = std::max<std::int64_t>(0, appPlayer->GetTime());
  const double playbackSpeed = static_cast<double>(appPlayer->GetPlaySpeed());
  if (nowMs <= 0)
    nowMs = GetMonotonicTimeMs();
  std::int64_t watchedMs = 0;
  {
    std::unique_lock lock(m_critSection);
    watchedMs = m_watchedTime.Observe(nowMs, positionMs, playbackSpeed);
  }
  if (durationMs == 0)
    return {};
  KODI::JUMPGATE::JumpgateHistorySnapshot snapshot;
  snapshot.durationMs = durationMs;
  snapshot.positionMs = std::min(positionMs, durationMs);
  snapshot.watchedMs = std::clamp<std::int64_t>(watchedMs, 0, durationMs);
  return snapshot;
}

std::int64_t TraktScrobbler::GetMonotonicTimeMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
