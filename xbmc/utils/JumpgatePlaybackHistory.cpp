/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackHistory.h"

#include "Digest.h"
#include "JSONVariantParser.h"
#include "JSONVariantWriter.h"
#include "Variant.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace KODI::JUMPGATE
{
namespace
{
constexpr int HISTORY_SCHEMA_VERSION = 2;
constexpr int LEGACY_HISTORY_SCHEMA_VERSION = 1;
constexpr std::size_t MAX_PROFILE_ID_LENGTH = 256;
constexpr std::size_t MAX_CANONICAL_ID_LENGTH = 256;
constexpr std::size_t MAX_TITLE_LENGTH = 8192;
constexpr std::size_t MAX_BLOCKED_PROFILES = JUMPGATE_HISTORY_MAX_ENTRIES;
constexpr std::size_t MAX_FORGOTTEN_PROFILES = JUMPGATE_HISTORY_MAX_ENTRIES;
constexpr std::string_view LOCAL_SOURCE_FINGERPRINT_DOMAIN =
    "jumpgate-history-local-source-fingerprints-v1";
constexpr std::string_view LOCAL_SOURCE_FALLBACK_DOMAIN =
    "jumpgate-history-local-source-raw-launch-uri-v1";

void AppendUint64(std::string& output, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    output.push_back(static_cast<char>((value >> shift) & 0xff));
}

void AppendLengthPrefixed(std::string& output, std::string_view value)
{
  AppendUint64(output, value.size());
  output.append(value.data(), value.size());
}

std::string DigestLengthPrefixed(std::string_view domain,
                                 const std::vector<std::string_view>& values)
{
  std::size_t size = sizeof(uint64_t) * (values.size() + 2) + domain.size();
  for (const std::string_view value : values)
    size += value.size();

  std::string material;
  material.reserve(size);
  AppendLengthPrefixed(material, domain);
  AppendUint64(material, values.size());
  for (const std::string_view value : values)
    AppendLengthPrefixed(material, value);
  return KODI::UTILITY::CDigest::Calculate(KODI::UTILITY::CDigest::Type::SHA256, material);
}

bool ParseHistoryNamespace(const CVariant& value,
                           JumpgatePlaybackHistoryNamespace& historyNamespace)
{
  if (!value.isString())
    return false;
  if (value.asString() == "authenticated_profile")
  {
    historyNamespace = JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
    return true;
  }
  if (value.asString() == "local_source")
  {
    historyNamespace = JumpgatePlaybackHistoryNamespace::LocalSource;
    return true;
  }
  return false;
}

bool ReadUtf8CodePoint(std::string_view value, std::size_t& offset, std::uint32_t& codePoint)
{
  const auto first = static_cast<unsigned char>(value[offset]);
  std::size_t length = 0;
  std::uint32_t minimum = 0;
  if (first <= 0x7f)
  {
    codePoint = first;
    ++offset;
    return true;
  }
  if (first >= 0xc2 && first <= 0xdf)
  {
    length = 2;
    codePoint = first & 0x1f;
    minimum = 0x80;
  }
  else if (first >= 0xe0 && first <= 0xef)
  {
    length = 3;
    codePoint = first & 0x0f;
    minimum = 0x800;
  }
  else if (first >= 0xf0 && first <= 0xf4)
  {
    length = 4;
    codePoint = first & 0x07;
    minimum = 0x10000;
  }
  else
  {
    return false;
  }

  if (offset + length > value.size())
    return false;
  for (std::size_t index = 1; index < length; ++index)
  {
    const auto continuation = static_cast<unsigned char>(value[offset + index]);
    if ((continuation & 0xc0) != 0x80)
      return false;
    codePoint = (codePoint << 6) | (continuation & 0x3f);
  }
  offset += length;
  return codePoint >= minimum && codePoint <= 0x10ffff &&
         !(codePoint >= 0xd800 && codePoint <= 0xdfff);
}

bool IsBoundedText(std::string_view value,
                   std::size_t maximumUtf16Length,
                   bool allowEmpty,
                   bool requireTrimmed)
{
  if ((!allowEmpty && value.empty()) || value.size() > maximumUtf16Length * 4)
    return false;
  if (requireTrimmed && !value.empty())
  {
    const auto isEdgeWhitespace = [](unsigned char character)
    { return character <= 0x20 || character == 0x7f; };
    if (isEdgeWhitespace(static_cast<unsigned char>(value.front())) ||
        isEdgeWhitespace(static_cast<unsigned char>(value.back())))
    {
      return false;
    }
  }

  std::size_t offset = 0;
  std::size_t utf16Length = 0;
  while (offset < value.size())
  {
    std::uint32_t codePoint = 0;
    if (!ReadUtf8CodePoint(value, offset, codePoint) || codePoint <= 0x1f || codePoint == 0x7f)
      return false;
    utf16Length += codePoint > 0xffff ? 2 : 1;
    if (utf16Length > maximumUtf16Length)
      return false;
  }
  return true;
}

bool HasOnlyKeys(const CVariant& object, std::initializer_list<std::string_view> allowedKeys)
{
  if (!object.isObject())
    return false;
  for (auto item = object.begin_map(); item != object.end_map(); ++item)
  {
    if (std::find(allowedKeys.begin(), allowedKeys.end(), item->first) == allowedKeys.end())
      return false;
  }
  return true;
}

bool HasAllMembers(const CVariant& object, std::initializer_list<std::string_view> requiredKeys)
{
  return std::all_of(requiredKeys.begin(), requiredKeys.end(),
                     [&object](std::string_view key) { return object.isMember(std::string{key}); });
}

std::optional<int64_t> ReadNonNegativeInt64(const CVariant& value)
{
  if (value.isSignedInteger())
  {
    const int64_t parsed = value.asInteger();
    if (parsed >= 0)
      return parsed;
  }
  else if (value.isUnsignedInteger() &&
           value.asUnsignedInteger() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
  {
    return static_cast<int64_t>(value.asUnsignedInteger());
  }
  return std::nullopt;
}

std::optional<int> ReadNonNegativeInt(const CVariant& value, int maximum)
{
  const std::optional<int64_t> parsed = ReadNonNegativeInt64(value);
  if (!parsed || *parsed > maximum)
    return std::nullopt;
  return static_cast<int>(*parsed);
}

bool SameIdentity(const JumpgateCanonicalIdentity& left, const JumpgateCanonicalIdentity& right)
{
  return left.provider == right.provider && left.id == right.id &&
         left.mediaType == right.mediaType && left.season == right.season &&
         left.episode == right.episode;
}

bool SameEntryPayload(const JumpgatePlaybackHistoryEntry& left,
                      const JumpgatePlaybackHistoryEntry& right)
{
  if (left.canonicalIdentity.has_value() != right.canonicalIdentity.has_value() ||
      (left.canonicalIdentity &&
       !SameIdentity(*left.canonicalIdentity, *right.canonicalIdentity)))
  {
    return false;
  }
  return left.historyNamespace == right.historyNamespace && left.profileId == right.profileId &&
         left.contentKey == right.contentKey && left.display.title == right.display.title &&
         left.display.year == right.display.year && left.display.season == right.display.season &&
         left.display.episode == right.display.episode && left.positionMs == right.positionMs &&
         left.durationMs == right.durationMs && left.completed == right.completed &&
         left.watched == right.watched && left.updatedAtMs == right.updatedAtMs;
}

bool ParseCanonicalIdentity(const CVariant& value, JumpgateCanonicalIdentity& identity)
{
  if (!HasOnlyKeys(value, {"provider", "id", "mediaType", "season", "episode"}) ||
      !HasAllMembers(value, {"provider", "id", "mediaType", "season", "episode"}) ||
      !value["provider"].isString() || !value["id"].isString() || !value["mediaType"].isString())
  {
    return false;
  }

  const std::string provider = value["provider"].asString();
  if (provider == "imdb")
    identity.provider = JumpgateCanonicalProvider::Imdb;
  else if (provider == "tmdb")
    identity.provider = JumpgateCanonicalProvider::Tmdb;
  else if (provider == "tvdb")
    identity.provider = JumpgateCanonicalProvider::Tvdb;
  else if (provider == "trakt")
    identity.provider = JumpgateCanonicalProvider::Trakt;
  else
    return false;

  identity.id = value["id"].asString();
  const std::string mediaType = value["mediaType"].asString();
  if (mediaType == "movie")
  {
    identity.mediaType = JumpgateMediaType::Movie;
    if (!value["season"].isNull() || !value["episode"].isNull())
      return false;
  }
  else if (mediaType == "episode")
  {
    identity.mediaType = JumpgateMediaType::Episode;
    identity.season = ReadNonNegativeInt(value["season"], std::numeric_limits<int>::max());
    identity.episode = ReadNonNegativeInt(value["episode"], std::numeric_limits<int>::max());
    if (!identity.season || !identity.episode)
      return false;
  }
  else
  {
    return false;
  }
  return IsValidJumpgateHistoryCanonicalIdentity(identity);
}

bool ValidateDisplay(const JumpgatePlaybackHistoryDisplay& display,
                     const std::optional<JumpgateCanonicalIdentity>& identity)
{
  if (display.title && !IsBoundedText(*display.title, MAX_TITLE_LENGTH, true, false))
    return false;
  if ((display.year && (*display.year < 0 || *display.year > 9999)) ||
      (display.season && *display.season < 0) || (display.episode && *display.episode < 0))
  {
    return false;
  }
  if (!identity)
    return true;
  if (identity->mediaType == JumpgateMediaType::Movie)
    return !display.season && !display.episode;
  return (!display.season || display.season == identity->season) &&
         (!display.episode || display.episode == identity->episode);
}

bool ParseOptionalDisplayString(const CVariant& value,
                                const char* key,
                                std::optional<std::string>& parsed)
{
  if (value[key].isNull())
    return true;
  if (!value[key].isString())
    return false;
  parsed = value[key].asString();
  return true;
}

bool ParseOptionalDisplayInt(const CVariant& value,
                             const char* key,
                             int maximum,
                             std::optional<int>& parsed)
{
  if (value[key].isNull())
    return true;
  parsed = ReadNonNegativeInt(value[key], maximum);
  return parsed.has_value();
}

bool ParseDisplay(const CVariant& value,
                  const std::optional<JumpgateCanonicalIdentity>& identity,
                  JumpgatePlaybackHistoryDisplay& display)
{
  if (!HasOnlyKeys(value, {"title", "year", "season", "episode"}) ||
      !HasAllMembers(value, {"title", "year", "season", "episode"}) ||
      !ParseOptionalDisplayString(value, "title", display.title) ||
      !ParseOptionalDisplayInt(value, "year", 9999, display.year) ||
      !ParseOptionalDisplayInt(value, "season", std::numeric_limits<int>::max(), display.season) ||
      !ParseOptionalDisplayInt(value, "episode", std::numeric_limits<int>::max(), display.episode))
  {
    return false;
  }
  return ValidateDisplay(display, identity);
}

bool ValidateEntry(const JumpgatePlaybackHistoryEntry& entry)
{
  if (!IsValidJumpgatePlaybackHistoryKey(GetJumpgatePlaybackHistoryKey(entry)) ||
      (entry.canonicalIdentity &&
       !IsValidJumpgateHistoryCanonicalIdentity(*entry.canonicalIdentity)) ||
      !ValidateDisplay(entry.display, entry.canonicalIdentity) || entry.positionMs < 0 ||
      entry.durationMs < 0 || entry.updatedAtMs < 0)
  {
    return false;
  }
  if (entry.historyNamespace == JumpgatePlaybackHistoryNamespace::LocalSource &&
      entry.canonicalIdentity)
  {
    return false;
  }
  if (!entry.completed &&
      IsJumpgatePlaybackThresholdReached(entry.positionMs, entry.durationMs, 90))
  {
    return false;
  }
  return entry.watched ||
         !IsJumpgatePlaybackThresholdReached(entry.positionMs, entry.durationMs, 80);
}

CVariant OptionalInt(const std::optional<int>& value)
{
  return value ? CVariant{*value} : CVariant{};
}

CVariant SerializeCanonicalIdentity(const JumpgateCanonicalIdentity& identity)
{
  CVariant value(CVariant::VariantTypeObject);
  value["provider"] = ToString(identity.provider);
  value["id"] = identity.id;
  value["mediaType"] = ToString(identity.mediaType);
  value["season"] = OptionalInt(identity.season);
  value["episode"] = OptionalInt(identity.episode);
  return value;
}

CVariant SerializeDisplay(const JumpgatePlaybackHistoryDisplay& display)
{
  CVariant value(CVariant::VariantTypeObject);
  value["title"] = display.title ? CVariant{*display.title} : CVariant{};
  value["year"] = OptionalInt(display.year);
  value["season"] = OptionalInt(display.season);
  value["episode"] = OptionalInt(display.episode);
  return value;
}

CVariant SerializeEntry(const JumpgatePlaybackHistoryEntry& entry, int schemaVersion)
{
  CVariant value(CVariant::VariantTypeObject);
  if (schemaVersion == HISTORY_SCHEMA_VERSION)
  {
    value["namespace"] = ToString(entry.historyNamespace);
    value["profileId"] =
        entry.historyNamespace == JumpgatePlaybackHistoryNamespace::AuthenticatedProfile
            ? CVariant{entry.profileId}
            : CVariant{};
  }
  else
  {
    value["profileId"] = entry.profileId;
  }
  value["contentKey"] = entry.contentKey;
  value["canonicalIdentity"] =
      entry.canonicalIdentity ? SerializeCanonicalIdentity(*entry.canonicalIdentity) : CVariant{};
  value["display"] = SerializeDisplay(entry.display);
  value["positionMs"] = entry.positionMs;
  value["durationMs"] = entry.durationMs;
  value["completed"] = entry.completed;
  value["watched"] = entry.watched;
  value["updatedAtMs"] = entry.updatedAtMs;
  return value;
}

bool ParseEntry(const CVariant& value, int schemaVersion, JumpgatePlaybackHistoryEntry& entry)
{
  const bool legacy = schemaVersion == LEGACY_HISTORY_SCHEMA_VERSION;
  const bool validKeys =
      legacy
          ? HasOnlyKeys(value, {"profileId", "contentKey", "canonicalIdentity", "display",
                                "positionMs", "durationMs", "completed", "watched", "updatedAtMs"})
          : HasOnlyKeys(value,
                        {"namespace", "profileId", "contentKey", "canonicalIdentity", "display",
                         "positionMs", "durationMs", "completed", "watched", "updatedAtMs"});
  const bool hasMembers =
      legacy ? HasAllMembers(value,
                             {"profileId", "contentKey", "canonicalIdentity", "display",
                              "positionMs", "durationMs", "completed", "watched", "updatedAtMs"})
             : HasAllMembers(value, {"namespace", "profileId", "contentKey", "canonicalIdentity",
                                     "display", "positionMs", "durationMs", "completed", "watched",
                                     "updatedAtMs"});
  if (!validKeys || !hasMembers || !value["contentKey"].isString() ||
      !value["completed"].isBoolean() || !value["watched"].isBoolean())
  {
    return false;
  }

  if (legacy)
  {
    if (!value["profileId"].isString())
      return false;
    entry.historyNamespace = JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
    entry.profileId = value["profileId"].asString();
  }
  else
  {
    if (!ParseHistoryNamespace(value["namespace"], entry.historyNamespace))
      return false;
    if (entry.historyNamespace == JumpgatePlaybackHistoryNamespace::AuthenticatedProfile)
    {
      if (!value["profileId"].isString())
        return false;
      entry.profileId = value["profileId"].asString();
    }
    else if (!value["profileId"].isNull())
    {
      return false;
    }
  }
  entry.contentKey = value["contentKey"].asString();
  if (!value["canonicalIdentity"].isNull())
  {
    JumpgateCanonicalIdentity identity;
    if (!ParseCanonicalIdentity(value["canonicalIdentity"], identity))
      return false;
    entry.canonicalIdentity = std::move(identity);
  }
  if (!ParseDisplay(value["display"], entry.canonicalIdentity, entry.display))
    return false;
  const std::optional<int64_t> positionMs = ReadNonNegativeInt64(value["positionMs"]);
  const std::optional<int64_t> durationMs = ReadNonNegativeInt64(value["durationMs"]);
  const std::optional<int64_t> updatedAtMs = ReadNonNegativeInt64(value["updatedAtMs"]);
  if (!positionMs || !durationMs || !updatedAtMs)
    return false;
  entry.positionMs = *positionMs;
  entry.durationMs = *durationMs;
  entry.completed = value["completed"].asBoolean();
  entry.watched = value["watched"].asBoolean();
  entry.updatedAtMs = *updatedAtMs;
  return ValidateEntry(entry);
}

auto EntryIdentity(const JumpgatePlaybackHistoryEntry& entry)
{
  return std::tie(entry.historyNamespace, entry.profileId, entry.contentKey);
}

auto EntryAge(const JumpgatePlaybackHistoryEntry& entry)
{
  return std::tie(entry.updatedAtMs, entry.historyNamespace, entry.profileId, entry.contentKey);
}

bool SerializeDocument(const JumpgatePlaybackHistoryDocument& document,
                       std::string& json,
                       bool enforceSize,
                       std::string& error,
                       int schemaVersion = HISTORY_SCHEMA_VERSION)
{
  if (schemaVersion != HISTORY_SCHEMA_VERSION &&
      schemaVersion != LEGACY_HISTORY_SCHEMA_VERSION)
  {
    error = "Jumpgate playback history serialization schema is invalid";
    return false;
  }
  if (document.entries.size() > JUMPGATE_HISTORY_MAX_ENTRIES)
  {
    error = "Jumpgate playback history contains too many entries";
    return false;
  }
  if (document.blockedProfiles.size() > MAX_BLOCKED_PROFILES)
  {
    error = "Jumpgate playback history contains too many blocked profiles";
    return false;
  }
  if (document.forgottenProfiles.size() > MAX_FORGOTTEN_PROFILES)
  {
    error = "Jumpgate playback history contains too many forgotten profiles";
    return false;
  }
  std::set<std::string> uniqueBlockedProfiles;
  for (const std::string& profileId : document.blockedProfiles)
  {
    if (!IsValidJumpgateHistoryProfileId(profileId) ||
        !uniqueBlockedProfiles.emplace(profileId).second)
    {
      error = "Jumpgate playback history contains an invalid blocked profile";
      return false;
    }
  }
  std::set<std::string> uniqueForgottenProfiles;
  for (const std::string& profileId : document.forgottenProfiles)
  {
    if (!IsValidJumpgateHistoryProfileId(profileId) ||
        !uniqueForgottenProfiles.emplace(profileId).second)
    {
      error = "Jumpgate playback history contains an invalid forgotten profile";
      return false;
    }
  }
  std::set<std::tuple<JumpgatePlaybackHistoryNamespace, std::string, std::string>> uniqueEntries;
  std::vector<JumpgatePlaybackHistoryEntry> ordered = document.entries;
  for (const JumpgatePlaybackHistoryEntry& entry : ordered)
  {
    if (!ValidateEntry(entry) ||
        (schemaVersion == LEGACY_HISTORY_SCHEMA_VERSION &&
         entry.historyNamespace != JumpgatePlaybackHistoryNamespace::AuthenticatedProfile) ||
        !uniqueEntries.emplace(entry.historyNamespace, entry.profileId, entry.contentKey).second)
    {
      error = "Jumpgate playback history contains an invalid or duplicate entry";
      return false;
    }
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right)
            { return EntryIdentity(left) < EntryIdentity(right); });

  CVariant root(CVariant::VariantTypeObject);
  root["schemaVersion"] = schemaVersion;
  CVariant blockedProfiles(CVariant::VariantTypeArray);
  for (const std::string& profileId : uniqueBlockedProfiles)
    blockedProfiles.push_back(profileId);
  root["blockedProfiles"] = std::move(blockedProfiles);
  CVariant forgottenProfiles(CVariant::VariantTypeArray);
  for (const std::string& profileId : uniqueForgottenProfiles)
    forgottenProfiles.push_back(profileId);
  root["forgottenProfiles"] = std::move(forgottenProfiles);
  CVariant entries(CVariant::VariantTypeArray);
  for (const JumpgatePlaybackHistoryEntry& entry : ordered)
    entries.push_back(SerializeEntry(entry, schemaVersion));
  root["entries"] = std::move(entries);
  if (!CJSONVariantWriter::Write(root, json, true))
  {
    error = "Jumpgate playback history serialization failed";
    return false;
  }
  if (enforceSize && json.size() > JUMPGATE_HISTORY_MAX_BYTES)
  {
    error = "Jumpgate playback history exceeds the size limit";
    return false;
  }
  return true;
}

} // namespace

bool IsValidJumpgateHistoryProfileId(const std::string& profileId)
{
  return IsBoundedText(profileId, MAX_PROFILE_ID_LENGTH, false, true);
}

bool IsValidJumpgateHistoryContentKey(const std::string& contentKey)
{
  return contentKey.size() == 64 && std::all_of(contentKey.begin(), contentKey.end(),
                                                [](char character)
                                                {
                                                  return (character >= '0' && character <= '9') ||
                                                         (character >= 'a' && character <= 'f');
                                                });
}

bool IsValidJumpgatePlaybackHistoryKey(const JumpgatePlaybackHistoryKey& key)
{
  if (!IsValidJumpgateHistoryContentKey(key.contentKey))
    return false;
  switch (key.historyNamespace)
  {
    case JumpgatePlaybackHistoryNamespace::AuthenticatedProfile:
      return IsValidJumpgateHistoryProfileId(key.profileId);
    case JumpgatePlaybackHistoryNamespace::LocalSource:
      return key.profileId.empty();
  }
  return false;
}

JumpgatePlaybackHistoryKey GetJumpgatePlaybackHistoryKey(const JumpgatePlaybackHistoryEntry& entry)
{
  return {entry.historyNamespace, entry.profileId, entry.contentKey};
}

const char* ToString(JumpgatePlaybackHistoryNamespace historyNamespace)
{
  switch (historyNamespace)
  {
    case JumpgatePlaybackHistoryNamespace::AuthenticatedProfile:
      return "authenticated_profile";
    case JumpgatePlaybackHistoryNamespace::LocalSource:
      return "local_source";
  }
  return "invalid";
}

std::optional<std::string> DeriveJumpgateLocalSourceHistoryKey(
    const std::vector<std::string>& canonicalFingerprints)
{
  if (canonicalFingerprints.empty() || canonicalFingerprints.size() > 64)
    return std::nullopt;

  std::vector<std::string> ordered = canonicalFingerprints;
  for (const std::string& fingerprint : ordered)
  {
    if (fingerprint.empty() || fingerprint.size() > 4096 ||
        !std::all_of(fingerprint.begin(), fingerprint.end(), [](unsigned char character)
                     { return character >= 0x21 && character <= 0x7e; }))
    {
      return std::nullopt;
    }
  }
  std::sort(ordered.begin(), ordered.end());
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

  std::vector<std::string_view> material;
  material.reserve(ordered.size());
  for (const std::string& fingerprint : ordered)
    material.emplace_back(fingerprint);
  return DigestLengthPrefixed(LOCAL_SOURCE_FINGERPRINT_DOMAIN, material);
}

std::string DeriveJumpgateLocalSourceFallbackHistoryKey(std::string_view rawLaunchUri)
{
  return DigestLengthPrefixed(LOCAL_SOURCE_FALLBACK_DOMAIN, {rawLaunchUri});
}

bool IsValidJumpgateHistoryCanonicalIdentity(const JumpgateCanonicalIdentity& identity)
{
  if (!IsBoundedText(identity.id, MAX_CANONICAL_ID_LENGTH, false, true))
    return false;
  switch (identity.provider)
  {
    case JumpgateCanonicalProvider::Imdb:
      if (identity.id.size() < 9 || identity.id.compare(0, 2, "tt") != 0 ||
          !std::all_of(identity.id.begin() + 2, identity.id.end(),
                       [](char character) { return character >= '0' && character <= '9'; }))
      {
        return false;
      }
      break;
    case JumpgateCanonicalProvider::Tmdb:
    case JumpgateCanonicalProvider::Tvdb:
    case JumpgateCanonicalProvider::Trakt:
    {
      uint64_t numericId = 0;
      const auto [end, parseError] =
          std::from_chars(identity.id.data(), identity.id.data() + identity.id.size(), numericId);
      if (parseError != std::errc{} || end != identity.id.data() + identity.id.size() ||
          numericId == 0 ||
          numericId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
          identity.id.front() == '0')
      {
        return false;
      }
      break;
    }
    default:
      return false;
  }
  if (identity.mediaType == JumpgateMediaType::Movie)
    return !identity.season && !identity.episode;
  return identity.mediaType == JumpgateMediaType::Episode && identity.season && identity.episode &&
         *identity.season >= 0 && *identity.episode >= 0;
}

bool IsJumpgatePlaybackThresholdReached(int64_t positionMs, int64_t durationMs, int percentage)
{
  if (positionMs < 0 || durationMs <= 0 || percentage <= 0 || percentage > 100)
    return false;
  const int64_t whole = durationMs / 100;
  const int64_t remainder = durationMs % 100;
  const int64_t threshold = whole * percentage + (remainder * percentage + 99) / 100;
  return positionMs >= threshold;
}

int64_t GetJumpgatePlaybackResumePosition(const JumpgatePlaybackHistoryEntry& entry)
{
  return entry.completed ? 0 : entry.positionMs;
}

bool ParseJumpgatePlaybackHistory(const std::string& json,
                                  JumpgatePlaybackHistoryDocument& document,
                                  std::string& error)
{
  document = {};
  error.clear();
  if (json.empty() || json.size() > JUMPGATE_HISTORY_MAX_BYTES)
  {
    error = "Jumpgate playback history is empty or exceeds the size limit";
    return false;
  }
  CVariant root;
  if (!CJSONVariantParser::Parse(json, root) ||
      !HasOnlyKeys(root, {"schemaVersion", "entries", "blockedProfiles", "forgottenProfiles"}) ||
      !HasAllMembers(root, {"schemaVersion", "entries"}) || !root["schemaVersion"].isInteger() ||
      (root["schemaVersion"].asInteger(-1) != HISTORY_SCHEMA_VERSION &&
       root["schemaVersion"].asInteger(-1) != LEGACY_HISTORY_SCHEMA_VERSION) ||
      !root["entries"].isArray() || root["entries"].size() > JUMPGATE_HISTORY_MAX_ENTRIES)
  {
    error = "Jumpgate playback history has an invalid schema";
    return false;
  }

  if (root.isMember("forgottenProfiles"))
  {
    if (!root["forgottenProfiles"].isArray() ||
        root["forgottenProfiles"].size() > MAX_FORGOTTEN_PROFILES)
    {
      error = "Jumpgate playback history has invalid forgotten profiles";
      return false;
    }
    std::set<std::string> uniqueForgottenProfiles;
    for (auto item = root["forgottenProfiles"].begin_array();
         item != root["forgottenProfiles"].end_array(); ++item)
    {
      if (!item->isString() || !IsValidJumpgateHistoryProfileId(item->asString()) ||
          !uniqueForgottenProfiles.emplace(item->asString()).second)
      {
        document = {};
        error = "Jumpgate playback history has invalid forgotten profiles";
        return false;
      }
      document.forgottenProfiles.emplace_back(item->asString());
    }
  }

  if (root.isMember("blockedProfiles"))
  {
    if (!root["blockedProfiles"].isArray() || root["blockedProfiles"].size() > MAX_BLOCKED_PROFILES)
    {
      error = "Jumpgate playback history has invalid blocked profiles";
      return false;
    }
    std::set<std::string> uniqueBlockedProfiles;
    for (auto item = root["blockedProfiles"].begin_array();
         item != root["blockedProfiles"].end_array(); ++item)
    {
      if (!item->isString() || !IsValidJumpgateHistoryProfileId(item->asString()) ||
          !uniqueBlockedProfiles.emplace(item->asString()).second)
      {
        document = {};
        error = "Jumpgate playback history has invalid blocked profiles";
        return false;
      }
      document.blockedProfiles.emplace_back(item->asString());
    }
  }

  const int schemaVersion = root["schemaVersion"].asInteger();
  document.loadedFromLegacySchema = schemaVersion == LEGACY_HISTORY_SCHEMA_VERSION;
  std::set<std::tuple<JumpgatePlaybackHistoryNamespace, std::string, std::string>> uniqueEntries;
  document.entries.reserve(root["entries"].size());
  for (auto item = root["entries"].begin_array(); item != root["entries"].end_array(); ++item)
  {
    JumpgatePlaybackHistoryEntry entry;
    if (!ParseEntry(*item, schemaVersion, entry) ||
        !uniqueEntries.emplace(entry.historyNamespace, entry.profileId, entry.contentKey).second)
    {
      document = {};
      error = "Jumpgate playback history contains an invalid or duplicate entry";
      return false;
    }
    document.entries.emplace_back(std::move(entry));
  }
  return true;
}

bool SerializeJumpgatePlaybackHistory(const JumpgatePlaybackHistoryDocument& document,
                                      std::string& json,
                                      std::string& error)
{
  error.clear();
  return SerializeDocument(document, json, true, error);
}

CJumpgatePlaybackHistoryStore::CJumpgatePlaybackHistoryStore(IJumpgateProfileStorage& storage)
  : m_storage(storage)
{
}

bool CJumpgatePlaybackHistoryStore::Load(JumpgatePlaybackHistoryDocument& document,
                                         std::string& error) const
{
  std::string contents;
  bool exists = false;
  if (!m_storage.Read(contents, exists, error))
    return false;
  if (!exists)
  {
    document = {};
    error.clear();
    return true;
  }
  return ParseJumpgatePlaybackHistory(contents, document, error);
}

bool CJumpgatePlaybackHistoryStore::Get(const JumpgatePlaybackHistoryKey& key,
                                        std::optional<JumpgatePlaybackHistoryEntry>& entry,
                                        std::string& error) const
{
  entry.reset();
  if (!IsValidJumpgatePlaybackHistoryKey(key))
  {
    error = "Jumpgate playback history lookup identity is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool profileBound =
      key.historyNamespace == JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
  if (profileBound && (m_blockedProfiles.find(key.profileId) != m_blockedProfiles.end() ||
                       m_forgottenProfiles.find(key.profileId) != m_forgottenProfiles.end()))
  {
    error.clear();
    return true;
  }
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  if (profileBound &&
      (std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(), key.profileId) !=
           document.blockedProfiles.end() ||
       std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(),
                 key.profileId) != document.forgottenProfiles.end()))
  {
    m_blockedProfiles.emplace(key.profileId);
    if (std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(),
                  key.profileId) != document.forgottenProfiles.end())
    {
      m_forgottenProfiles.emplace(key.profileId);
    }
    error.clear();
    return true;
  }
  const auto found = std::find_if(document.entries.begin(), document.entries.end(),
                                  [&key](const auto& candidate)
                                  {
                                    return candidate.historyNamespace == key.historyNamespace &&
                                           candidate.profileId == key.profileId &&
                                           candidate.contentKey == key.contentKey;
                                  });
  if (found != document.entries.end())
    entry = *found;
  return true;
}

