/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateProfileStore.h"

#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <set>
#include <utility>

using KODI::UTILITY::CDigest;

namespace KODI::JUMPGATE
{
namespace
{
constexpr const char* DEFAULT_PAIRING_ORIGIN = "https://jumpgate-bridge.fly.dev";
constexpr size_t MAX_SETTINGS_JSON_BYTES = 64 * 1024;
constexpr size_t MAX_CONFIG_BYTES = 64 * 1024;
constexpr size_t MAX_CREDENTIAL_JSON_BYTES = 2 * 1024 * 1024;

const std::regex ORIGIN_PATTERN("^(https|http)://"
                                "(localhost|127\\.0\\.0\\.1|\\[::1\\]|[A-Za-z0-9](?:[A-Za-z0-9.-]{"
                                "0,251}[A-Za-z0-9])?)(?::([0-9]{1,5}))?$",
                                std::regex::icase);
const std::regex BRIDGE_URL_PATTERN("^(https|http)://"
                                    "(localhost|127\\.0\\.0\\.1|\\[::1\\]|[A-Za-z0-9](?:[A-Za-z0-9."
                                    "-]{0,251}[A-Za-z0-9])?)(?::([0-9]{1,5}))?(/[^?#]*)$",
                                    std::regex::icase);

void SecureClear(std::string& value)
{
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
  value.shrink_to_fit();
}

bool IsOpaqueAscii(const std::string& value, size_t minimumLength, size_t maximumLength)
{
  if (value.size() < minimumLength || value.size() > maximumLength)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char c)
                     {
                       return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
                     });
}

bool IsIdentifier(const std::string& value)
{
  return IsOpaqueAscii(value, 8, 128);
}

bool IsDeviceToken(const std::string& value)
{
  return IsOpaqueAscii(value, 32, 128);
}

bool IsConfig(const std::string& value)
{
  return IsOpaqueAscii(value, 16, MAX_CONFIG_BYTES);
}

bool IsCredentialRef(const std::string& value)
{
  constexpr std::string_view prefix{"jgcred_"};
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0 &&
         IsOpaqueAscii(value.substr(prefix.size()), 16, 128);
}

bool HasControls(const std::string& value)
{
  return std::any_of(value.begin(), value.end(),
                     [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string Trimmed(std::string value)
{
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c)
                                          { return !isSpace(static_cast<unsigned char>(c)); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](char c) { return !isSpace(static_cast<unsigned char>(c)); })
                  .base(),
              value.end());
  return value;
}

std::string LowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool SerializeJson(const CVariant& value, std::string& json, size_t maximumBytes)
{
  if (!CJSONVariantWriter::Write(value, json, true) || json.size() > maximumBytes)
    return false;
  return true;
}

bool ReadAliasedString(const CVariant& object,
                       std::initializer_list<const char*> aliases,
                       bool required,
                       std::string& value,
                       std::string& error)
{
  bool found = false;
  for (const char* alias : aliases)
  {
    if (!object.isMember(alias))
      continue;
    if (!object[alias].isString())
    {
      error = std::string(alias) + " must be a string";
      return false;
    }
    const std::string candidate = object[alias].asString();
    if (found && candidate != value)
    {
      error = "conflicting aliases in pairing response";
      return false;
    }
    value = candidate;
    found = true;
  }
  if (required && (!found || value.empty()))
  {
    error = "pairing response is missing a required field";
    return false;
  }
  return true;
}

std::string FirstString(const CVariant& object, std::initializer_list<const char*> aliases)
{
  if (!object.isObject())
    return {};
  for (const char* alias : aliases)
  {
    if (object.isMember(alias) && object[alias].isString())
      return object[alias].asString();
  }
  return {};
}

int64_t FirstInteger(const CVariant& object,
                     std::initializer_list<const char*> aliases,
                     int64_t fallback)
{
  if (!object.isObject())
    return fallback;
  for (const char* alias : aliases)
  {
    if (object.isMember(alias) && object[alias].isInteger())
      return object[alias].asInteger(fallback);
  }
  return fallback;
}

bool HasOnlyMembers(const CVariant& object, std::initializer_list<const char*> allowed)
{
  if (!object.isObject())
    return false;
  for (auto it = object.begin_map(); it != object.end_map(); ++it)
  {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char* key) { return it->first == key; }))
      return false;
  }
  return true;
}

bool ParseOrigin(const std::string& input,
                 bool allowInsecureLoopback,
                 std::string& normalized,
                 std::string* hostOut = nullptr)
{
  if (input.empty() || input.size() > 512 || input != Trimmed(input))
    return false;

  std::smatch match;
  if (!std::regex_match(input, match, ORIGIN_PATTERN))
    return false;

  std::string scheme = LowerAscii(match[1].str());
  std::string host = LowerAscii(match[2].str());
  const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
  if (scheme != "https" && !(scheme == "http" && loopback && allowInsecureLoopback))
    return false;

  if (match[3].matched)
  {
    const int port = std::atoi(match[3].str().c_str());
    if (port < 1 || port > 65535)
      return false;
  }

  normalized = scheme + "://" + host;
  if (match[3].matched)
    normalized += ":" + match[3].str();
  if (hostOut)
    *hostOut = host;
  return true;
}

bool ParseBridgeBaseUrl(const std::string& input,
                        const std::string& config,
                        bool allowInsecureLoopback,
                        std::string& normalizedUrl,
                        std::string& origin,
                        std::string& routeKind)
{
  if (input.empty() || input.size() > 65536 || input != Trimmed(input))
    return false;

  std::smatch match;
  if (!std::regex_match(input, match, BRIDGE_URL_PATTERN))
    return false;

  std::string candidateOrigin = match[1].str() + "://" + match[2].str();
  if (match[3].matched)
    candidateOrigin += ":" + match[3].str();
  if (!ParseOrigin(candidateOrigin, allowInsecureLoopback, origin))
    return false;

  const std::string path = match[4].str();
  if (path == "/_c/" + config)
    routeKind = "configured";
  else if (path == "/" + config)
    routeKind = "legacy_configured";
  else
    return false;

  normalizedUrl = origin + path;
  return true;
}

