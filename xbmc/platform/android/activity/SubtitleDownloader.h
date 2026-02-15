/*
 *  Copyright (C) 2024 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "interfaces/IAnnouncer.h"
#include "threads/CriticalSection.h"

#include <string>

class SubtitleDownloader : public ANNOUNCEMENT::IAnnouncer
{
public:
  SubtitleDownloader();
  ~SubtitleDownloader() override;

  // IAnnouncer
  void Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                const std::string& sender,
                const std::string& message,
                const CVariant& data) override;

  // Lifecycle
  void Initialize();
  void Deinitialize();

  // Content info (shared from XBMCApp, same as TraktScrobbler)
  void SetContentInfo(const std::string& imdbId,
                      const std::string& title,
                      int year,
                      int season,
                      int episode);
  void ClearContentInfo();

  // Late subtitle download (F-011: propagate late content identification)
  void TriggerSearch();

  // Language settings
  void SetLanguages(const std::string& languages);
  std::string GetLanguages() const;

private:
  // Auth
  bool LoadCredentials();
  bool SaveCredentials();
  bool PromptForCredentials();
  bool Login();
  bool IsAuthenticated() const;

  // Search & Download
  bool SearchAndDownload();
  bool SearchAndDownloadAllLanguages();
  bool SearchSubtitles(const std::string& language, int& outFileId, std::string& outFileName);
  bool DownloadSubtitle(int fileId, const std::string& fileName, const std::string& language);

  // HTTP helpers (lock-free: accept credentials as parameter, no member access)
  bool OSGetWithCredentials(const std::string& endpoint,
                            std::string& response,
                            const std::string& apiKey,
                            const std::string& jwtToken);
  bool OSPostWithCredentials(const std::string& endpoint,
                             const std::string& jsonBody,
                             std::string& response,
                             const std::string& apiKey,
                             const std::string& jwtToken);

  // HTTP helpers (legacy wrappers: read credentials from members)
  bool OSGet(const std::string& endpoint, std::string& response);
  bool OSPost(const std::string& endpoint, const std::string& jsonBody,
              std::string& response);

  // State
  bool m_initialized{false};
  bool m_subtitleLoaded{false};

  // Credentials (from opensubtitles.json)
  std::string m_username;
  std::string m_password;
  std::string m_apiKey;
  std::string m_languages{"en"};  // Comma-separated language codes (e.g. "en,es,fr")

  // JWT auth
  std::string m_jwtToken;

  // Content info (set from XBMCApp)
  std::string m_imdbId;
  std::string m_title;
  int m_year{0};
  int m_season{-1};
  int m_episode{-1};

  // API constants
  static constexpr const char* OS_API_URL = "https://api.opensubtitles.com/api/v1";
  static constexpr const char* CREDENTIALS_FILE = "special://profile/opensubtitles.json";

  mutable CCriticalSection m_critSection;
};