bool CJumpgatePlaybackHistoryStore::Save(const JumpgatePlaybackHistoryKey& expectedKey,
                                         JumpgatePlaybackHistoryEntry entry,
                                         std::string& error)
{
  const bool explicitCompletion = entry.completed;
  const bool meaningfulProgress = entry.positionMs > 0 && entry.durationMs > 0;
  const bool thresholdCompletion =
      IsJumpgatePlaybackThresholdReached(entry.positionMs, entry.durationMs, 90);
  entry.completed = explicitCompletion || thresholdCompletion;
  entry.watched =
      entry.watched || IsJumpgatePlaybackThresholdReached(entry.positionMs, entry.durationMs, 80);
  const JumpgatePlaybackHistoryKey entryKey = GetJumpgatePlaybackHistoryKey(entry);
  if (!IsValidJumpgatePlaybackHistoryKey(expectedKey) ||
      expectedKey.historyNamespace != entryKey.historyNamespace ||
      expectedKey.profileId != entryKey.profileId ||
      expectedKey.contentKey != entryKey.contentKey || !ValidateEntry(entry))
  {
    error = "Jumpgate playback history identity mismatch or invalid entry";
    return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const bool profileBound =
      expectedKey.historyNamespace == JumpgatePlaybackHistoryNamespace::AuthenticatedProfile;
  if (profileBound &&
      (m_blockedProfiles.find(expectedKey.profileId) != m_blockedProfiles.end() ||
       m_forgottenProfiles.find(expectedKey.profileId) != m_forgottenProfiles.end()))
  {
    error = "Jumpgate playback history profile was cleared";
    return false;
  }
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  const bool persistedForgotten =
      profileBound &&
      std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(),
                expectedKey.profileId) != document.forgottenProfiles.end();
  if (profileBound && (std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(),
                                 expectedKey.profileId) != document.blockedProfiles.end() ||
                       persistedForgotten))
  {
    m_blockedProfiles.emplace(expectedKey.profileId);
    if (persistedForgotten)
      m_forgottenProfiles.emplace(expectedKey.profileId);
    error = "Jumpgate playback history profile is blocked";
    return false;
  }
  const auto found =
      std::find_if(document.entries.begin(), document.entries.end(), [&entry](const auto& candidate)
                   { return EntryIdentity(candidate) == EntryIdentity(entry); });
  if (found != document.entries.end())
  {
    if (entry.updatedAtMs < found->updatedAtMs)
    {
      error = "Jumpgate playback history update is stale";
      return false;
    }
    if (found->canonicalIdentity && entry.canonicalIdentity &&
        !SameIdentity(*found->canonicalIdentity, *entry.canonicalIdentity))
    {
      error = "Jumpgate playback history canonical identity changed";
      return false;
    }
    if (!entry.canonicalIdentity)
      entry.canonicalIdentity = found->canonicalIdentity;
    if (!explicitCompletion && !thresholdCompletion)
      entry.completed = meaningfulProgress ? false : found->completed;
    entry.watched = entry.watched || found->watched;
    if (entry.positionMs == 0 && entry.durationMs == 0)
    {
      entry.positionMs = found->positionMs;
      entry.durationMs = found->durationMs;
    }
    if (entry.updatedAtMs == found->updatedAtMs)
    {
      if (!SameEntryPayload(entry, *found))
      {
        error = "Jumpgate playback history update timestamp conflicts with persisted state";
        return false;
      }
      error.clear();
      return true;
    }
    *found = entry;
  }
  else
  {
    document.entries.emplace_back(entry);
  }
  return WriteCandidate(document, entryKey, error);
}