std::string OriginFromLegacyUrl(const std::string& input)
{
  std::smatch match;
  if (!std::regex_match(input, match, BRIDGE_URL_PATTERN))
    return {};
  std::string origin = match[1].str() + "://" + match[2].str();
  if (match[3].matched)
    origin += ":" + match[3].str();
  std::string normalized;
  // Legacy HTTP origins remain disabled unless they are exact loopback development origins.
  if (!ParseOrigin(origin, true, normalized))
    return {};
  return normalized;
}

std::string NormalizeLegacyUrl(std::string value)
{
  value = Trimmed(std::move(value));
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  return value;
}

std::string LegacySuffix(const std::string& value)
{
  return CDigest::Calculate(CDigest::Type::SHA256, value).substr(0, 24);
}

CVariant DefaultSettings()
{
  CVariant settings(CVariant::VariantTypeObject);
  settings["subtitle_languages"] = "en";
  settings["trakt_enabled"] = true;
  settings["subtitles_enabled"] = true;
  settings["auto_update_check"] = true;
  return settings;
}

CVariant CanonicalSettings(const CVariant& input, const CVariant& fallback)
{
  CVariant settings(CVariant::VariantTypeObject);
  const CVariant defaults = fallback.isObject() ? fallback : DefaultSettings();

  const auto readBool = [&](const char* key, const char* alias, bool defaultValue)
  {
    if (input.isObject() && input.isMember(key) && input[key].isBoolean())
      return input[key].asBoolean();
    if (input.isObject() && alias && input.isMember(alias) && input[alias].isBoolean())
      return input[alias].asBoolean();
    if (defaults.isMember(key) && defaults[key].isBoolean())
      return defaults[key].asBoolean();
    return defaultValue;
  };
  const auto readString = [&](const char* key, const char* alias, const char* defaultValue)
  {
    if (input.isObject() && input.isMember(key) && input[key].isString() &&
        input[key].asString().size() <= 256 && !HasControls(input[key].asString()))
      return input[key].asString();
    if (input.isObject() && alias && input.isMember(alias) && input[alias].isString() &&
        input[alias].asString().size() <= 256 && !HasControls(input[alias].asString()))
      return input[alias].asString();
    if (defaults.isMember(key) && defaults[key].isString() &&
        defaults[key].asString().size() <= 256 && !HasControls(defaults[key].asString()))
      return defaults[key].asString();
    return std::string{defaultValue};
  };

  settings["subtitle_languages"] = readString("subtitle_languages", "subtitleLanguages", "en");
  settings["trakt_enabled"] = readBool("trakt_enabled", "traktEnabled", true);
  settings["subtitles_enabled"] = readBool("subtitles_enabled", "subtitlesEnabled", true);
  settings["auto_update_check"] = readBool("auto_update_check", "autoUpdateCheck", true);
  return settings;
}

CVariant ReadLegacyDefaults(const CVariant& root)
{
  CVariant input = root.isMember("defaults") && root["defaults"].isObject()
                       ? root["defaults"]
                       : CVariant(CVariant::VariantTypeObject);
  const std::array<const char*, 4> known = {"subtitle_languages", "trakt_enabled",
                                            "subtitles_enabled", "auto_update_check"};
  for (const char* key : known)
  {
    if (root.isMember(key))
      input[key] = root[key];
  }
  return CanonicalSettings(input, DefaultSettings());
}

bool NormalizePairingSettings(const CVariant& input, CVariant& settings, std::string& error)
{
  if (!input.isObject())
  {
    error = "pairing settings must be an object";
    return false;
  }
  settings = DefaultSettings();

  struct BoolAlias
  {
    const char* canonical;
    const char* alias;
  };
  const std::array<BoolAlias, 3> booleans = {{{"trakt_enabled", "traktEnabled"},
                                              {"subtitles_enabled", "subtitlesEnabled"},
                                              {"auto_update_check", "autoUpdateCheck"}}};
  for (const auto& entry : booleans)
  {
    bool found = false;
    bool value = false;
    for (const char* key : {entry.canonical, entry.alias})
    {
      if (!input.isMember(key))
        continue;
      if (!input[key].isBoolean())
      {
        error = std::string(key) + " must be a boolean";
        return false;
      }
      if (found && value != input[key].asBoolean())
      {
        error = "conflicting settings aliases";
        return false;
      }
      found = true;
      value = input[key].asBoolean();
    }
    if (found)
      settings[entry.canonical] = value;
  }

  std::string languages;
  bool languagesFound = false;
  for (const char* key : {"subtitle_languages", "subtitleLanguages"})
  {
    if (!input.isMember(key))
      continue;
    if (!input[key].isString())
    {
      error = std::string(key) + " must be a string";
      return false;
    }
    const std::string candidate = input[key].asString();
    if (languagesFound && candidate != languages)
    {
      error = "conflicting settings aliases";
      return false;
    }
    languagesFound = true;
    languages = candidate;
  }
  if (languages.size() > 256 || HasControls(languages))
  {
    error = "subtitle_languages is invalid";
    return false;
  }
  if (languagesFound)
    settings["subtitle_languages"] = languages;

  std::string json;
  if (!SerializeJson(settings, json, MAX_SETTINGS_JSON_BYTES))
  {
    error = "pairing settings exceed the supported size";
    return false;
  }
  return true;
}

