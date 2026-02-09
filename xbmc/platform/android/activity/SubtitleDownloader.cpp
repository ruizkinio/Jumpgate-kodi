/*
 *  Copyright (C) 2024 Team ModiKodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SubtitleDownloader.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIKeyboardFactory.h"
#include "interfaces/AnnouncementManager.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <mutex>

using namespace ANNOUNCEMENT;

SubtitleDownloader::SubtitleDownloader() = default;

SubtitleDownloader::~SubtitleDownloader()
{
  Deinitialize();
}

void SubtitleDownloader::Initialize()
{
  std::unique_lock lock(m_critSection);
  if (m_initialized)
    return;

  LoadCredentials();
  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(this, ANNOUNCEMENT::Player);
  m_initialized = true;
  CLog::Log(LOGINFO, "SubtitleDownloader: Initialized");
}

void SubtitleDownloader::Deinitialize()
{
  std::unique_lock lock(m_critSection);
  if (!m_initialized)
    return;

  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);
  m_initialized = false;
  CLog::Log(LOGINFO, "SubtitleDownloader: Deinitialized");
}

void SubtitleDownloader::Announce(AnnouncementFlag flag,
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
    if (m_subtitleLoaded)
      return;

    if (m_imdbId.empty() && m_title.empty())
    {
      CLog::Log(LOGDEBUG, "SubtitleDownloader: No content info, skipping subtitle search");
      return;
    }

    if (m_apiKey.empty())
    {
      if (!PromptForCredentials())
      {
        CLog::Log(LOGINFO, "SubtitleDownloader: User cancelled credential input");
        return;
      }
    }

    if (!IsAuthenticated())
    {
      if (!Login())
      {
        CLog::Log(LOGERROR, "SubtitleDownloader: Login failed");
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error,
            "Subtitles", "OpenSubtitles login failed", 3000, true);
        return;
      }
    }

    SearchAndDownload();
  }
  else if (message == "OnStop")
  {
    m_subtitleLoaded = false;
  }
}

void SubtitleDownloader::SetContentInfo(const std::string& imdbId,
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

  CLog::Log(LOGINFO, "SubtitleDownloader: Content info set - imdb={}, title={}, S{}E{}",
            imdbId, title, season, episode);
}

void SubtitleDownloader::ClearContentInfo()
{
  std::unique_lock lock(m_critSection);
  m_imdbId.clear();
  m_title.clear();
  m_year = 0;
  m_season = -1;
  m_episode = -1;
  m_subtitleLoaded = false;
}

void SubtitleDownloader::SetLanguages(const std::string& languages)
{
  std::unique_lock lock(m_critSection);
  if (!languages.empty())
    m_languages = languages;
  CLog::Log(LOGINFO, "SubtitleDownloader: Languages set to: {}", m_languages);
}

std::string SubtitleDownloader::GetLanguages() const
{
  std::unique_lock lock(m_critSection);
  return m_languages;
}

// --- Credentials ---

bool SubtitleDownloader::LoadCredentials()
{
  std::string path = CSpecialProtocol::TranslatePath(CREDENTIALS_FILE);

  XFILE::CFile file;
  std::vector<uint8_t> buffer;
  ssize_t bytesRead = file.LoadFile(path, buffer);
  if (bytesRead <= 0)
  {
    CLog::Log(LOGDEBUG, "SubtitleDownloader: No credentials file found");
    return false;
  }

  std::string json(buffer.begin(), buffer.end());
  CVariant data;
  if (!CJSONVariantParser::Parse(json, data))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to parse credentials file");
    return false;
  }

  m_apiKey = data["api_key"].asString();
  m_username = data["username"].asString();
  m_password = data["password"].asString();
  std::string lang = data["language"].asString();
  if (!lang.empty())
    m_languages = lang;

  CLog::Log(LOGINFO, "SubtitleDownloader: Credentials loaded (user={})", m_username);
  return !m_apiKey.empty();
}

bool SubtitleDownloader::SaveCredentials()
{
  CVariant data(CVariant::VariantTypeObject);
  data["api_key"] = m_apiKey;
  data["username"] = m_username;
  data["password"] = m_password;
  data["language"] = m_languages;

  std::string json;
  if (!CJSONVariantWriter::Write(data, json, true))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to serialize credentials");
    return false;
  }

  std::string path = CSpecialProtocol::TranslatePath(CREDENTIALS_FILE);

  XFILE::CFile file;
  if (!file.OpenForWrite(path, true))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to open credentials file for writing");
    return false;
  }

  ssize_t written = file.Write(json.c_str(), json.size());
  file.Close();

  if (written != static_cast<ssize_t>(json.size()))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to write credentials");
    return false;
  }

  CLog::Log(LOGINFO, "SubtitleDownloader: Credentials saved");
  return true;
}

bool SubtitleDownloader::PromptForCredentials()
{
  std::string apiKey;
  if (!CGUIKeyboardFactory::ShowAndGetInput(apiKey, CVariant{"OpenSubtitles API Key"}, false, true))
    return false;

  std::string username;
  if (!CGUIKeyboardFactory::ShowAndGetInput(username, CVariant{"OpenSubtitles Username"}, false))
    return false;

  std::string password;
  if (!CGUIKeyboardFactory::ShowAndGetInput(password, CVariant{"OpenSubtitles Password"}, false, true))
    return false;

  std::string lang = m_languages;
  if (CGUIKeyboardFactory::ShowAndGetInput(lang,
        CVariant{"Subtitle Languages (comma-separated, default: en)"}, true))
  {
    if (!lang.empty())
      m_languages = lang;
  }

  m_apiKey = apiKey;
  m_username = username;
  m_password = password;

  SaveCredentials();
  return true;
}

// --- Auth ---

bool SubtitleDownloader::IsAuthenticated() const
{
  return !m_jwtToken.empty();
}

bool SubtitleDownloader::Login()
{
  if (m_apiKey.empty() || m_username.empty() || m_password.empty())
    return false;

  CVariant body(CVariant::VariantTypeObject);
  body["username"] = m_username;
  body["password"] = m_password;

  std::string jsonBody;
  CJSONVariantWriter::Write(body, jsonBody, true);

  std::string response;
  if (!OSPost("/login", jsonBody, response))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Login request failed");
    return false;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to parse login response");
    return false;
  }

  m_jwtToken = result["token"].asString();

  if (m_jwtToken.empty())
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: No token in login response");
    return false;
  }

  CLog::Log(LOGINFO, "SubtitleDownloader: Logged in successfully");
  return true;
}

// --- Search & Download ---

bool SubtitleDownloader::SearchAndDownload()
{
  // Parse languages list
  std::vector<std::string> langs;
  std::string langStr = m_languages;
  size_t pos = 0;
  while ((pos = langStr.find(',')) != std::string::npos)
  {
    std::string lang = langStr.substr(0, pos);
    StringUtils::Trim(lang);
    if (!lang.empty())
      langs.push_back(lang);
    langStr = langStr.substr(pos + 1);
  }
  StringUtils::Trim(langStr);
  if (!langStr.empty())
    langs.push_back(langStr);
  if (langs.empty())
    langs.push_back("en");

  // If multiple languages configured, download all
  if (langs.size() > 1)
    return SearchAndDownloadAllLanguages();

  // Single language path
  int fileId = 0;
  std::string fileName;
  if (!SearchSubtitles(langs[0], fileId, fileName))
  {
    CLog::Log(LOGWARNING, "SubtitleDownloader: No subtitles found for '{}'", langs[0]);
    return false;
  }

  if (!DownloadSubtitle(fileId, fileName, langs[0]))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to download subtitle");
    return false;
  }

  return true;
}

bool SubtitleDownloader::SearchAndDownloadAllLanguages()
{
  // Parse languages list
  std::vector<std::string> langs;
  std::string langStr = m_languages;
  size_t pos = 0;
  while ((pos = langStr.find(',')) != std::string::npos)
  {
    std::string lang = langStr.substr(0, pos);
    StringUtils::Trim(lang);
    if (!lang.empty())
      langs.push_back(lang);
    langStr = langStr.substr(pos + 1);
  }
  StringUtils::Trim(langStr);
  if (!langStr.empty())
    langs.push_back(langStr);

  int loaded = 0;
  for (const auto& lang : langs)
  {
    int fileId = 0;
    std::string fileName;

    if (!SearchSubtitles(lang, fileId, fileName))
    {
      CLog::Log(LOGWARNING, "SubtitleDownloader: No subtitles found for '{}'", lang);
      continue;
    }

    if (DownloadSubtitle(fileId, fileName, lang))
      loaded++;
  }

  if (loaded > 0)
  {
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
        "Subtitles", "Loaded " + std::to_string(loaded) + " subtitle track(s)", 3000, true);
  }

  return loaded > 0;
}

bool SubtitleDownloader::SearchSubtitles(const std::string& language, int& outFileId, std::string& outFileName)
{
  std::string endpoint = "/subtitles?languages=" + language;

  if (!m_imdbId.empty())
  {
    std::string imdbNum = m_imdbId;
    if (StringUtils::StartsWith(imdbNum, "tt"))
      imdbNum = imdbNum.substr(2);
    endpoint += "&imdb_id=" + imdbNum;
  }
  else if (!m_title.empty())
  {
    std::string encodedTitle = m_title;
    StringUtils::Replace(encodedTitle, " ", "+");
    endpoint += "&query=" + encodedTitle;
  }
  else
  {
    return false;
  }

  if (m_season >= 0 && m_episode >= 0)
  {
    endpoint += "&season_number=" + std::to_string(m_season);
    endpoint += "&episode_number=" + std::to_string(m_episode);
  }

  CLog::Log(LOGINFO, "SubtitleDownloader: Searching subtitles [{}]: {}", language, endpoint);

  std::string response;
  if (!OSGet(endpoint, response))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Search request failed for '{}'", language);
    return false;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
    return false;

  const CVariant& data = result["data"];
  if (!data.isArray() || data.size() == 0)
  {
    CLog::Log(LOGWARNING, "SubtitleDownloader: No subtitle results for '{}'", language);
    return false;
  }

  const CVariant& first = data[0];
  const CVariant& attributes = first["attributes"];
  const CVariant& files = attributes["files"];

  if (!files.isArray() || files.size() == 0)
    return false;

  outFileId = files[0]["file_id"].asInteger(0);
  outFileName = files[0]["file_name"].asString();

  if (outFileId == 0)
    return false;

  CLog::Log(LOGINFO, "SubtitleDownloader: Found subtitle [{}]: {} (file_id={})",
            language, outFileName, outFileId);
  return true;
}

bool SubtitleDownloader::DownloadSubtitle(int fileId, const std::string& fileName, const std::string& language)
{
  CVariant body(CVariant::VariantTypeObject);
  body["file_id"] = fileId;

  std::string jsonBody;
  CJSONVariantWriter::Write(body, jsonBody, true);

  std::string response;
  if (!OSPost("/download", jsonBody, response))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Download request failed for '{}'", language);
    return false;
  }

  CVariant result;
  if (!CJSONVariantParser::Parse(response, result))
    return false;

  std::string downloadLink = result["link"].asString();
  if (downloadLink.empty())
    return false;

  std::string subtitleContent;
  XFILE::CCurlFile curl;
  curl.SetTimeout(15);
  if (!curl.Get(downloadLink, subtitleContent))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: Failed to download subtitle file [{}]", language);
    return false;
  }

  // Name files distinctly per language: subtitle_en.srt, subtitle_es.srt
  std::string saveFileName = "subtitle_" + language + ".srt";
  std::string savePath = CSpecialProtocol::TranslatePath("special://temp/" + saveFileName);

  XFILE::CFile file;
  if (!file.OpenForWrite(savePath, true))
    return false;

  ssize_t written = file.Write(subtitleContent.c_str(), subtitleContent.size());
  file.Close();

  if (written != static_cast<ssize_t>(subtitleContent.size()))
    return false;

  // Load into player
  auto& components = CServiceBroker::GetAppComponents();
  auto appPlayer = components.GetComponent<CApplicationPlayer>();
  appPlayer->AddSubtitle(savePath);

  m_subtitleLoaded = true;

  CLog::Log(LOGINFO, "SubtitleDownloader: Subtitle loaded [{}]: {}", language, savePath);
  return true;
}

// --- HTTP Helpers ---

bool SubtitleDownloader::OSGet(const std::string& endpoint, std::string& response)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("Api-Key", m_apiKey);
  curl.SetRequestHeader("User-Agent", "ModiKodi v1.0");
  if (!m_jwtToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + m_jwtToken);
  curl.SetTimeout(10);

  std::string url = std::string(OS_API_URL) + endpoint;

  CLog::Log(LOGDEBUG, "SubtitleDownloader: GET {}", endpoint);

  if (!curl.Get(url, response))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: GET {} failed", endpoint);
    return false;
  }

  return true;
}

bool SubtitleDownloader::OSPost(const std::string& endpoint,
                                 const std::string& jsonBody,
                                 std::string& response)
{
  XFILE::CCurlFile curl;
  curl.SetRequestHeader("Content-Type", "application/json");
  curl.SetRequestHeader("Api-Key", m_apiKey);
  curl.SetRequestHeader("User-Agent", "ModiKodi v1.0");
  if (!m_jwtToken.empty())
    curl.SetRequestHeader("Authorization", "Bearer " + m_jwtToken);
  curl.SetTimeout(10);

  std::string url = std::string(OS_API_URL) + endpoint;

  CLog::Log(LOGDEBUG, "SubtitleDownloader: POST {}", endpoint);

  if (!curl.Post(url, jsonBody, response))
  {
    CLog::Log(LOGERROR, "SubtitleDownloader: POST {} failed", endpoint);
    return false;
  }

  return true;
}