bool CJumpgatePlaybackHistoryStore::ClearProfile(const std::string& profileId, std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  if (std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId) ==
      document.blockedProfiles.end())
  {
    document.blockedProfiles.emplace_back(profileId);
  }
  if (std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId) ==
      document.forgottenProfiles.end())
  {
    document.forgottenProfiles.emplace_back(profileId);
  }
  document.entries.erase(
      std::remove_if(document.entries.begin(), document.entries.end(),
                     [&profileId](const auto& entry)
                     {
                       return entry.historyNamespace ==
                                  JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                              entry.profileId == profileId;
                     }),
      document.entries.end());
  if (!WriteDestructiveProtectionCandidate(document, profileId, error))
  {
    m_blockedProfiles.emplace(profileId);
    return false;
  }
  m_blockedProfiles.emplace(profileId);
  m_forgottenProfiles.emplace(profileId);
  return true;
}

bool CJumpgatePlaybackHistoryStore::BlockProfile(const std::string& profileId, std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  if (std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId) ==
      document.blockedProfiles.end())
  {
    document.blockedProfiles.emplace_back(profileId);
    if (!WriteExact(document, error))
      return false;
  }
  m_blockedProfiles.emplace(profileId);
  error.clear();
  return true;
}

bool CJumpgatePlaybackHistoryStore::IsProfileBlocked(const std::string& profileId,
                                                     bool& blocked,
                                                     std::string& error) const
{
  bool forgotten = false;
  return GetProfileProtection(profileId, blocked, forgotten, error);
}