bool ReadSettingBool(const CVariant& settings,
                     std::initializer_list<const char*> aliases,
                     bool fallback)
{
  for (const char* key : aliases)
  {
    if (settings.isObject() && settings.isMember(key) && settings[key].isBoolean())
      return settings[key].asBoolean();
  }
  return fallback;
}

std::string ReadSettingString(const CVariant& settings,
                              std::initializer_list<const char*> aliases,
                              const std::string& fallback)
{
  for (const char* key : aliases)
  {
    if (settings.isObject() && settings.isMember(key) && settings[key].isString())
      return settings[key].asString();
  }
  return fallback;
}

void ApplyPreferences(const CVariant& settings, ActiveProfile& active)
{
  active.settings = settings.isObject() ? settings : DefaultSettings();
  active.traktEnabled = ReadSettingBool(active.settings, {"trakt_enabled", "traktEnabled"}, true);
  active.subtitlesEnabled =
      ReadSettingBool(active.settings, {"subtitles_enabled", "subtitlesEnabled"}, true);
  active.autoUpdateCheck =
      ReadSettingBool(active.settings, {"auto_update_check", "autoUpdateCheck"}, true);
  active.subtitleLanguages =
      ReadSettingString(active.settings, {"subtitle_languages", "subtitleLanguages"}, "en");
  if (active.subtitleLanguages.empty())
    active.subtitleLanguages = "en";
}

StoredProfile MakeLegacyProfile(const CVariant& raw,
                                const std::string& bridgeUrl,
                                const CVariant& defaults,
                                int fallbackIndex)
{
  StoredProfile profile;
  profile.legacyBridgeBaseUrl = NormalizeLegacyUrl(bridgeUrl);
  const std::string suffix = LegacySuffix(profile.legacyBridgeBaseUrl);
  profile.profileId = "legacy_profile_" + suffix;
  profile.deviceId = "legacy_device_" + suffix;
  profile.state = "legacy_unpaired";
  profile.bridgeOrigin = OriginFromLegacyUrl(profile.legacyBridgeBaseUrl);
  profile.bridgeRouteKind = "legacy_url";
  profile.name = FirstString(raw, {"name", "profile_name", "profileName"});
  if (profile.name.empty())
    profile.name = "Legacy Profile " + std::to_string(fallbackIndex);
  profile.settings = CanonicalSettings(raw.isObject() && raw.isMember("settings")
                                           ? raw["settings"]
                                           : CVariant(CVariant::VariantTypeObject),
                                       defaults);
  profile.addedAt = FirstInteger(raw, {"addedAt", "added_at"}, 0);
  profile.updatedAt = FirstInteger(raw, {"updatedAt", "updated_at"}, profile.addedAt);
  return profile;
}

bool ParseCredentialJson(const std::string& json,
                         const StoredProfile& profile,
                         ActiveProfile& active,
                         std::string& error)
{
  if (json.empty() || json.size() > MAX_CREDENTIAL_JSON_BYTES)
  {
    error = "stored credential envelope is invalid";
    return false;
  }
  CVariant secret;
  if (!CJSONVariantParser::Parse(json, secret) || !secret.isObject())
  {
    error = "stored credential envelope is invalid";
    return false;
  }
  if (!secret.isMember("schemaVersion") ||
      secret["schemaVersion"].asInteger() != CREDENTIAL_SCHEMA_VERSION ||
      !secret.isMember("profileId") || !secret["profileId"].isString() ||
      !secret.isMember("deviceId") || !secret["deviceId"].isString() ||
      secret["profileId"].asString() != profile.profileId ||
      secret["deviceId"].asString() != profile.deviceId)
  {
    error = "stored credential scope does not match the selected profile";
    return false;
  }

  const std::string deviceToken = FirstString(secret, {"deviceToken"});
  const std::string bridgeBaseUrl = FirstString(secret, {"bridgeBaseUrl"});
  const std::string config = FirstString(secret, {"config"});
  std::string normalizedUrl;
  std::string origin;
  std::string routeKind;
  if (!IsDeviceToken(deviceToken) || !IsConfig(config) ||
      !ParseBridgeBaseUrl(bridgeBaseUrl, config, true, normalizedUrl, origin, routeKind) ||
      origin != profile.bridgeOrigin || routeKind != profile.bridgeRouteKind)
  {
    error = "stored credential material is invalid";
    return false;
  }

  active.bridgeBaseUrl = std::move(normalizedUrl);
  active.deviceToken = deviceToken;
  active.credentialsValid = true;
  return true;
}

bool ValidateSettingMutation(const std::string& key, const CVariant& value)
{
  if (key == "subtitle_languages")
    return value.isString() && value.asString().size() <= 256 && !HasControls(value.asString());
  if (key == "trakt_enabled" || key == "subtitles_enabled" || key == "auto_update_check")
    return value.isBoolean();
  return false;
}

bool IsProfileState(const std::string& state)
{
  return state == "paired" || state == "legacy_unpaired" || state == "revocation_pending";
}

bool IsBridgeRouteKind(const std::string& routeKind)
{
  return routeKind.empty() || routeKind == "configured" || routeKind == "legacy_configured" ||
         routeKind == "legacy_url";
}

} // namespace

void PairingRedemption::ClearSecrets()
{
  SecureClear(deviceToken);
  SecureClear(bridgeBaseUrl);
  SecureClear(config);
  capabilities = CVariant(CVariant::VariantTypeObject);
}

bool StoredProfile::IsSourceBacked() const
{
  return state == "paired" && IsIdentifier(profileId) && IsIdentifier(deviceId) &&
         IsCredentialRef(credentialRef) && !bridgeOrigin.empty();
}

void ActiveProfile::ClearSecrets()
{
  SecureClear(deviceToken);
  SecureClear(bridgeBaseUrl);
  credentialsValid = false;
  sourceBacked = false;
  traktEnabled = false;
}

