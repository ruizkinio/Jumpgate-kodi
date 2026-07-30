/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgatePlaybackContext.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>

namespace KODI::JUMPGATE
{
namespace
{
constexpr std::size_t MAX_CONTEXT_STRING_LENGTH = 8192;
constexpr std::size_t MAX_CONTEXT_STRING_BYTES = MAX_CONTEXT_STRING_LENGTH * 4;
constexpr std::size_t MAX_CONTEXT_ARRAY_LENGTH = 64;
constexpr std::size_t MAX_CONTEXT_OBJECT_KEYS = 64;
constexpr std::size_t MAX_CONTEXT_DEPTH = 6;
constexpr std::size_t MAX_CONTEXT_NODES = 2048;
constexpr std::size_t MAX_CONTEXT_TOTAL_BYTES = 256 * 1024;
constexpr std::size_t MAX_OBJECT_KEY_LENGTH = 128;
constexpr std::size_t MAX_IDENTIFIER_LENGTH = 256;
constexpr std::size_t MAX_FINGERPRINTS = 32;
constexpr std::size_t MAX_FINGERPRINT_LENGTH = 512;
constexpr std::int64_t MAX_SAFE_INTEGER = 9007199254740991LL;
constexpr std::int64_t MAX_CONTEXT_TTL_MS = 7LL * 24 * 60 * 60 * 1000;

struct TreeBudget
{
  std::size_t nodes{0};
  std::size_t bytes{0};
};

bool AddWithoutOverflow(std::size_t amount, std::size_t maximum, std::size_t& total)
{
  if (amount > maximum - total)
    return false;
  total += amount;
  return true;
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

bool IsValidUtf8WithLength(std::string_view value,
                           std::size_t maximumUtf16Length,
                           bool rejectControls)
{
  std::size_t offset = 0;
  std::size_t utf16Length = 0;
  while (offset < value.size())
  {
    std::uint32_t codePoint = 0;
    if (!ReadUtf8CodePoint(value, offset, codePoint))
      return false;
    utf16Length += codePoint > 0xffff ? 2 : 1;
    if (utf16Length > maximumUtf16Length)
      return false;
    if (rejectControls && (codePoint <= 0x1f || codePoint == 0x7f))
      return false;
  }
  return true;
}

bool IsTrimmed(std::string_view value)
{
  if (value.empty())
    return true;
  const auto isEdgeWhitespace = [](unsigned char character)
  { return character <= 0x20 || character == 0x7f; };
  return !isEdgeWhitespace(static_cast<unsigned char>(value.front())) &&
         !isEdgeWhitespace(static_cast<unsigned char>(value.back()));
}

bool IsBoundedString(const CVariant& value,
                     std::size_t maximumLength,
                     bool allowEmpty,
                     bool rejectControls,
                     bool requireTrimmed)
{
  if (!value.isString())
    return false;
  const std::string text = value.asString();
  return (allowEmpty || !text.empty()) && text.size() <= maximumLength * 4 &&
         IsValidUtf8WithLength(text, maximumLength, rejectControls) &&
         (!requireTrimmed || IsTrimmed(text));
}

bool IsIdentifier(const CVariant& value)
{
  return IsBoundedString(value, MAX_IDENTIFIER_LENGTH, false, true, true);
}

bool IsForbiddenObjectKey(std::string_view key)
{
  return key == "__proto__" || key == "prototype" || key == "constructor";
}

bool ValidateBoundedTree(const CVariant& value, TreeBudget& budget, std::size_t depth)
{
  if (++budget.nodes > MAX_CONTEXT_NODES || depth > MAX_CONTEXT_DEPTH)
    return false;

  if (value.isNull() || value.isBoolean())
    return true;
  if (value.isSignedInteger())
  {
    const std::int64_t number = value.asInteger();
    return number >= -MAX_SAFE_INTEGER && number <= MAX_SAFE_INTEGER;
  }
  if (value.isUnsignedInteger())
    return value.asUnsignedInteger() <= static_cast<std::uint64_t>(MAX_SAFE_INTEGER);
  if (value.isString())
  {
    const std::string text = value.asString();
    return text.size() <= MAX_CONTEXT_STRING_BYTES &&
           IsValidUtf8WithLength(text, MAX_CONTEXT_STRING_LENGTH, false) &&
           AddWithoutOverflow(text.size(), MAX_CONTEXT_TOTAL_BYTES, budget.bytes);
  }
  if (value.isArray())
  {
    if (value.size() > MAX_CONTEXT_ARRAY_LENGTH)
      return false;
    for (auto item = value.begin_array(); item != value.end_array(); ++item)
    {
      if (!ValidateBoundedTree(*item, budget, depth + 1))
        return false;
    }
    return true;
  }
  if (value.isObject())
  {
    if (value.size() > MAX_CONTEXT_OBJECT_KEYS)
      return false;
    for (auto item = value.begin_map(); item != value.end_map(); ++item)
    {
      if (item->first.empty() || item->first.size() > MAX_OBJECT_KEY_LENGTH * 4 ||
          !IsValidUtf8WithLength(item->first, MAX_OBJECT_KEY_LENGTH, true) ||
          IsForbiddenObjectKey(item->first) ||
          !AddWithoutOverflow(item->first.size(), MAX_CONTEXT_TOTAL_BYTES, budget.bytes) ||
          !ValidateBoundedTree(item->second, budget, depth + 1))
      {
        return false;
      }
    }
    return true;
  }
  return false;
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

std::optional<int> ReadBoundedNonNegativeInteger(const CVariant& value, int maximum)
{
  if (value.isSignedInteger())
  {
    const std::int64_t number = value.asInteger();
    if (number >= 0 && number <= maximum)
      return static_cast<int>(number);
  }
  else if (value.isUnsignedInteger() &&
           value.asUnsignedInteger() <= static_cast<std::uint64_t>(maximum))
  {
    return static_cast<int>(value.asUnsignedInteger());
  }
  return std::nullopt;
}

bool IsExactInteger(const CVariant& value, int expected)
{
  const std::optional<int> parsed = ReadBoundedNonNegativeInteger(value, expected);
  return parsed && *parsed == expected;
}

bool IsLowerHex(std::string_view value, std::size_t length)
{
  return value.size() == length && std::all_of(value.begin(), value.end(),
                                               [](char character)
                                               {
                                                 return (character >= '0' && character <= '9') ||
                                                        (character >= 'a' && character <= 'f');
                                               });
}

bool ParseFileIndex(std::string_view value)
{
  if (value.empty() || (value.size() > 1 && value.front() == '0'))
    return false;
  unsigned int parsed = 0;
  for (char character : value)
  {
    if (character < '0' || character > '9')
      return false;
    parsed = parsed * 10 + static_cast<unsigned int>(character - '0');
    if (parsed > 65535)
      return false;
  }
  return true;
}

bool IsCanonicalFingerprint(std::string_view fingerprint)
{
  constexpr std::string_view HASH_MARKER = ":sha256:";
  constexpr std::array<std::string_view, 15> HASHED_TYPES = {
      "url",         "external-url",     "android-tv-url", "tizen-url",
      "webos-url",   "player-frame-url", "yt-id",          "proxy-source",
      "archive-rar", "archive-zip",      "archive-7zip",   "archive-tgz",
      "archive-tar", "nzb-source",       "opaque"};
  constexpr std::string_view HASHED_PREFIX = "v1:";
  if (fingerprint.compare(0, HASHED_PREFIX.size(), HASHED_PREFIX) == 0)
  {
    const std::size_t marker = fingerprint.find(HASH_MARKER, HASHED_PREFIX.size());
    if (marker != std::string_view::npos)
    {
      const std::string_view type =
          fingerprint.substr(HASHED_PREFIX.size(), marker - HASHED_PREFIX.size());
      if (std::find(HASHED_TYPES.begin(), HASHED_TYPES.end(), type) != HASHED_TYPES.end() &&
          IsLowerHex(fingerprint.substr(marker + HASH_MARKER.size()), 64))
      {
        return true;
      }
    }
  }

  constexpr std::string_view INFO_PREFIX = "v1:info-hash:";
  constexpr std::string_view FILE_INDEX_MARKER = ":file-idx:";
  if (fingerprint.compare(0, INFO_PREFIX.size(), INFO_PREFIX) != 0 ||
      fingerprint.size() < INFO_PREFIX.size() + 40 + FILE_INDEX_MARKER.size() + 1 ||
      !IsLowerHex(fingerprint.substr(INFO_PREFIX.size(), 40), 40) ||
      fingerprint.substr(INFO_PREFIX.size() + 40, FILE_INDEX_MARKER.size()) != FILE_INDEX_MARKER)
  {
    return false;
  }

  std::string_view selector =
      fingerprint.substr(INFO_PREFIX.size() + 40 + FILE_INDEX_MARKER.size());
  constexpr std::string_view FILTER_MARKER = ":file-must-include:sha256:";
  const std::size_t filter = selector.find(FILTER_MARKER);
  if (filter == std::string_view::npos)
    return selector == "-1" || ParseFileIndex(selector);
  return selector.substr(0, filter) == "-1" &&
         IsLowerHex(selector.substr(filter + FILTER_MARKER.size()), 64);
}

bool IsValidPort(std::string_view port)
{
  if (port.empty() || port.size() > 5)
    return false;
  unsigned int parsed = 0;
  for (char character : port)
  {
    if (character < '0' || character > '9')
      return false;
    parsed = parsed * 10 + static_cast<unsigned int>(character - '0');
  }
  return parsed > 0 && parsed <= 65535;
}

bool IsValidAuthority(std::string_view authority)
{
  if (authority.empty() || authority.find('@') != std::string_view::npos)
    return false;
  if (authority.front() == '[')
  {
    const std::size_t closingBracket = authority.find(']');
    if (closingBracket <= 1)
      return false;
    const std::string_view address = authority.substr(1, closingBracket - 1);
    if (address.find(':') == std::string_view::npos ||
        !std::all_of(address.begin(), address.end(),
                     [](char character)
                     {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f') ||
                              (character >= 'A' && character <= 'F') || character == ':' ||
                              character == '.';
                     }))
    {
      return false;
    }
    const std::string_view suffix = authority.substr(closingBracket + 1);
    return suffix.empty() || (suffix.front() == ':' && IsValidPort(suffix.substr(1)));
  }

  const std::size_t colon = authority.find(':');
  const std::string_view host = authority.substr(0, colon);
  if (host.empty() || !std::all_of(host.begin(), host.end(),
                                   [](char character)
                                   {
                                     return (character >= '0' && character <= '9') ||
                                            (character >= 'a' && character <= 'z') ||
                                            (character >= 'A' && character <= 'Z') ||
                                            character == '-' || character == '.';
                                   }))
  {
    return false;
  }
  return colon == std::string_view::npos ||
         (authority.find(':', colon + 1) == std::string_view::npos &&
          IsValidPort(authority.substr(colon + 1)));
}

bool IsSafeRemoteUrl(const CVariant& value)
{
  if (!IsBoundedString(value, MAX_CONTEXT_STRING_LENGTH, false, true, true))
    return false;
  const std::string url = value.asString();
  constexpr std::string_view PREFIX = "https://";
  if (url.compare(0, PREFIX.size(), PREFIX) != 0)
    return false;
  const std::size_t authorityEnd = url.find_first_of("/?#", PREFIX.size());
  const std::string_view authority{url.data() + PREFIX.size(),
                                   (authorityEnd == std::string::npos ? url.size() : authorityEnd) -
                                       PREFIX.size()};
  return IsValidAuthority(authority) && url.find('\\') == std::string::npos &&
         std::none_of(url.begin(), url.end(),
                      [](unsigned char character) { return std::isspace(character) != 0; });
}

bool ParseCanonicalIdentity(const CVariant& value, JumpgateCanonicalIdentity& parsed)
{
  if (!HasOnlyKeys(value, {"provider", "id", "mediaType", "season", "episode", "confidence",
                           "provenance"}) ||
      !HasAllMembers(value, {"provider", "id", "mediaType", "confidence", "provenance"}) ||
      !IsIdentifier(value["provider"]) || !IsIdentifier(value["id"]) ||
      !value["mediaType"].isString() || !value["confidence"].isString() ||
      !value["provenance"].isString() || value["confidence"].asString() != "canonical" ||
      (value["provenance"].asString() != "metadata-request" &&
       value["provenance"].asString() != "verified-external-id"))
  {
    return false;
  }

  const std::string provider = value["provider"].asString();
  if (provider == "imdb")
    parsed.provider = JumpgateCanonicalProvider::Imdb;
  else if (provider == "tmdb")
    parsed.provider = JumpgateCanonicalProvider::Tmdb;
  else if (provider == "tvdb")
    parsed.provider = JumpgateCanonicalProvider::Tvdb;
  else if (provider == "trakt")
    parsed.provider = JumpgateCanonicalProvider::Trakt;
  else
    return false;

  parsed.id = value["id"].asString();
  if (parsed.provider == JumpgateCanonicalProvider::Imdb &&
      (parsed.id.size() < 9 || parsed.id.compare(0, 2, "tt") != 0 ||
       !std::all_of(parsed.id.begin() + 2, parsed.id.end(),
                    [](char character) { return character >= '0' && character <= '9'; })))
  {
    return false;
  }
  if (parsed.provider != JumpgateCanonicalProvider::Imdb)
  {
    uint64_t numericId = 0;
    const auto [end, parseError] =
        std::from_chars(parsed.id.data(), parsed.id.data() + parsed.id.size(), numericId);
    if (parseError != std::errc{} || end != parsed.id.data() + parsed.id.size() || numericId == 0 ||
        numericId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        parsed.id.front() == '0')
    {
      return false;
    }
  }

  const std::string mediaType = value["mediaType"].asString();
  if (mediaType == "movie")
  {
    parsed.mediaType = JumpgateMediaType::Movie;
    return (!value.isMember("season") || value["season"].isNull()) &&
           (!value.isMember("episode") || value["episode"].isNull());
  }
  if (mediaType != "episode" || !value.isMember("season") || !value.isMember("episode"))
    return false;

  parsed.season = ReadBoundedNonNegativeInteger(value["season"], std::numeric_limits<int>::max());
  parsed.episode = ReadBoundedNonNegativeInteger(value["episode"], std::numeric_limits<int>::max());
  if (!parsed.season || !parsed.episode)
    return false;
  parsed.mediaType = JumpgateMediaType::Episode;
  return true;
}

bool ReadOptionalDisplayInteger(const CVariant& display,
                                std::string_view key,
                                int maximum,
                                std::optional<int>& parsed)
{
  const std::string name{key};
  if (!display.isMember(name) || display[name].isNull())
    return true;
  parsed = ReadBoundedNonNegativeInteger(display[name], maximum);
  return parsed.has_value();
}

bool ReadOptionalDisplayString(const CVariant& display,
                               std::string_view key,
                               bool allowEmpty,
                               bool requireUrl,
                               std::optional<std::string>& parsed)
{
  const std::string name{key};
  if (!display.isMember(name) || display[name].isNull())
    return true;
  if ((requireUrl && !IsSafeRemoteUrl(display[name])) ||
      (!requireUrl &&
       !IsBoundedString(display[name], MAX_CONTEXT_STRING_LENGTH, allowEmpty, true, false)))
  {
    return false;
  }
  parsed = display[name].asString();
  return true;
}

bool ParseDisplay(const CVariant& value,
                  const std::optional<JumpgateCanonicalIdentity>& identity,
                  JumpgatePlaybackDisplay& parsed)
{
  if (!HasOnlyKeys(value, {"title", "year", "season", "episode", "poster", "background", "logo"}) ||
      !ReadOptionalDisplayString(value, "title", true, false, parsed.title) ||
      !ReadOptionalDisplayInteger(value, "year", 9999, parsed.year) ||
      !ReadOptionalDisplayInteger(value, "season", std::numeric_limits<int>::max(),
                                  parsed.season) ||
      !ReadOptionalDisplayInteger(value, "episode", std::numeric_limits<int>::max(),
                                  parsed.episode) ||
      !ReadOptionalDisplayString(value, "poster", false, true, parsed.poster) ||
      !ReadOptionalDisplayString(value, "background", false, true, parsed.background) ||
      !ReadOptionalDisplayString(value, "logo", false, true, parsed.logo))
  {
    return false;
  }

  if (!identity)
    return true;
  if (identity->mediaType == JumpgateMediaType::Movie)
    return !parsed.season && !parsed.episode;
  return (!parsed.season || parsed.season == identity->season) &&
         (!parsed.episode || parsed.episode == identity->episode);
}

bool ParseInlineSubtitles(const CVariant& value, std::vector<JumpgateInlineSubtitle>& parsed)
{
  if (!value.isArray() || value.size() > MAX_CONTEXT_ARRAY_LENGTH)
    return false;
  parsed.reserve(value.size());
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    // Subtitle addons may attach transport-specific fields such as headers or
    // tokens. Keep those fields out of the native projection, and do not let an
    // unusable optional subtitle invalidate an authenticated content identity.
    if (!item->isObject() || !item->isMember("url") || !IsSafeRemoteUrl((*item)["url"]))
      continue;

    JumpgateInlineSubtitle subtitle;
    subtitle.url = (*item)["url"].asString();
    if (item->isMember("id"))
    {
      if (!IsBoundedString((*item)["id"], MAX_IDENTIFIER_LENGTH, true, true, false))
        continue;
      subtitle.id = (*item)["id"].asString();
    }
    if (item->isMember("lang"))
    {
      if (!IsBoundedString((*item)["lang"], 64, true, true, true))
        continue;
      subtitle.language = (*item)["lang"].asString();
    }
    parsed.emplace_back(std::move(subtitle));
  }
  return true;
}

bool ValidateProvenanceObject(const CVariant& value,
                              std::initializer_list<std::string_view> allowedKeys,
                              std::string_view singularKey,
                              std::string_view pluralKey)
{
  if (!HasOnlyKeys(value, allowedKeys))
    return false;
  const std::string singular{singularKey};
  const std::string plural{pluralKey};
  std::optional<std::string> singularProvider;
  if (value.isMember(singular))
  {
    if (!IsIdentifier(value[singular]))
      return false;
    singularProvider = value[singular].asString();
  }
  if (value.isMember(plural))
  {
    if (!value[plural].isArray())
      return false;
    std::set<std::string> providers;
    for (auto item = value[plural].begin_array(); item != value[plural].end_array(); ++item)
    {
      if (!IsIdentifier(*item) || !providers.emplace(item->asString()).second)
        return false;
    }
    if (singularProvider && providers.find(*singularProvider) == providers.end())
      return false;
  }
  return true;
}

bool ValidateRequest(const CVariant& value)
{
  constexpr std::array<std::string_view, 5> STRING_KEYS = {"resource", "type", "metaId", "videoId",
                                                           "metaProvider"};
  if (!ValidateProvenanceObject(value,
                                {"resource", "type", "metaId", "videoId", "metaProvider",
                                 "streamProvider", "streamProviders"},
                                "streamProvider", "streamProviders"))
  {
    return false;
  }
  return std::all_of(STRING_KEYS.begin(), STRING_KEYS.end(),
                     [&value](std::string_view key)
                     {
                       const std::string name{key};
                       return !value.isMember(name) || IsIdentifier(value[name]);
                     });
}

bool ValidateSource(const CVariant& value)
{
  if (!ValidateProvenanceObject(value, {"type", "provider", "providers"}, "provider", "providers"))
  {
    return false;
  }
  return !value.isMember("type") || IsIdentifier(value["type"]);
}

bool ValidateFingerprints(const CVariant& value)
{
  if (!value.isArray() || value.empty() || value.size() > MAX_FINGERPRINTS)
    return false;
  std::set<std::string> fingerprints;
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    if (!IsBoundedString(*item, MAX_FINGERPRINT_LENGTH, false, true, false) ||
        !IsCanonicalFingerprint(item->asString()) || !fingerprints.emplace(item->asString()).second)
    {
      return false;
    }
  }
  return true;
}

bool IsLeapYear(int year)
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

std::int64_t DaysBeforeYear(int year)
{
  const std::int64_t previous = year - 1;
  return 365 * previous + previous / 4 - previous / 100 + previous / 400;
}

std::optional<std::int64_t> ParseCanonicalTimestamp(const CVariant& value)
{
  if (!IsBoundedString(value, 64, false, true, true))
    return std::nullopt;
  const std::string timestamp = value.asString();
  if (timestamp.size() != 24 || timestamp[4] != '-' || timestamp[7] != '-' ||
      timestamp[10] != 'T' || timestamp[13] != ':' || timestamp[16] != ':' ||
      timestamp[19] != '.' || timestamp[23] != 'Z')
  {
    return std::nullopt;
  }
  constexpr std::array<std::size_t, 7> DIGIT_OFFSETS = {0, 5, 8, 11, 14, 17, 20};
  constexpr std::array<std::size_t, 7> DIGIT_LENGTHS = {4, 2, 2, 2, 2, 2, 3};
  std::array<int, 7> parts{};
  for (std::size_t part = 0; part < parts.size(); ++part)
  {
    for (std::size_t index = 0; index < DIGIT_LENGTHS[part]; ++index)
    {
      const char character = timestamp[DIGIT_OFFSETS[part] + index];
      if (character < '0' || character > '9')
        return std::nullopt;
      parts[part] = parts[part] * 10 + character - '0';
    }
  }
  const int year = parts[0];
  const int month = parts[1];
  const int day = parts[2];
  if (year < 1970 || month < 1 || month > 12 || parts[3] > 23 || parts[4] > 59 || parts[5] > 59)
  {
    return std::nullopt;
  }
  constexpr std::array<int, 12> DAYS_PER_MONTH = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int maximumDay = DAYS_PER_MONTH[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
  if (day < 1 || day > maximumDay)
    return std::nullopt;

  std::int64_t days = DaysBeforeYear(year) - DaysBeforeYear(1970);
  for (int precedingMonth = 1; precedingMonth < month; ++precedingMonth)
  {
    days += DAYS_PER_MONTH[precedingMonth - 1];
    if (precedingMonth == 2 && IsLeapYear(year))
      ++days;
  }
  days += day - 1;
  return (((days * 24 + parts[3]) * 60 + parts[4]) * 60 + parts[5]) * 1000 + parts[6];
}

} // namespace

const char* ToString(JumpgateCanonicalProvider provider)
{
  switch (provider)
  {
    case JumpgateCanonicalProvider::Imdb:
      return "imdb";
    case JumpgateCanonicalProvider::Tmdb:
      return "tmdb";
    case JumpgateCanonicalProvider::Tvdb:
      return "tvdb";
    case JumpgateCanonicalProvider::Trakt:
      return "trakt";
  }
  return "";
}

const char* ToString(JumpgateMediaType mediaType)
{
  switch (mediaType)
  {
    case JumpgateMediaType::Movie:
      return "movie";
    case JumpgateMediaType::Episode:
      return "episode";
  }
  return "";
}

std::optional<JumpgatePlaybackContext> CJumpgatePlaybackContextParser::Parse(const CVariant& value)
{
  TreeBudget budget;
  if (!ValidateBoundedTree(value, budget, 0) ||
      !HasOnlyKeys(value, {"schemaVersion", "contextId", "profileId", "contentKey",
                           "canonicalIdentity", "traktEligible", "request", "display", "source",
                           "fingerprints", "inlineSubtitles", "createdAt", "expiresAt"}) ||
      !HasAllMembers(value, {"schemaVersion", "contextId", "profileId", "contentKey",
                             "canonicalIdentity", "traktEligible", "request", "display", "source",
                             "fingerprints", "inlineSubtitles", "createdAt", "expiresAt"}) ||
      !IsExactInteger(value["schemaVersion"], 1) || !IsIdentifier(value["contextId"]) ||
      !IsIdentifier(value["profileId"]) || !value["traktEligible"].isBoolean() ||
      !ValidateRequest(value["request"]) || !ValidateSource(value["source"]) ||
      !ValidateFingerprints(value["fingerprints"]))
  {
    return std::nullopt;
  }

  JumpgatePlaybackContext parsed;
  parsed.schemaVersion = 1;
  parsed.profileId = value["profileId"].asString();
  if (!value["contentKey"].isNull())
  {
    if (!value["contentKey"].isString() || !IsLowerHex(value["contentKey"].asString(), 64))
      return std::nullopt;
    parsed.contentKey = value["contentKey"].asString();
  }

  if (!value["canonicalIdentity"].isNull())
  {
    JumpgateCanonicalIdentity identity;
    if (!ParseCanonicalIdentity(value["canonicalIdentity"], identity))
      return std::nullopt;
    parsed.canonicalIdentity = std::move(identity);
  }
  parsed.traktEligible = value["traktEligible"].asBoolean();
  if (parsed.traktEligible && !parsed.canonicalIdentity)
    return std::nullopt;

  if (!ParseDisplay(value["display"], parsed.canonicalIdentity, parsed.display) ||
      !ParseInlineSubtitles(value["inlineSubtitles"], parsed.inlineSubtitles))
  {
    return std::nullopt;
  }

  const std::optional<std::int64_t> createdAt = ParseCanonicalTimestamp(value["createdAt"]);
  const std::optional<std::int64_t> expiresAt = ParseCanonicalTimestamp(value["expiresAt"]);
  if (!createdAt || !expiresAt || *expiresAt <= *createdAt ||
      *expiresAt - *createdAt > MAX_CONTEXT_TTL_MS)
  {
    return std::nullopt;
  }
  return parsed;
}

} // namespace KODI::JUMPGATE