bool CJumpgatePlaybackHistoryStore::GetProfileProtection(const std::string& profileId,
                                                         bool& blocked,
                                                         bool& forgotten,
                                                         std::string& error) const
{
  blocked = false;
  forgotten = false;
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  forgotten = m_forgottenProfiles.find(profileId) != m_forgottenProfiles.end() ||
              std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(),
                        profileId) != document.forgottenProfiles.end();
  blocked = forgotten || m_blockedProfiles.find(profileId) != m_blockedProfiles.end() ||
            std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(),
                      profileId) != document.blockedProfiles.end();
  if (blocked)
    m_blockedProfiles.emplace(profileId);
  if (forgotten)
    m_forgottenProfiles.emplace(profileId);
  error.clear();
  return true;
}

bool CJumpgatePlaybackHistoryStore::UnblockProfile(const std::string& profileId, std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  if (m_forgottenProfiles.find(profileId) != m_forgottenProfiles.end() ||
      std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId) !=
          document.forgottenProfiles.end())
  {
    error = "Jumpgate playback history profile is forgotten and must be purged before repair";
    return false;
  }
  document.blockedProfiles.erase(
      std::remove(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId),
      document.blockedProfiles.end());
  if (!WriteExact(document, error))
    return false;
  m_blockedProfiles.erase(profileId);
  return true;
}