bool ParsePairingPayload(const CVariant& response,
                         bool allowInsecureLoopback,
                         PairingPayload& payload,
                         std::string& error)
{
  payload.ClearSecrets();
  payload = PairingPayload{};
  error.clear();
  if (!response.isObject())
  {
    error = "pairing response must be an object";
    return false;
  }

  if (!ReadAliasedString(response, {"profileId", "profile_id"}, true, payload.profileId, error) ||
      !ReadAliasedString(response, {"deviceId", "device_id"}, true, payload.deviceId, error) ||
      !ReadAliasedString(response, {"deviceToken", "device_token"}, true, payload.deviceToken,
                         error) ||
      !ReadAliasedString(response, {"bridgeBaseUrl", "bridge_base_url", "bridgeUrl", "bridge_url"},
                         true, payload.bridgeBaseUrl, error) ||
      !ReadAliasedString(response, {"config"}, true, payload.config, error) ||
      !ReadAliasedString(response, {"name", "profileName", "profile_name"}, false, payload.name,
                         error))
  {
    payload.ClearSecrets();
    return false;
  }

  payload.name = Trimmed(payload.name);
  if (!IsIdentifier(payload.profileId) || !IsIdentifier(payload.deviceId) ||
      !IsDeviceToken(payload.deviceToken) || !IsConfig(payload.config) ||
      payload.name.size() > 128 || HasControls(payload.name))
  {
    error = "pairing response contains invalid identity material";
    payload.ClearSecrets();
    return false;
  }

  std::string normalizedUrl;
  if (!ParseBridgeBaseUrl(payload.bridgeBaseUrl, payload.config, allowInsecureLoopback,
                          normalizedUrl, payload.bridgeOrigin, payload.bridgeRouteKind))
  {
    error = "pairing Bridge URL is invalid or does not match config";
    payload.ClearSecrets();
    return false;
  }
  payload.bridgeBaseUrl = std::move(normalizedUrl);

  if (!response.isMember("settings") ||
      !NormalizePairingSettings(response["settings"], payload.settings, error))
  {
    payload.ClearSecrets();
    return false;
  }
  if (response.isMember("capabilities"))
  {
    if (!response["capabilities"].isObject())
    {
      error = "pairing capabilities must be an object";
      payload.ClearSecrets();
      return false;
    }
    payload.capabilities = response["capabilities"];
  }
  return true;
}

bool ParseProfileDocument(const CVariant& root,
                          ProfileDocument& document,
                          bool& requiresRewrite,
                          std::string& error)
{
  document = ProfileDocument{};
  requiresRewrite = false;
  error.clear();
  if (!root.isObject())
  {
    error = "profile document must be an object";
    return false;
  }

  document.defaults = ReadLegacyDefaults(root);
  const CVariant inputDefaults =
      root.isMember("defaults") ? root["defaults"] : CVariant(CVariant::VariantTypeObject);
  if (!inputDefaults.isObject() || inputDefaults != document.defaults ||
      !HasOnlyMembers(inputDefaults, {"subtitle_languages", "trakt_enabled", "subtitles_enabled",
                                      "auto_update_check"}))
    requiresRewrite = true;
  document.pairingOrigin = FirstString(root, {"pairingOrigin", "pairing_origin"});
  if (document.pairingOrigin.empty())
    document.pairingOrigin = DEFAULT_PAIRING_ORIGIN;
  std::string normalizedPairingOrigin;
  if (!ParseOrigin(document.pairingOrigin, true, normalizedPairingOrigin))
  {
    document.pairingOrigin = DEFAULT_PAIRING_ORIGIN;
    requiresRewrite = true;
  }
  else
  {
    if (document.pairingOrigin != normalizedPairingOrigin)
      requiresRewrite = true;
    document.pairingOrigin = normalizedPairingOrigin;
  }

  document.activeProfileId = FirstString(root, {"activeProfileId", "active_profile_id"});
  const std::string legacyActiveUrl =
      NormalizeLegacyUrl(FirstString(root, {"bridge_url", "bridgeUrl"}));
  const std::string legacyActiveName =
      FirstString(root, {"active_profile_name", "activeProfileName"});

  if (root.isMember("profiles") && !root["profiles"].isArray())
  {
    error = "profiles must be an array";
    return false;
  }

  std::set<std::string> profileIds;
  std::set<std::string> legacyUrls;
  int legacyIndex = 0;
  if (root.isMember("profiles"))
  {
    for (auto it = root["profiles"].begin_array(); it != root["profiles"].end_array(); ++it)
    {
      if (!it->isObject())
      {
        requiresRewrite = true;
        continue;
      }

      const std::string legacyUrl = NormalizeLegacyUrl(
          FirstString(*it, {"bridgeBaseUrl", "bridge_base_url", "bridgeUrl", "bridge_url"}));
      std::string profileId = FirstString(*it, {"profileId", "profile_id"});
      if (!legacyUrl.empty() && profileId.empty())
      {
        StoredProfile profile = MakeLegacyProfile(*it, legacyUrl, document.defaults, ++legacyIndex);
        if (profileIds.insert(profile.profileId).second)
        {
          legacyUrls.insert(legacyUrl);
          document.profiles.push_back(std::move(profile));
          requiresRewrite = true;
        }
        continue;
      }

      if (!IsIdentifier(profileId) || !profileIds.insert(profileId).second)
      {
        requiresRewrite = true;
        continue;
      }

      StoredProfile profile;
      profile.profileId = std::move(profileId);
      profile.deviceId = FirstString(*it, {"deviceId", "device_id"});
      profile.credentialRef = FirstString(*it, {"credentialRef", "credential_ref"});
      profile.name = FirstString(*it, {"name", "profileName", "profile_name"});
      profile.state = FirstString(*it, {"state", "status"});
      if (profile.state.empty())
        profile.state = !profile.credentialRef.empty() ? "paired" : "legacy_unpaired";
      if (it->isMember("bridge") && (*it)["bridge"].isObject())
      {
        profile.bridgeOrigin = FirstString((*it)["bridge"], {"origin"});
        profile.bridgeRouteKind = FirstString((*it)["bridge"], {"routeKind", "route_kind"});
      }
      const CVariant inputSettings =
          it->isMember("settings") ? (*it)["settings"] : CVariant(CVariant::VariantTypeObject);
      profile.settings = CanonicalSettings(inputSettings, document.defaults);
      profile.addedAt = FirstInteger(*it, {"addedAt", "added_at"}, 0);
      profile.updatedAt = FirstInteger(*it, {"updatedAt", "updated_at"}, profile.addedAt);

      if ((!profile.deviceId.empty() && !IsIdentifier(profile.deviceId)) ||
          (!profile.credentialRef.empty() && !IsCredentialRef(profile.credentialRef)) ||
          !IsProfileState(profile.state) || !IsBridgeRouteKind(profile.bridgeRouteKind))
      {
        profileIds.erase(profile.profileId);
        requiresRewrite = true;
        continue;
      }

      if (!legacyUrl.empty())
      {
        if (profile.credentialRef.empty())
        {
          profile.legacyBridgeBaseUrl = legacyUrl;
          if (profile.deviceId.empty())
            profile.deviceId = "legacy_device_" + LegacySuffix(legacyUrl);
          if (profile.state == "paired")
            profile.state = "legacy_unpaired";
          if (profile.bridgeOrigin.empty())
            profile.bridgeOrigin = OriginFromLegacyUrl(legacyUrl);
          if (profile.bridgeRouteKind.empty())
            profile.bridgeRouteKind = "legacy_url";
          legacyUrls.insert(legacyUrl);
        }
        requiresRewrite = true;
      }

      if (!profile.bridgeOrigin.empty())
      {
        std::string normalizedOrigin;
        if (!ParseOrigin(profile.bridgeOrigin, true, normalizedOrigin))
        {
          profileIds.erase(profile.profileId);
          requiresRewrite = true;
          continue;
        }
        if (profile.bridgeOrigin != normalizedOrigin)
          requiresRewrite = true;
        profile.bridgeOrigin = std::move(normalizedOrigin);
      }
      if (profile.state == "paired" && !profile.credentialRef.empty() &&
          (profile.deviceId.empty() || profile.bridgeOrigin.empty() ||
           (profile.bridgeRouteKind != "configured" &&
            profile.bridgeRouteKind != "legacy_configured")))
      {
        profileIds.erase(profile.profileId);
        requiresRewrite = true;
        continue;
      }
      if (!HasOnlyMembers(*it, {"schemaVersion", "profileId", "deviceId", "credentialRef", "name",
                                "state", "settings", "addedAt", "updatedAt", "bridge"}) ||
          !it->isMember("profileId") || !it->isMember("name") || !it->isMember("state") ||
          !it->isMember("settings") || !it->isMember("addedAt") || !it->isMember("updatedAt") ||
          !it->isMember("bridge") || !inputSettings.isObject() ||
          inputSettings != profile.settings ||
          !HasOnlyMembers(inputSettings, {"subtitle_languages", "trakt_enabled",
                                          "subtitles_enabled", "auto_update_check"}) ||
          (it->isMember("bridge") && (!(*it)["bridge"].isObject() ||
                                      !HasOnlyMembers((*it)["bridge"], {"origin", "routeKind"}))) ||
          !it->isMember("schemaVersion") ||
          (*it)["schemaVersion"].asInteger() != PROFILE_SCHEMA_VERSION)
        requiresRewrite = true;
      document.profiles.push_back(std::move(profile));
    }
  }

  if (!legacyActiveUrl.empty() && document.activeProfileId.empty() &&
      legacyUrls.find(legacyActiveUrl) == legacyUrls.end())
  {
    CVariant raw(CVariant::VariantTypeObject);
    if (!legacyActiveName.empty())
      raw["name"] = legacyActiveName;
    StoredProfile profile =
        MakeLegacyProfile(raw, legacyActiveUrl, document.defaults, ++legacyIndex);
    if (profileIds.insert(profile.profileId).second)
    {
      document.profiles.push_back(profile);
      legacyUrls.insert(legacyActiveUrl);
    }
    requiresRewrite = true;
  }

  if (document.activeProfileId.empty() && !legacyActiveUrl.empty())
  {
    document.activeProfileId = "legacy_profile_" + LegacySuffix(legacyActiveUrl);
    requiresRewrite = true;
  }
  if (!document.activeProfileId.empty() && !IsIdentifier(document.activeProfileId))
  {
    document.activeProfileId.clear();
    requiresRewrite = true;
  }
  if (!document.activeProfileId.empty() &&
      profileIds.find(document.activeProfileId) == profileIds.end())
  {
    document.activeProfileId.clear();
    requiresRewrite = true;
  }

  if (!root.isMember("schemaVersion") ||
      root["schemaVersion"].asInteger() != PROFILE_SCHEMA_VERSION ||
      !root.isMember("activeProfileId") || !root.isMember("pairingOrigin") ||
      !root.isMember("defaults") || !root.isMember("profiles") || root.isMember("bridge_url") ||
      root.isMember("bridgeUrl") || root.isMember("active_profile_name") ||
      root.isMember("activeProfileName") || root.isMember("active_profile_id") ||
      root.isMember("pairing_origin") ||
      !HasOnlyMembers(
          root, {"schemaVersion", "activeProfileId", "pairingOrigin", "defaults", "profiles"}))
  {
    requiresRewrite = true;
  }
  return true;
}