bool CJumpgatePlaybackHistoryStore::PurgeBlockedProfile(const std::string& profileId,
                                                        std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  const bool protectedInMemory = m_blockedProfiles.find(profileId) != m_blockedProfiles.end() ||
                                 m_forgottenProfiles.find(profileId) != m_forgottenProfiles.end();
  const bool protectedOnDisk =
      std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId) !=
          document.blockedProfiles.end() ||
      std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId) !=
          document.forgottenProfiles.end();
  if (!protectedInMemory && !protectedOnDisk)
  {
    error = "Jumpgate playback history profile is not blocked";
    return false;
  }
  if (std::find(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId) ==
      document.blockedProfiles.end())
  {
    document.blockedProfiles.emplace_back(profileId);
  }
  if (std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId) ==
      document.forgottenProfiles.end())
  {
    document.forgottenProfiles.emplace_back(profileId);
  }
  // Persist forgotten provenance independently from the destructive purge so
  // a failed second atomic write cannot make later pairing resurrect records.
  if (!WriteDestructiveProtectionCandidate(document, profileId, error))
  {
    m_blockedProfiles.emplace(profileId);
    m_forgottenProfiles.emplace(profileId);
    return false;
  }
  m_blockedProfiles.emplace(profileId);
  m_forgottenProfiles.emplace(profileId);
  document.entries.erase(
      std::remove_if(document.entries.begin(), document.entries.end(),
                     [&profileId](const auto& entry)
                     {
                       return entry.historyNamespace ==
                                  JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                              entry.profileId == profileId;
                     }),
      document.entries.end());
  if (!WriteExact(document, error))
  {
    return false;
  }
  return true;
}