bool SerializeProfileDocument(const ProfileDocument& document, CVariant& root, std::string& error)
{
  error.clear();
  std::string normalizedPairingOrigin;
  const std::string pairingOrigin =
      document.pairingOrigin.empty() ? DEFAULT_PAIRING_ORIGIN : document.pairingOrigin;
  if (!ParseOrigin(pairingOrigin, true, normalizedPairingOrigin) ||
      pairingOrigin != normalizedPairingOrigin ||
      (!document.activeProfileId.empty() && !IsIdentifier(document.activeProfileId)))
  {
    error = "profile document contains invalid root metadata";
    return false;
  }
  root = CVariant(CVariant::VariantTypeObject);
  root["schemaVersion"] = PROFILE_SCHEMA_VERSION;
  root["activeProfileId"] = document.activeProfileId;
  root["pairingOrigin"] = normalizedPairingOrigin;
  root["defaults"] = CanonicalSettings(document.defaults, DefaultSettings());

  CVariant profiles(CVariant::VariantTypeArray);
  for (const StoredProfile& profile : document.profiles)
  {
    if (!profile.legacyBridgeBaseUrl.empty() || !IsIdentifier(profile.profileId) ||
        (!profile.deviceId.empty() && !IsIdentifier(profile.deviceId)) ||
        (!profile.credentialRef.empty() && !IsCredentialRef(profile.credentialRef)) ||
        !IsProfileState(profile.state) || !IsBridgeRouteKind(profile.bridgeRouteKind) ||
        profile.name.size() > 128 || HasControls(profile.name))
    {
      error = "profile document contains uncommitted or invalid credential metadata";
      return false;
    }
    std::string normalizedBridgeOrigin;
    if ((!profile.bridgeOrigin.empty() &&
         (!ParseOrigin(profile.bridgeOrigin, true, normalizedBridgeOrigin) ||
          profile.bridgeOrigin != normalizedBridgeOrigin)) ||
        (profile.state == "paired" && !profile.credentialRef.empty() &&
         (profile.deviceId.empty() || profile.bridgeOrigin.empty() ||
          (profile.bridgeRouteKind != "configured" &&
           profile.bridgeRouteKind != "legacy_configured"))))
    {
      error = "profile document contains invalid bridge metadata";
      return false;
    }

    CVariant item(CVariant::VariantTypeObject);
    item["schemaVersion"] = PROFILE_SCHEMA_VERSION;
    item["profileId"] = profile.profileId;
    if (!profile.deviceId.empty())
      item["deviceId"] = profile.deviceId;
    else
      item.erase("deviceId");
    if (!profile.credentialRef.empty())
      item["credentialRef"] = profile.credentialRef;
    else
      item.erase("credentialRef");
    item["name"] = profile.name;
    item["state"] = profile.state;
    item["settings"] = CanonicalSettings(profile.settings, document.defaults);
    item["addedAt"] = profile.addedAt;
    item["updatedAt"] = profile.updatedAt;

    CVariant bridge(CVariant::VariantTypeObject);
    bridge["origin"] = profile.bridgeOrigin;
    bridge["routeKind"] = profile.bridgeRouteKind;
    item["bridge"] = bridge;
    profiles.push_back(std::move(item));
  }
  root["profiles"] = std::move(profiles);
  return true;
}

CJumpgateProfileStore::CJumpgateProfileStore(IJumpgateProfileStorage& storage,
                                             IJumpgateCredentialStore& credentials)
  : m_storage(storage),
    m_credentials(credentials)
{
}

bool CJumpgateProfileStore::Load(ProfileDocument& document, std::string& error)
{
  std::string contents;
  bool exists = false;
  if (!m_storage.Read(contents, exists, error))
    return false;

  CVariant root(CVariant::VariantTypeObject);
  const bool newDocument = !exists;
  if (exists)
  {
    if (contents.empty() || !CJSONVariantParser::Parse(contents, root))
    {
      error = "Jumpgate profile metadata is not valid JSON";
      return false;
    }
  }
  else
  {
    root["schemaVersion"] = PROFILE_SCHEMA_VERSION;
    root["activeProfileId"] = "";
    root["pairingOrigin"] = DEFAULT_PAIRING_ORIGIN;
    root["defaults"] = DefaultSettings();
    root["profiles"] = CVariant(CVariant::VariantTypeArray);
  }

  bool requiresRewrite = false;
  if (!ParseProfileDocument(root, document, requiresRewrite, error))
    return false;

  std::vector<std::string> createdRefs;
  if (!MigrateLegacyCredentials(document, createdRefs, error))
  {
    for (const std::string& ref : createdRefs)
    {
      std::string ignored;
      m_credentials.Remove(ref, ignored);
    }
    return false;
  }

  if (newDocument || requiresRewrite || !createdRefs.empty())
  {
    CVariant serialized;
    std::string json;
    if (!SerializeProfileDocument(document, serialized, error) ||
        !CJSONVariantWriter::Write(serialized, json, true) || !m_storage.WriteAtomic(json, error))
    {
      for (const std::string& ref : createdRefs)
      {
        std::string ignored;
        m_credentials.Remove(ref, ignored);
      }
      return false;
    }
  }
  return true;
}

bool CJumpgateProfileStore::MigrateLegacyCredentials(ProfileDocument& document,
                                                     std::vector<std::string>& createdRefs,
                                                     std::string& error)
{
  for (StoredProfile& profile : document.profiles)
  {
    if (profile.legacyBridgeBaseUrl.empty())
      continue;
    if (!profile.credentialRef.empty())
    {
      SecureClear(profile.legacyBridgeBaseUrl);
      continue;
    }
    if (profile.deviceId.empty())
      profile.deviceId = "legacy_device_" + LegacySuffix(profile.legacyBridgeBaseUrl);

    CVariant secret(CVariant::VariantTypeObject);
    secret["schemaVersion"] = CREDENTIAL_SCHEMA_VERSION;
    secret["profileId"] = profile.profileId;
    secret["deviceId"] = profile.deviceId;
    secret["legacy"] = true;
    secret["bridgeBaseUrl"] = profile.legacyBridgeBaseUrl;
    std::string secretJson;
    if (!SerializeJson(secret, secretJson, MAX_CREDENTIAL_JSON_BYTES))
    {
      error = "legacy credential migration serialization failed";
      return false;
    }

    std::string credentialRef;
    if (!m_credentials.Store(profile.profileId, profile.deviceId, secretJson, credentialRef,
                             error) ||
        !IsCredentialRef(credentialRef))
    {
      SecureClear(secretJson);
      if (error.empty())
        error = "legacy credential migration failed";
      return false;
    }
    SecureClear(secretJson);
    createdRefs.push_back(credentialRef);
    profile.credentialRef = std::move(credentialRef);
    SecureClear(profile.legacyBridgeBaseUrl);
  }
  return true;
}

bool CJumpgateProfileStore::StorePairing(ProfileDocument& document,
                                         PairingPayload& payload,
                                         int64_t now,
                                         std::string& error)
{
  CVariant secret(CVariant::VariantTypeObject);
  secret["schemaVersion"] = CREDENTIAL_SCHEMA_VERSION;
  secret["profileId"] = payload.profileId;
  secret["deviceId"] = payload.deviceId;
  secret["deviceToken"] = payload.deviceToken;
  secret["bridgeBaseUrl"] = payload.bridgeBaseUrl;
  secret["config"] = payload.config;
  if (payload.capabilities.isObject() && !payload.capabilities.empty())
    secret["capabilities"] = payload.capabilities;

  std::string secretJson;
  if (!SerializeJson(secret, secretJson, MAX_CREDENTIAL_JSON_BYTES))
  {
    error = "credential serialization failed";
    payload.ClearSecrets();
    return false;
  }

  std::string newRef;
  if (!m_credentials.Store(payload.profileId, payload.deviceId, secretJson, newRef, error) ||
      !IsCredentialRef(newRef))
  {
    SecureClear(secretJson);
    payload.ClearSecrets();
    if (error.empty())
      error = "secure credential storage failed";
    return false;
  }
  SecureClear(secretJson);

  ProfileDocument candidate = document;
  for (const StoredProfile& profile : candidate.profiles)
  {
    if (profile.deviceId == payload.deviceId && profile.profileId != payload.profileId)
    {
      std::string ignored;
      m_credentials.Remove(newRef, ignored);
      payload.ClearSecrets();
      error = "device identity is already bound to another local profile";
      return false;
    }
  }

  StoredProfile* profile = FindProfile(candidate, payload.profileId);
  std::string oldRef;
  if (!profile)
  {
    StoredProfile created;
    created.profileId = payload.profileId;
    created.addedAt = now;
    candidate.profiles.push_back(std::move(created));
    profile = &candidate.profiles.back();
  }
  else
  {
    oldRef = profile->credentialRef;
  }

  profile->deviceId = payload.deviceId;
  profile->credentialRef = newRef;
  profile->name = payload.name.empty() ? "Jumpgate Profile" : payload.name;
  profile->state = "paired";
  profile->bridgeOrigin = payload.bridgeOrigin;
  profile->bridgeRouteKind = payload.bridgeRouteKind;
  profile->settings = payload.settings;
  profile->updatedAt = now;
  if (profile->addedAt <= 0)
    profile->addedAt = now;
  profile->legacyBridgeBaseUrl.clear();
  candidate.activeProfileId = payload.profileId;

  payload.ClearSecrets();
  if (!SaveCandidate(document, candidate, error))
  {
    std::string ignored;
    m_credentials.Remove(newRef, ignored);
    return false;
  }

  // WriteAtomic may commit the visible rename but report that directory fsync
  // was not confirmed. Retain both credential generations in that case: after
  // a power loss either metadata generation will still have a usable envelope.
  if (error.empty() && !oldRef.empty() && oldRef != newRef)
  {
    std::string cleanupError;
    if (!m_credentials.Remove(oldRef, cleanupError))
      error = "profile paired; encrypted previous credential cleanup is pending";
  }
  return true;
}

bool CJumpgateProfileStore::LoadActive(const ProfileDocument& document,
                                       ActiveProfile& active,
                                       std::string& error)
{
  active.ClearSecrets();
  active = ActiveProfile{};
  error.clear();
  if (document.activeProfileId.empty())
  {
    ApplyPreferences(document.defaults, active);
    active.traktEnabled = false;
    return true;
  }

  const StoredProfile* profile = FindProfile(document, document.activeProfileId);
  if (!profile)
  {
    error = "selected Jumpgate profile is unavailable";
    ApplyPreferences(document.defaults, active);
    active.traktEnabled = false;
    return false;
  }

  active.selected = true;
  active.profileId = profile->profileId;
  active.deviceId = profile->deviceId;
  active.name = profile->name;
  active.state = profile->state;
  active.bridgeOrigin = profile->bridgeOrigin;
  ApplyPreferences(profile->settings, active);
  active.sourceBacked = profile->IsSourceBacked();
  if (!active.sourceBacked)
  {
    active.traktEnabled = false;
    return true;
  }

  std::string secretJson;
  if (!m_credentials.Load(profile->profileId, profile->deviceId, profile->credentialRef, secretJson,
                          error))
  {
    active.ClearSecrets();
    return false;
  }
  const bool parsed = ParseCredentialJson(secretJson, *profile, active, error);
  SecureClear(secretJson);
  if (!parsed)
  {
    active.ClearSecrets();
    return false;
  }
  return true;
}