bool CJumpgatePlaybackHistoryStore::CompleteProfileRepair(const std::string& profileId,
                                                          std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  const bool forgotten =
      m_forgottenProfiles.find(profileId) != m_forgottenProfiles.end() ||
      std::find(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId) !=
          document.forgottenProfiles.end();
  if (forgotten &&
      std::any_of(document.entries.begin(), document.entries.end(),
                  [&profileId](const auto& entry)
                  {
                    return entry.historyNamespace ==
                               JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                           entry.profileId == profileId;
                  }))
  {
    error = "Jumpgate playback history profile must be purged before repair";
    return false;
  }

  document.blockedProfiles.erase(
      std::remove(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId),
      document.blockedProfiles.end());
  document.forgottenProfiles.erase(
      std::remove(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId),
      document.forgottenProfiles.end());
  if (!WriteExact(document, error))
    return false;
  m_blockedProfiles.erase(profileId);
  m_forgottenProfiles.erase(profileId);
  return true;
}

bool CJumpgatePlaybackHistoryStore::ResetProfile(const std::string& profileId, std::string& error)
{
  if (!IsValidJumpgateHistoryProfileId(profileId))
  {
    error = "Jumpgate playback history profile is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  JumpgatePlaybackHistoryDocument document;
  if (!Load(document, error))
    return false;
  document.entries.erase(
      std::remove_if(document.entries.begin(), document.entries.end(),
                     [&profileId](const auto& entry)
                     {
                       return entry.historyNamespace ==
                                  JumpgatePlaybackHistoryNamespace::AuthenticatedProfile &&
                              entry.profileId == profileId;
                     }),
      document.entries.end());
  document.blockedProfiles.erase(
      std::remove(document.blockedProfiles.begin(), document.blockedProfiles.end(), profileId),
      document.blockedProfiles.end());
  document.forgottenProfiles.erase(
      std::remove(document.forgottenProfiles.begin(), document.forgottenProfiles.end(), profileId),
      document.forgottenProfiles.end());
  if (!WriteExact(document, error))
    return false;
  m_blockedProfiles.erase(profileId);
  m_forgottenProfiles.erase(profileId);
  return true;
}

bool CJumpgatePlaybackHistoryStore::WriteExact(const JumpgatePlaybackHistoryDocument& document,
                                               std::string& error)
{
  std::string json;
  if (!SerializeDocument(document, json, false, error))
    return false;
  if (json.size() > JUMPGATE_HISTORY_MAX_BYTES)
  {
    if (!document.loadedFromLegacySchema)
    {
      error = "Jumpgate playback history exceeds the size limit";
      return false;
    }
    if (!SerializeDocument(document, json, true, error, LEGACY_HISTORY_SCHEMA_VERSION))
      return false;
  }
  return m_storage.WriteAtomic(json, error);
}

bool CJumpgatePlaybackHistoryStore::WriteCandidate(JumpgatePlaybackHistoryDocument& document,
                                                    const JumpgatePlaybackHistoryKey& retainedKey,
                                                    std::string& error)
{
  std::string json;
  while (true)
  {
    while (document.entries.size() > JUMPGATE_HISTORY_MAX_ENTRIES)
    {
      const auto oldest = std::min_element(document.entries.begin(), document.entries.end(),
                                           [](const auto& left, const auto& right)
                                           { return EntryAge(left) < EntryAge(right); });
      document.entries.erase(oldest);
    }
    if (!SerializeDocument(document, json, false, error))
      return false;
    if (json.size() <= JUMPGATE_HISTORY_MAX_BYTES -
                           JUMPGATE_HISTORY_PROFILE_PROTECTION_RESERVE_BYTES)
      break;
    if (document.entries.empty())
    {
      error = "Jumpgate playback history cannot fit within the size limit";
      return false;
    }
    const auto oldest = std::min_element(document.entries.begin(), document.entries.end(),
                                         [](const auto& left, const auto& right)
                                         { return EntryAge(left) < EntryAge(right); });
    document.entries.erase(oldest);
  }

  if (std::none_of(document.entries.begin(), document.entries.end(),
                   [&retainedKey](const auto& entry)
                   {
                     return entry.historyNamespace == retainedKey.historyNamespace &&
                            entry.profileId == retainedKey.profileId &&
                            entry.contentKey == retainedKey.contentKey;
                   }))
  {
    error = "Jumpgate playback history update was older than the retained entries";
    return false;
  }
  return m_storage.WriteAtomic(json, error);
}

bool CJumpgatePlaybackHistoryStore::WriteDestructiveProtectionCandidate(
    JumpgatePlaybackHistoryDocument& document,
    const std::string& affectedProfileId,
    std::string& error)
{
  std::string json;
  while (true)
  {
    if (!SerializeDocument(document, json, false, error))
      return false;
    if (json.size() <= JUMPGATE_HISTORY_MAX_BYTES)
      return m_storage.WriteAtomic(json, error);

    if (document.loadedFromLegacySchema)
    {
      if (!SerializeDocument(document, json, false, error, LEGACY_HISTORY_SCHEMA_VERSION))
        return false;
      if (json.size() <= JUMPGATE_HISTORY_MAX_BYTES)
        return m_storage.WriteAtomic(json, error);
    }

    auto oldest = document.entries.end();
    for (auto candidate = document.entries.begin(); candidate != document.entries.end(); ++candidate)
    {
      if (candidate->historyNamespace !=
              JumpgatePlaybackHistoryNamespace::AuthenticatedProfile ||
          candidate->profileId != affectedProfileId)
      {
        continue;
      }
      if (oldest == document.entries.end() || EntryAge(*candidate) < EntryAge(*oldest))
        oldest = candidate;
    }
    if (oldest == document.entries.end())
    {
      error = "Jumpgate playback history protection markers cannot fit within the size limit";
      return false;
    }
    document.entries.erase(oldest);
  }
}

} // namespace KODI::JUMPGATE