bool CJumpgateProfileStore::SaveCandidate(ProfileDocument& document,
                                          ProfileDocument& candidate,
                                          std::string& error)
{
  CVariant serialized;
  std::string json;
  if (!SerializeProfileDocument(candidate, serialized, error) ||
      !CJSONVariantWriter::Write(serialized, json, true) || !m_storage.WriteAtomic(json, error))
    return false;
  document = std::move(candidate);
  return true;
}

bool CJumpgateProfileStore::SelectActive(ProfileDocument& document,
                                         const std::string& profileId,
                                         std::string& error)
{
  const StoredProfile* profile = FindProfile(document, profileId);
  if (!profile || profile->state == "revocation_pending")
  {
    error = "profile cannot be selected";
    return false;
  }
  ProfileDocument candidate = document;
  candidate.activeProfileId = profileId;
  return SaveCandidate(document, candidate, error);
}

bool CJumpgateProfileStore::ClearActive(ProfileDocument& document, std::string& error)
{
  ProfileDocument candidate = document;
  candidate.activeProfileId.clear();
  return SaveCandidate(document, candidate, error);
}

bool CJumpgateProfileStore::MarkRevocationPending(ProfileDocument& document,
                                                  const std::string& profileId,
                                                  const std::string& deviceId,
                                                  int64_t now,
                                                  std::string& error)
{
  ProfileDocument candidate = document;
  StoredProfile* profile = FindProfile(candidate, profileId);
  if (!profile || profile->deviceId != deviceId)
  {
    error = "exact profile/device binding was not found";
    return false;
  }
  profile->state = "revocation_pending";
  profile->updatedAt = now;
  if (candidate.activeProfileId == profileId)
    candidate.activeProfileId.clear();
  return SaveCandidate(document, candidate, error);
}

bool CJumpgateProfileStore::ForgetLocal(ProfileDocument& document,
                                        const std::string& profileId,
                                        const std::string& deviceId,
                                        std::string& error)
{
  ProfileDocument candidate = document;
  auto it = std::find_if(
      candidate.profiles.begin(), candidate.profiles.end(), [&](const StoredProfile& profile)
      { return profile.profileId == profileId && profile.deviceId == deviceId; });
  if (it == candidate.profiles.end())
  {
    error = "exact profile/device binding was not found";
    return false;
  }
  const std::string credentialRef = it->credentialRef;
  candidate.profiles.erase(it);
  if (candidate.activeProfileId == profileId)
    candidate.activeProfileId.clear();
  if (!SaveCandidate(document, candidate, error))
    return false;

  // Do not delete the credential if metadata durability was not confirmed.
  // A crash may restore the old metadata generation that still references it.
  if (error.empty() && !credentialRef.empty())
  {
    std::string cleanupError;
    if (!m_credentials.Remove(credentialRef, cleanupError))
    {
      // The orphan remains encrypted and unreachable; metadata has already committed safely.
      error = "profile forgotten locally; encrypted credential cleanup is pending";
    }
  }
  return true;
}

bool CJumpgateProfileStore::SetActiveSetting(ProfileDocument& document,
                                             const std::string& key,
                                             const CVariant& value,
                                             std::string& error)
{
  if (!ValidateSettingMutation(key, value))
  {
    error = "unsupported or invalid profile setting";
    return false;
  }
  ProfileDocument candidate = document;
  if (candidate.activeProfileId.empty())
  {
    if (!candidate.defaults.isObject())
      candidate.defaults = DefaultSettings();
    candidate.defaults[key] = value;
  }
  else
  {
    StoredProfile* profile = FindProfile(candidate, candidate.activeProfileId);
    if (!profile)
    {
      error = "selected profile is unavailable";
      return false;
    }
    if (!profile->settings.isObject())
      profile->settings = candidate.defaults;
    profile->settings[key] = value;
  }
  return SaveCandidate(document, candidate, error);
}

bool CJumpgateProfileStore::SetPairingOrigin(ProfileDocument& document,
                                             const std::string& origin,
                                             bool allowInsecureLoopback,
                                             std::string& error)
{
  std::string normalized;
  if (!ParseOrigin(origin, allowInsecureLoopback, normalized))
  {
    error = "pairing origin must be HTTPS or an explicit HTTP loopback development origin";
    return false;
  }
  ProfileDocument candidate = document;
  candidate.pairingOrigin = std::move(normalized);
  return SaveCandidate(document, candidate, error);
}

const StoredProfile* FindProfile(const ProfileDocument& document, const std::string& profileId)
{
  auto it =
      std::find_if(document.profiles.begin(), document.profiles.end(),
                   [&](const StoredProfile& profile) { return profile.profileId == profileId; });
  return it == document.profiles.end() ? nullptr : &*it;
}

StoredProfile* FindProfile(ProfileDocument& document, const std::string& profileId)
{
  auto it =
      std::find_if(document.profiles.begin(), document.profiles.end(),
                   [&](const StoredProfile& profile) { return profile.profileId == profileId; });
  return it == document.profiles.end() ? nullptr : &*it;
}

bool IsValidPairingOrigin(const std::string& origin, bool allowInsecureLoopback)
{
  std::string ignored;
  return NormalizePairingOrigin(origin, allowInsecureLoopback, ignored);
}

bool NormalizePairingOrigin(const std::string& origin,
                            bool allowInsecureLoopback,
                            std::string& normalized)
{
  normalized.clear();
  return ParseOrigin(origin, allowInsecureLoopback, normalized);
}

std::string RedactBridgeForDisplay(const std::string& origin)
{
  std::string normalized;
  return ParseOrigin(origin, true, normalized) ? normalized : std::string{"<invalid-origin>"};
}

} // namespace KODI::JUMPGATE
