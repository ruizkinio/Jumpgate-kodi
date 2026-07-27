/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateSourceFingerprint.h"

#include "Digest.h"
#include "JSONVariantParser.h"
#include "JSONVariantWriter.h"
#include "URL.h"
#include "Variant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace KODI
{
namespace UTILITY
{
namespace
{

constexpr std::size_t MAX_EXACT_VALUE_LENGTH = 8192;
constexpr std::size_t MAX_FINGERPRINT_VALUE_LENGTH = 512;
constexpr std::size_t MAX_FINGERPRINTS = 32;
constexpr std::size_t MAX_CONTEXT_ARRAY_LENGTH = 64;
constexpr std::size_t MAX_CONTEXT_TOTAL_BYTES = 256 * 1024;
constexpr std::size_t MAX_PLAYBACK_URL_BYTES = 64 * 1024;
constexpr std::uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;
constexpr int MAX_FILE_INDEX = 65535;

constexpr std::array<std::pair<std::string_view, std::string_view>, 5> ARCHIVE_FIELDS{{
    {"rarUrls", "archive-rar"},
    {"zipUrls", "archive-zip"},
    {"7zipUrls", "archive-7zip"},
    {"tgzUrls", "archive-tgz"},
    {"tarUrls", "archive-tar"},
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 4> EXTERNAL_FIELDS{{
    {"externalUrl", "external-url"},
    {"androidTvUrl", "android-tv-url"},
    {"tizenUrl", "tizen-url"},
    {"webosUrl", "webos-url"},
}};

bool IsHex(char value)
{
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

bool IsLowerHex(std::string_view value)
{
  return std::all_of(value.begin(), value.end(), [](char item)
                     { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); });
}

std::string AsciiLower(std::string value)
{
  for (char& item : value)
  {
    if (item >= 'A' && item <= 'Z')
      item = static_cast<char>(item - 'A' + 'a');
  }
  return value;
}

bool Utf16Length(std::string_view value, std::size_t& units)
{
  units = 0;
  for (std::size_t index = 0; index < value.size();)
  {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    std::size_t addedUnits = 1;

    if (first <= 0x7f)
    {
      length = 1;
    }
    else if (first >= 0xc2 && first <= 0xdf)
    {
      length = 2;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
      length = 3;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
      length = 4;
      addedUnits = 2;
    }
    else
    {
      return false;
    }

    if (index + length > value.size())
      return false;

    for (std::size_t offset = 1; offset < length; ++offset)
    {
      if ((static_cast<unsigned char>(value[index + offset]) & 0xc0) != 0x80)
        return false;
    }

    if (length == 3)
    {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0))
        return false;
    }
    else if (length == 4)
    {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if ((first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))
        return false;
    }

    units += addedUnits;
    index += length;
  }
  return true;
}

bool IsBoundedString(std::string_view value,
                     std::size_t maxLength,
                     bool allowEmpty,
                     bool rejectControls,
                     bool requireTrimmed)
{
  if ((!allowEmpty && value.empty()) || value.size() > maxLength * 4)
    return false;

  std::size_t utf16Units = 0;
  if (!Utf16Length(value, utf16Units) || utf16Units > maxLength)
    return false;

  if (rejectControls && std::any_of(value.begin(), value.end(),
                                    [](char item)
                                    {
                                      const auto byte = static_cast<unsigned char>(item);
                                      return byte <= 0x1f || byte == 0x7f;
                                    }))
  {
    return false;
  }

  if (requireTrimmed && !value.empty())
  {
    const auto first = static_cast<unsigned char>(value.front());
    const auto last = static_cast<unsigned char>(value.back());
    if (first == ' ' || first == '\t' || first == '\r' || first == '\n' || last == ' ' ||
        last == '\t' || last == '\r' || last == '\n')
    {
      return false;
    }
  }
  return true;
}

bool ReadBoundedString(const CVariant& value,
                       std::size_t maxLength,
                       bool allowEmpty,
                       bool rejectControls,
                       bool requireTrimmed,
                       std::string& result)
{
  if (!value.isString())
    return false;

  result = value.asString();
  return IsBoundedString(result, maxLength, allowEmpty, rejectControls, requireTrimmed);
}

bool ReadOptionalExactString(const CVariant& object,
                             std::string_view key,
                             std::optional<std::string>& result)
{
  const std::string ownedKey{key};
  if (!object.isMember(ownedKey) || object[ownedKey].isNull())
  {
    result.reset();
    return true;
  }

  std::string value;
  if (!ReadBoundedString(object[ownedKey], MAX_EXACT_VALUE_LENGTH, false, true, false, value))
    return false;
  result = std::move(value);
  return true;
}

bool ReadSafeInteger(const CVariant& value, std::int64_t& result)
{
  if (value.isSignedInteger())
  {
    result = value.asInteger();
    return result >= -static_cast<std::int64_t>(MAX_SAFE_INTEGER) &&
           result <= static_cast<std::int64_t>(MAX_SAFE_INTEGER);
  }
  if (value.isUnsignedInteger())
  {
    const std::uint64_t unsignedValue = value.asUnsignedInteger();
    if (unsignedValue > MAX_SAFE_INTEGER)
      return false;
    result = static_cast<std::int64_t>(unsignedValue);
    return true;
  }
  if (value.isDouble())
  {
    const double doubleValue = value.asDouble();
    if (!std::isfinite(doubleValue) || std::floor(doubleValue) != doubleValue ||
        std::abs(doubleValue) > static_cast<double>(MAX_SAFE_INTEGER))
    {
      return false;
    }
    result = static_cast<std::int64_t>(doubleValue);
    return true;
  }
  return false;
}

bool ParseFileIndexString(std::string_view value, int& result)
{
  if (value == "-1")
  {
    result = -1;
    return true;
  }
  if (value.empty() || value.size() > 5 || (value.size() > 1 && value.front() == '0'))
    return false;

  unsigned int parsed = 0;
  for (char item : value)
  {
    if (item < '0' || item > '9')
      return false;
    parsed = parsed * 10 + static_cast<unsigned int>(item - '0');
  }
  if (parsed > MAX_FILE_INDEX)
    return false;
  result = static_cast<int>(parsed);
  return true;
}

bool NormalizeFileIndex(const CVariant& value, int& result)
{
  if (value.isString())
  {
    const std::string stringValue = value.asString();
    return ParseFileIndexString(stringValue, result);
  }

  std::int64_t integer = 0;
  if (!ReadSafeInteger(value, integer) || integer < -1 || integer > MAX_FILE_INDEX)
    return false;
  result = static_cast<int>(integer);
  return true;
}

bool NormalizeOptionalFileIndex(const CVariant& stream, std::optional<int>& result)
{
  if (!stream.isMember("fileIdx") || stream["fileIdx"].isNull())
  {
    result.reset();
    return true;
  }

  int fileIndex = 0;
  if (!NormalizeFileIndex(stream["fileIdx"], fileIndex))
    return false;
  result = fileIndex;
  return true;
}

bool NormalizeStringArray(const CVariant& object,
                          std::string_view key,
                          bool rejectExplicitEmpty,
                          std::vector<std::string>& result)
{
  result.clear();
  const std::string ownedKey{key};
  if (!object.isMember(ownedKey) || object[ownedKey].isNull())
    return true;

  const CVariant& value = object[ownedKey];
  if (!value.isArray() || value.size() > MAX_CONTEXT_ARRAY_LENGTH ||
      (rejectExplicitEmpty && value.empty()))
  {
    return false;
  }

  result.reserve(value.size());
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    std::string normalized;
    if (!ReadBoundedString(*item, MAX_EXACT_VALUE_LENGTH, false, true, false, normalized))
      return false;
    result.emplace_back(std::move(normalized));
  }
  return true;
}

CVariant ToStringArray(const std::vector<std::string>& values)
{
  CVariant result{CVariant::VariantTypeArray};
  for (const std::string& value : values)
    result.push_back(value);
  return result;
}

bool NormalizeArchiveUrls(const CVariant& value, CVariant& result)
{
  if (!value.isArray() || value.empty() || value.size() > MAX_CONTEXT_ARRAY_LENGTH)
    return false;

  result = CVariant{CVariant::VariantTypeArray};
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    if (!item->isArray() || item->size() < 1 || item->size() > 2)
      return false;

    std::string url;
    if (!ReadBoundedString((*item)[0], MAX_EXACT_VALUE_LENGTH, false, true, false, url))
      return false;

    CVariant normalizedEntry{CVariant::VariantTypeArray};
    normalizedEntry.push_back(url);
    if (item->size() == 2 && !(*item)[1].isNull())
    {
      std::int64_t bytes = 0;
      if (!ReadSafeInteger((*item)[1], bytes) || bytes < 0)
        return false;
      normalizedEntry.push_back(static_cast<std::uint64_t>(bytes));
    }
    result.push_back(std::move(normalizedEntry));
  }
  return true;
}

bool IsHeaderName(std::string_view value)
{
  if (!IsBoundedString(value, 256, false, true, true))
    return false;

  return std::all_of(value.begin(), value.end(),
                     [](char item)
                     {
                       const auto byte = static_cast<unsigned char>(item);
                       if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9'))
                       {
                         return true;
                       }
                       constexpr std::string_view TOKEN_PUNCTUATION{"!#$%&'*+-.^_`|~"};
                       return TOKEN_PUNCTUATION.find(item) != std::string_view::npos;
                     });
}

struct HeaderEntry
{
  std::string lowerName;
  std::string originalName;
  std::string value;
};

bool NormalizeHeaderMap(const CVariant& proxy, std::string_view key, CVariant& result)
{
  result = CVariant{CVariant::VariantTypeArray};
  const std::string ownedKey{key};
  if (!proxy.isMember(ownedKey) || proxy[ownedKey].isNull())
    return true;

  const CVariant& headers = proxy[ownedKey];
  if (!headers.isObject() || headers.size() > MAX_CONTEXT_ARRAY_LENGTH)
    return false;

  std::vector<HeaderEntry> entries;
  entries.reserve(headers.size());
  for (auto item = headers.begin_map(); item != headers.end_map(); ++item)
  {
    if (!IsHeaderName(item->first))
      return false;

    std::string headerValue;
    if (!ReadBoundedString(item->second, MAX_EXACT_VALUE_LENGTH, true, true, false, headerValue))
    {
      return false;
    }
    entries.push_back({AsciiLower(item->first), item->first, std::move(headerValue)});
  }

  std::sort(entries.begin(), entries.end(),
            [](const HeaderEntry& left, const HeaderEntry& right)
            {
              if (left.lowerName != right.lowerName)
                return left.lowerName < right.lowerName;
              return left.originalName < right.originalName;
            });

  for (std::size_t index = 1; index < entries.size(); ++index)
  {
    if (entries[index - 1].lowerName == entries[index].lowerName)
      return false;
  }

  for (const HeaderEntry& entry : entries)
  {
    CVariant pair{CVariant::VariantTypeArray};
    pair.push_back(entry.lowerName);
    pair.push_back(entry.value);
    result.push_back(std::move(pair));
  }
  return true;
}

bool NormalizeProxySource(const CVariant& stream,
                          const std::string& url,
                          std::optional<CVariant>& result)
{
  result.reset();
  if (!stream.isMember("behaviorHints") || stream["behaviorHints"].isNull())
    return true;

  const CVariant& behaviorHints = stream["behaviorHints"];
  if (!behaviorHints.isObject())
    return false;
  if (!behaviorHints.isMember("proxyHeaders") || behaviorHints["proxyHeaders"].isNull())
    return true;

  const CVariant& proxy = behaviorHints["proxyHeaders"];
  if (!proxy.isObject())
    return false;

  CVariant request;
  CVariant response;
  if (!NormalizeHeaderMap(proxy, "request", request) ||
      !NormalizeHeaderMap(proxy, "response", response))
  {
    return false;
  }

  CVariant source{CVariant::VariantTypeObject};
  source["url"] = url;
  source["request"] = std::move(request);
  source["response"] = std::move(response);
  result = std::move(source);
  return true;
}

std::string Sha256(std::string_view value)
{
  return CDigest::Calculate(CDigest::Type::SHA256, value.data(), value.size());
}

bool FingerprintTypedValue(std::string_view type, std::string_view value, std::string& fingerprint)
{
  if (!IsBoundedString(value, MAX_EXACT_VALUE_LENGTH, false, true, false))
    return false;
  fingerprint = "v1:" + std::string{type} + ":sha256:" + Sha256(value);
  return true;
}

bool FingerprintCanonicalSource(std::string_view type,
                                const CVariant& source,
                                std::string& fingerprint)
{
  std::string serialized;
  if (!CJSONVariantWriter::Write(source, serialized, true) ||
      serialized.size() > MAX_CONTEXT_TOTAL_BYTES)
  {
    return false;
  }
  fingerprint = "v1:" + std::string{type} + ":sha256:" + Sha256(serialized);
  return true;
}

std::string FingerprintTorrentFilters(const std::vector<std::string>& filters)
{
  constexpr char FINGERPRINT_DOMAIN[] = "jumpgate-info-hash-file-must-include:v1\0";
  CDigest digest{CDigest::Type::SHA256};
  digest.Update(FINGERPRINT_DOMAIN, sizeof(FINGERPRINT_DOMAIN) - 1);
  for (const std::string& filter : filters)
  {
    digest.Update(std::to_string(filter.size()));
    digest.Update(":");
    digest.Update(filter);
    constexpr char TERMINATOR = '\0';
    digest.Update(&TERMINATOR, 1);
  }
  return digest.Finalize();
}

bool IsCanonicalFingerprint(std::string_view value)
{
  constexpr std::array<std::string_view, 14> TYPES{{
      "url",
      "external-url",
      "android-tv-url",
      "tizen-url",
      "webos-url",
      "player-frame-url",
      "yt-id",
      "proxy-source",
      "archive-rar",
      "archive-zip",
      "archive-7zip",
      "archive-tgz",
      "archive-tar",
      "nzb-source",
  }};

  for (std::string_view type : TYPES)
  {
    const std::string prefix = "v1:" + std::string{type} + ":sha256:";
    if (value.starts_with(prefix))
    {
      const std::string_view digest = value.substr(prefix.size());
      return digest.size() == 64 && IsLowerHex(digest);
    }
  }

  constexpr std::string_view OPAQUE_PREFIX{"v1:opaque:sha256:"};
  if (value.starts_with(OPAQUE_PREFIX))
  {
    const std::string_view digest = value.substr(OPAQUE_PREFIX.size());
    return digest.size() == 64 && IsLowerHex(digest);
  }

  constexpr std::string_view TORRENT_PREFIX{"v1:info-hash:"};
  constexpr std::string_view FILE_INDEX_MARKER{":file-idx:"};
  constexpr std::string_view FILTER_MARKER{":file-must-include:sha256:"};
  if (!value.starts_with(TORRENT_PREFIX) ||
      value.size() < TORRENT_PREFIX.size() + 40 + FILE_INDEX_MARKER.size() + 1)
  {
    return false;
  }

  const std::string_view infoHash = value.substr(TORRENT_PREFIX.size(), 40);
  if (!IsLowerHex(infoHash) ||
      value.substr(TORRENT_PREFIX.size() + 40, FILE_INDEX_MARKER.size()) != FILE_INDEX_MARKER)
  {
    return false;
  }

  std::string_view selector = value.substr(TORRENT_PREFIX.size() + 40 + FILE_INDEX_MARKER.size());
  const std::size_t filterAt = selector.find(FILTER_MARKER);
  const std::string_view indexValue = selector.substr(0, filterAt);
  int fileIndex = 0;
  if (!ParseFileIndexString(indexValue, fileIndex) || std::to_string(fileIndex) != indexValue)
    return false;
  if (filterAt == std::string_view::npos)
    return true;

  if (fileIndex != -1)
    return false;
  const std::string_view digest = selector.substr(filterAt + FILTER_MARKER.size());
  return digest.size() == 64 && IsLowerHex(digest);
}

void AppendUnique(std::vector<std::string>& values, std::string value)
{
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.emplace_back(std::move(value));
}

bool FingerprintStreamInternal(const CVariant& stream,
                               const std::vector<std::string>& extraFingerprints,
                               std::vector<std::string>& fingerprints)
{
  fingerprints.clear();
  if (!stream.isObject() || extraFingerprints.size() > MAX_FINGERPRINTS)
    return false;

  std::optional<std::string> url;
  if (!ReadOptionalExactString(stream, "url", url))
    return false;

  if (url)
  {
    std::string fingerprint;
    if (!FingerprintTypedValue("url", *url, fingerprint))
      return false;
    AppendUnique(fingerprints, std::move(fingerprint));

    std::optional<CVariant> proxySource;
    if (!NormalizeProxySource(stream, *url, proxySource))
      return false;
    if (proxySource)
    {
      if (!FingerprintCanonicalSource("proxy-source", *proxySource, fingerprint))
        return false;
      AppendUnique(fingerprints, std::move(fingerprint));
    }
  }
  else if (stream.isMember("ytId"))
  {
    std::optional<std::string> ytId;
    std::string fingerprint;
    if (!ReadOptionalExactString(stream, "ytId", ytId) || !ytId ||
        !FingerprintTypedValue("yt-id", *ytId, fingerprint))
    {
      return false;
    }
    AppendUnique(fingerprints, std::move(fingerprint));
  }
  else
  {
    const auto archive =
        std::find_if(ARCHIVE_FIELDS.begin(), ARCHIVE_FIELDS.end(), [&stream](const auto& item)
                     { return stream.isMember(std::string{item.first}); });
    if (archive != ARCHIVE_FIELDS.end())
    {
      CVariant urls;
      std::optional<int> fileIndex;
      std::vector<std::string> filters;
      if (!NormalizeArchiveUrls(stream[std::string{archive->first}], urls) ||
          !NormalizeOptionalFileIndex(stream, fileIndex) ||
          !NormalizeStringArray(stream, "fileMustInclude", false, filters))
      {
        return false;
      }

      CVariant source{CVariant::VariantTypeObject};
      source["urls"] = std::move(urls);
      source["fileIdx"] = fileIndex ? CVariant{*fileIndex} : CVariant{};
      source["fileMustInclude"] = ToStringArray(filters);

      std::string fingerprint;
      if (!FingerprintCanonicalSource(archive->second, source, fingerprint))
        return false;
      AppendUnique(fingerprints, std::move(fingerprint));
    }
    else if (stream.isMember("nzbUrl") || stream.isMember("nzbUrls") || stream.isMember("servers"))
    {
      std::optional<std::string> nzbUrl;
      std::vector<std::string> nzbUrls;
      std::vector<std::string> servers;
      std::optional<int> fileIndex;
      std::vector<std::string> filters;
      if (!ReadOptionalExactString(stream, "nzbUrl", nzbUrl) ||
          !NormalizeStringArray(stream, "nzbUrls", false, nzbUrls) ||
          !NormalizeStringArray(stream, "servers", true, servers) ||
          !NormalizeOptionalFileIndex(stream, fileIndex) ||
          !NormalizeStringArray(stream, "fileMustInclude", false, filters) ||
          (!nzbUrl && nzbUrls.empty()))
      {
        return false;
      }

      CVariant source{CVariant::VariantTypeObject};
      source["nzbUrl"] = nzbUrl ? CVariant{*nzbUrl} : CVariant{};
      source["nzbUrls"] = ToStringArray(nzbUrls);
      source["servers"] = ToStringArray(servers);
      source["fileIdx"] = fileIndex ? CVariant{*fileIndex} : CVariant{};
      source["fileMustInclude"] = ToStringArray(filters);

      std::string fingerprint;
      if (!FingerprintCanonicalSource("nzb-source", source, fingerprint))
        return false;
      AppendUnique(fingerprints, std::move(fingerprint));
    }
    else if (stream.isMember("infoHash"))
    {
      std::string infoHash;
      if (!ReadBoundedString(stream["infoHash"], 40, false, true, true, infoHash) ||
          infoHash.size() != 40 || !std::all_of(infoHash.begin(), infoHash.end(), IsHex))
      {
        return false;
      }

      std::optional<int> selectedFileIndex;
      if (!NormalizeOptionalFileIndex(stream, selectedFileIndex))
        return false;
      const int fileIndex = selectedFileIndex.value_or(-1);
      std::string selector = ":file-idx:" + std::to_string(fileIndex);
      if (fileIndex == -1)
      {
        std::vector<std::string> filters;
        if (!NormalizeStringArray(stream, "fileMustInclude", false, filters))
          return false;
        if (!filters.empty())
          selector += ":file-must-include:sha256:" + FingerprintTorrentFilters(filters);
      }
      AppendUnique(fingerprints, "v1:info-hash:" + AsciiLower(infoHash) + selector);
    }
    else if (stream.isMember("playerFrameUrl"))
    {
      std::optional<std::string> playerFrameUrl;
      std::string fingerprint;
      if (!ReadOptionalExactString(stream, "playerFrameUrl", playerFrameUrl) || !playerFrameUrl ||
          !FingerprintTypedValue("player-frame-url", *playerFrameUrl, fingerprint))
      {
        return false;
      }
      AppendUnique(fingerprints, std::move(fingerprint));
    }
    else
    {
      for (const auto& [field, type] : EXTERNAL_FIELDS)
      {
        std::optional<std::string> value;
        std::string fingerprint;
        if (!ReadOptionalExactString(stream, field, value))
          return false;
        if (value)
        {
          if (!FingerprintTypedValue(type, *value, fingerprint))
            return false;
          AppendUnique(fingerprints, std::move(fingerprint));
        }
      }
    }
  }

  if (fingerprints.empty() && stream.isMember("fileIdx") && !stream["fileIdx"].isNull())
    return false;
  if (fingerprints.size() > MAX_FINGERPRINTS)
    return false;

  for (const std::string& provided : extraFingerprints)
  {
    if (!IsBoundedString(provided, MAX_FINGERPRINT_VALUE_LENGTH, false, true, false))
      return false;

    if (IsCanonicalFingerprint(provided))
    {
      AppendUnique(fingerprints, provided);
    }
    else
    {
      std::string opaque;
      if (!FingerprintTypedValue("opaque", provided, opaque))
        return false;
      AppendUnique(fingerprints, std::move(opaque));
    }
  }
  return !fingerprints.empty() && fingerprints.size() <= MAX_FINGERPRINTS;
}

bool HasValidPercentEncoding(std::string_view value)
{
  for (std::size_t index = 0; index < value.size(); ++index)
  {
    if (value[index] == '%')
    {
      if (index + 2 >= value.size() || !IsHex(value[index + 1]) || !IsHex(value[index + 2]))
        return false;
      index += 2;
    }
  }
  return true;
}

bool PercentDecode(std::string_view value, bool formEncoded, std::string& result)
{
  if (!HasValidPercentEncoding(value))
    return false;

  std::string prepared{value};
  if (formEncoded)
    std::replace(prepared.begin(), prepared.end(), '+', ' ');
  result = CURL::Decode(prepared);
  return IsBoundedString(result, MAX_PLAYBACK_URL_BYTES, true, true, false);
}

bool ParseForm(std::string_view query,
               bool allowEmpty,
               std::vector<std::pair<std::string, std::string>>& pairs)
{
  pairs.clear();
  if (query.empty())
    return allowEmpty;

  std::size_t start = 0;
  while (start <= query.size())
  {
    const std::size_t end = query.find('&', start);
    const std::string_view pair = query.substr(start, end - start);
    const std::size_t equals = pair.find('=');
    if (pair.empty() || equals == std::string_view::npos)
      return false;

    std::string key;
    std::string value;
    if (!PercentDecode(pair.substr(0, equals), true, key) || key.empty() ||
        !PercentDecode(pair.substr(equals + 1), true, value))
    {
      return false;
    }
    pairs.emplace_back(std::move(key), std::move(value));
    if (pairs.size() > MAX_CONTEXT_ARRAY_LENGTH * 3)
      return false;

    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

struct ParsedUrl
{
  std::string protocol;
  std::string host;
  std::string path;
  std::string query;
  unsigned int port{0};
  bool bracketedHost{false};
  bool hasUserInfo{false};
  bool hasPort{false};
  bool hasQuery{false};
  bool hasFragment{false};
};

bool ParsePort(std::string_view value, unsigned int& port)
{
  port = 0;
  if (value.empty() || value.size() > 5)
    return false;
  for (char item : value)
  {
    if (item < '0' || item > '9')
      return false;
    port = port * 10 + static_cast<unsigned int>(item - '0');
  }
  return port > 0 && port <= 65535;
}

bool ParseAuthority(std::string_view authority, ParsedUrl& result)
{
  if (authority.empty() || authority.find('\\') != std::string_view::npos)
    return false;

  const std::size_t at = authority.find('@');
  if (at != authority.rfind('@'))
    return false;
  result.hasUserInfo = at != std::string_view::npos;
  std::string_view hostAndPort =
      at == std::string_view::npos ? authority : authority.substr(at + 1);
  if (hostAndPort.empty())
    return false;

  std::string_view encodedHost;
  std::string_view port;
  if (hostAndPort.front() == '[')
  {
    const std::size_t close = hostAndPort.find(']');
    if (close == std::string_view::npos || close == 1 || close != hostAndPort.rfind(']'))
      return false;
    encodedHost = hostAndPort.substr(1, close - 1);
    result.bracketedHost = true;
    const std::string_view remainder = hostAndPort.substr(close + 1);
    if (!remainder.empty())
    {
      if (!remainder.starts_with(':'))
        return false;
      port = remainder.substr(1);
      result.hasPort = true;
    }
  }
  else
  {
    if (hostAndPort.find_first_of("[]") != std::string_view::npos)
      return false;
    const std::size_t colon = hostAndPort.rfind(':');
    if (colon != std::string_view::npos)
    {
      if (colon != hostAndPort.find(':') || colon == 0)
        return false;
      port = hostAndPort.substr(colon + 1);
      hostAndPort = hostAndPort.substr(0, colon);
      result.hasPort = true;
    }
    if (hostAndPort.empty())
      return false;
    encodedHost = hostAndPort;
  }

  if (result.hasPort && !ParsePort(port, result.port))
    return false;

  if (!PercentDecode(encodedHost, false, result.host) || result.host.empty() ||
      result.host.find_first_of("/?#@[]") != std::string::npos ||
      (!result.bracketedHost && result.host.find(':') != std::string::npos))
  {
    return false;
  }
  result.host = AsciiLower(std::move(result.host));
  return true;
}

bool IsValidScheme(std::string_view value)
{
  if (value.empty() || !((value.front() >= 'A' && value.front() <= 'Z') ||
                         (value.front() >= 'a' && value.front() <= 'z')))
  {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(),
                     [](char item)
                     {
                       return (item >= 'A' && item <= 'Z') || (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9') || item == '+' || item == '-' ||
                              item == '.';
                     });
}

bool ParseUrl(std::string_view raw, std::size_t maxBytes, ParsedUrl& result)
{
  if (raw.empty() || raw.size() > maxBytes || raw.find('\\') != std::string_view::npos ||
      !HasValidPercentEncoding(raw) || !IsBoundedString(raw, maxBytes, false, true, false))
  {
    return false;
  }

  const std::size_t schemeEnd = raw.find("://");
  if (schemeEnd == std::string_view::npos || !IsValidScheme(raw.substr(0, schemeEnd)))
    return false;

  const std::size_t fragmentAt = raw.find('#', schemeEnd + 3);
  if (fragmentAt != std::string_view::npos &&
      raw.find('#', fragmentAt + 1) != std::string_view::npos)
    return false;
  const std::size_t contentEnd = fragmentAt == std::string_view::npos ? raw.size() : fragmentAt;
  const std::size_t authorityStart = schemeEnd + 3;
  std::size_t authorityEnd = raw.find_first_of("/?", authorityStart);
  if (authorityEnd == std::string_view::npos || authorityEnd > contentEnd)
    authorityEnd = contentEnd;

  result = {};
  result.protocol = AsciiLower(std::string{raw.substr(0, schemeEnd)});
  result.hasFragment = fragmentAt != std::string_view::npos;
  if (!ParseAuthority(raw.substr(authorityStart, authorityEnd - authorityStart), result))
    return false;
  if (authorityEnd == contentEnd)
    return true;

  if (raw[authorityEnd] == '?')
  {
    result.hasQuery = true;
    result.query = std::string{raw.substr(authorityEnd + 1, contentEnd - authorityEnd - 1)};
    return true;
  }

  std::size_t queryAt = raw.find('?', authorityEnd + 1);
  if (queryAt == std::string_view::npos || queryAt > contentEnd)
    queryAt = contentEnd;
  result.path = std::string{raw.substr(authorityEnd + 1, queryAt - authorityEnd - 1)};
  if (queryAt < contentEnd)
  {
    result.hasQuery = true;
    result.query = std::string{raw.substr(queryAt + 1, contentEnd - queryAt - 1)};
  }
  return true;
}

bool ParseIpv4(std::string_view host, std::array<unsigned int, 4>& octets)
{
  if (host.ends_with('.') && !host.empty())
    host.remove_suffix(1);
  std::size_t start = 0;
  for (std::size_t index = 0; index < octets.size(); ++index)
  {
    const std::size_t end = host.find('.', start);
    const bool isLast = index + 1 == octets.size();
    if ((isLast && end != std::string_view::npos) || (!isLast && end == std::string_view::npos))
      return false;

    const std::string_view value = host.substr(start, end - start);
    if (value.empty() || value.size() > 3 || (value.size() > 1 && value.front() == '0'))
      return false;

    unsigned int parsed = 0;
    for (char item : value)
    {
      if (item < '0' || item > '9')
        return false;
      parsed = parsed * 10 + static_cast<unsigned int>(item - '0');
    }
    if (parsed > 255)
      return false;
    octets[index] = parsed;
    if (!isLast)
      start = end + 1;
  }
  return true;
}

bool ParseIpv6Part(std::string_view part, std::vector<unsigned int>& words)
{
  if (part.empty())
    return true;
  std::size_t start = 0;
  while (start <= part.size())
  {
    const std::size_t end = part.find(':', start);
    const std::string_view token = part.substr(start, end - start);
    if (token.empty())
      return false;

    if (token.find('.') != std::string_view::npos)
    {
      if (end != std::string_view::npos)
        return false;
      std::array<unsigned int, 4> octets{};
      if (!ParseIpv4(token, octets))
        return false;
      words.emplace_back((octets[0] << 8) | octets[1]);
      words.emplace_back((octets[2] << 8) | octets[3]);
    }
    else
    {
      if (token.size() > 4)
        return false;
      unsigned int word = 0;
      for (char item : token)
      {
        if (!IsHex(item))
          return false;
        word *= 16;
        if (item >= '0' && item <= '9')
          word += static_cast<unsigned int>(item - '0');
        else if (item >= 'a' && item <= 'f')
          word += static_cast<unsigned int>(item - 'a' + 10);
        else
          word += static_cast<unsigned int>(item - 'A' + 10);
      }
      words.emplace_back(word);
    }

    if (words.size() > 8 || end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return words.size() <= 8;
}

bool ParseIpv6(std::string_view host, std::array<unsigned int, 8>& words)
{
  const std::size_t compression = host.find("::");
  if (compression != std::string_view::npos &&
      host.find("::", compression + 2) != std::string_view::npos)
    return false;

  std::vector<unsigned int> left;
  std::vector<unsigned int> right;
  if (compression == std::string_view::npos)
  {
    if (!ParseIpv6Part(host, left) || left.size() != words.size())
      return false;
    std::copy(left.begin(), left.end(), words.begin());
    return true;
  }

  if (!ParseIpv6Part(host.substr(0, compression), left) ||
      !ParseIpv6Part(host.substr(compression + 2), right) ||
      left.size() + right.size() >= words.size())
  {
    return false;
  }

  words.fill(0);
  std::copy(left.begin(), left.end(), words.begin());
  std::copy(right.begin(), right.end(), words.end() - right.size());
  return true;
}

enum class HostKind
{
  REMOTE,
  LOOPBACK,
  UNSAFE,
};

HostKind ClassifyHost(const ParsedUrl& parsed)
{
  if (parsed.bracketedHost)
  {
    std::array<unsigned int, 8> words{};
    if (!ParseIpv6(parsed.host, words))
      return HostKind::UNSAFE;

    const bool ipv6Loopback =
        std::all_of(words.begin(), words.end() - 1, [](unsigned int word) { return word == 0; }) &&
        words.back() == 1;
    const bool mappedIpv4Loopback = std::all_of(words.begin(), words.begin() + 5,
                                                [](unsigned int word) { return word == 0; }) &&
                                    words[5] == 0xffff && (words[6] >> 8) == 127;
    const bool compatibleIpv4Loopback = std::all_of(words.begin(), words.begin() + 6,
                                                    [](unsigned int word) { return word == 0; }) &&
                                        (words[6] >> 8) == 127;
    return ipv6Loopback || mappedIpv4Loopback || compatibleIpv4Loopback ? HostKind::LOOPBACK
                                                                        : HostKind::REMOTE;
  }

  std::string_view host = parsed.host;
  if (host.ends_with('.') && !host.empty())
    host.remove_suffix(1);
  if (host == "localhost")
    return HostKind::LOOPBACK;

  std::array<unsigned int, 4> octets{};
  if (ParseIpv4(host, octets))
    return octets[0] == 127 ? HostKind::LOOPBACK : HostKind::REMOTE;

  if (host.empty() || host.front() == '.' || host.back() == '.' ||
      host.find("..") != std::string_view::npos)
  {
    return HostKind::UNSAFE;
  }

  const bool numericLookalike = host.front() >= '0' && host.front() <= '9' &&
                                std::all_of(host.begin(), host.end(),
                                            [](char item)
                                            {
                                              return (item >= '0' && item <= '9') ||
                                                     (item >= 'a' && item <= 'f') ||
                                                     (item >= 'A' && item <= 'F') || item == 'x' ||
                                                     item == 'X' || item == '.';
                                            });
  return numericLookalike ? HostKind::UNSAFE : HostKind::REMOTE;
}

bool IsSupportedDirectProtocol(std::string_view protocol)
{
  constexpr std::array<std::string_view, 7> SUPPORTED{{
      "http",
      "https",
      "ftp",
      "ftps",
      "rtmp",
      "rtmps",
      "rtsp",
  }};
  return std::find(SUPPORTED.begin(), SUPPORTED.end(), protocol) != SUPPORTED.end();
}

int LzAlphabetValue(char item)
{
  if (item >= 'A' && item <= 'Z')
    return item - 'A';
  if (item >= 'a' && item <= 'z')
    return item - 'a' + 26;
  if (item >= '0' && item <= '9')
    return item - '0' + 52;
  if (item == '+' || item == ' ')
    return 62;
  if (item == '-')
    return 63;
  return -1;
}

class LzBitReader
{
public:
  explicit LzBitReader(std::string_view input) : m_input(input)
  {
    if (!m_input.empty())
      m_value = LzAlphabetValue(m_input[0]);
  }

  bool IsValid() const { return !m_input.empty() && m_value >= 0; }

  bool Read(unsigned int count, std::uint32_t& result)
  {
    result = 0;
    std::uint32_t power = 1;
    for (unsigned int bit = 0; bit < count; ++bit)
    {
      if ((m_value & m_position) != 0)
        result |= power;
      power <<= 1;
      m_position >>= 1;
      if (m_position == 0)
      {
        m_position = 32;
        if (m_index >= m_input.size())
          return false;
        m_value = LzAlphabetValue(m_input[m_index++]);
        if (m_value < 0)
          return false;
      }
    }
    return true;
  }

private:
  std::string_view m_input;
  std::size_t m_index{1};
  int m_value{-1};
  int m_position{32};
};

bool Utf16ToUtf8(const std::u16string& input, std::string& output)
{
  output.clear();
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index)
  {
    std::uint32_t codePoint = input[index];
    if (codePoint >= 0xd800 && codePoint <= 0xdbff)
    {
      if (++index >= input.size())
        return false;
      const std::uint32_t low = input[index];
      if (low < 0xdc00 || low > 0xdfff)
        return false;
      codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
    }
    else if (codePoint >= 0xdc00 && codePoint <= 0xdfff)
    {
      return false;
    }

    if (codePoint <= 0x7f)
    {
      output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7ff)
    {
      output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    else if (codePoint <= 0xffff)
    {
      output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    else
    {
      output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    if (output.size() > MAX_CONTEXT_TOTAL_BYTES)
      return false;
  }
  return true;
}

bool LzDecompress(std::string_view input, std::string& output)
{
  LzBitReader reader{input};
  if (!reader.IsValid())
    return false;

  std::uint32_t token = 0;
  if (!reader.Read(2, token))
    return false;

  std::uint32_t character = 0;
  if (token == 0)
  {
    if (!reader.Read(8, character))
      return false;
  }
  else if (token == 1)
  {
    if (!reader.Read(16, character))
      return false;
  }
  else if (token == 2)
  {
    output.clear();
    return true;
  }
  else
  {
    return false;
  }

  std::vector<std::u16string> dictionary(4);
  dictionary[3] = std::u16string(1, static_cast<char16_t>(character));
  std::u16string previous = dictionary[3];
  std::u16string decompressed = previous;
  std::size_t dictionarySize = 4;
  std::uint64_t enlargeIn = 4;
  unsigned int numberOfBits = 3;

  while (true)
  {
    std::uint32_t code = 0;
    if (!reader.Read(numberOfBits, code))
      return false;

    if (code == 0 || code == 1)
    {
      if (!reader.Read(code == 0 ? 8 : 16, character))
        return false;
      dictionary.emplace_back(1, static_cast<char16_t>(character));
      code = static_cast<std::uint32_t>(dictionarySize++);
      if (enlargeIn == 0)
        return false;
      --enlargeIn;
    }
    else if (code == 2)
    {
      return Utf16ToUtf8(decompressed, output);
    }

    if (enlargeIn == 0)
    {
      if (numberOfBits >= 24)
        return false;
      enlargeIn = std::uint64_t{1} << numberOfBits++;
    }

    std::u16string entry;
    if (code < dictionary.size() && !dictionary[code].empty())
    {
      entry = dictionary[code];
    }
    else if (code == dictionarySize && !previous.empty())
    {
      entry = previous + previous.front();
    }
    else
    {
      return false;
    }

    if (decompressed.size() + entry.size() > MAX_CONTEXT_TOTAL_BYTES)
      return false;
    decompressed += entry;
    dictionary.emplace_back(previous + entry.front());
    ++dictionarySize;
    if (dictionarySize > MAX_CONTEXT_TOTAL_BYTES || enlargeIn == 0)
      return false;
    --enlargeIn;
    previous = std::move(entry);

    if (enlargeIn == 0)
    {
      if (numberOfBits >= 24)
        return false;
      enlargeIn = std::uint64_t{1} << numberOfBits++;
    }
  }
}

bool HasOnlyMembers(const CVariant& object, std::initializer_list<std::string_view> allowedMembers)
{
  if (!object.isObject())
    return false;
  for (auto item = object.begin_map(); item != object.end_map(); ++item)
  {
    if (std::none_of(allowedMembers.begin(), allowedMembers.end(),
                     [&item](std::string_view key) { return item->first == key; }))
    {
      return false;
    }
  }
  return true;
}

bool FingerprintCreateRoute(std::string_view type,
                            const ParsedUrl& parsed,
                            std::vector<std::string>& fingerprints)
{
  std::vector<std::pair<std::string, std::string>> query;
  if (!parsed.hasQuery || !ParseForm(parsed.query, false, query) || query.size() != 1 ||
      query[0].first != "lz" || query[0].second.empty())
  {
    return false;
  }

  std::string json;
  CVariant payload;
  if (!LzDecompress(query[0].second, json) || json.empty() ||
      !CJSONVariantParser::Parse(json, payload) || !payload.isObject())
  {
    return false;
  }

  if (type == "nzb")
  {
    if (!HasOnlyMembers(payload, {"nzbUrl", "nzbUrls", "servers", "fileIdx", "fileMustInclude"}))
      return false;
    return FingerprintStreamInternal(payload, {}, fingerprints);
  }

  if (!HasOnlyMembers(payload, {"urls", "fileIdx", "fileMustInclude"}) || !payload.isMember("urls"))
  {
    return false;
  }

  CVariant stream{CVariant::VariantTypeObject};
  stream[std::string{type} + "Urls"] = payload["urls"];
  if (payload.isMember("fileIdx"))
    stream["fileIdx"] = payload["fileIdx"];
  if (payload.isMember("fileMustInclude"))
    stream["fileMustInclude"] = payload["fileMustInclude"];
  return FingerprintStreamInternal(stream, {}, fingerprints);
}

bool FingerprintProxyRoute(const ParsedUrl& parsed, std::vector<std::string>& fingerprints)
{
  constexpr std::string_view PREFIX{"proxy/"};
  const std::string_view remainder = std::string_view{parsed.path}.substr(PREFIX.size());
  const std::size_t pathAt = remainder.find('/');
  if (pathAt == std::string_view::npos || pathAt + 1 == remainder.size())
    return false;

  std::vector<std::pair<std::string, std::string>> routeValues;
  if (!ParseForm(remainder.substr(0, pathAt), false, routeValues))
    return false;

  std::optional<std::string> origin;
  CVariant request{CVariant::VariantTypeObject};
  CVariant response{CVariant::VariantTypeObject};
  for (const auto& [key, value] : routeValues)
  {
    if (key == "d")
    {
      if (origin || value.empty())
        return false;
      origin = value;
      continue;
    }
    if (key != "h" && key != "r")
      return false;

    const std::size_t colon = value.find(':');
    if (colon == std::string::npos)
      return false;
    const std::string name = value.substr(0, colon);
    const std::string headerValue = value.substr(colon + 1);
    if (!IsHeaderName(name) ||
        !IsBoundedString(headerValue, MAX_EXACT_VALUE_LENGTH, true, true, false))
    {
      return false;
    }
    CVariant& headers = key == "h" ? request : response;
    const std::string lowerName = AsciiLower(name);
    if (headers.isMember(lowerName))
      return false;
    headers[lowerName] = headerValue;
  }

  ParsedUrl parsedOrigin;
  if (!origin || origin->ends_with('/') ||
      !ParseUrl(*origin, MAX_EXACT_VALUE_LENGTH * 4, parsedOrigin) ||
      (parsedOrigin.protocol != "http" && parsedOrigin.protocol != "https") ||
      ClassifyHost(parsedOrigin) != HostKind::REMOTE || !parsedOrigin.path.empty() ||
      parsedOrigin.hasQuery || parsedOrigin.hasFragment)
  {
    return false;
  }

  std::string sourceUrl = *origin + "/" + std::string{remainder.substr(pathAt + 1)};
  if (parsed.hasQuery)
    sourceUrl += "?" + parsed.query;

  CVariant stream{CVariant::VariantTypeObject};
  stream["url"] = std::move(sourceUrl);
  stream["behaviorHints"] = CVariant{CVariant::VariantTypeObject};
  stream["behaviorHints"]["proxyHeaders"] = CVariant{CVariant::VariantTypeObject};
  stream["behaviorHints"]["proxyHeaders"]["request"] = std::move(request);
  stream["behaviorHints"]["proxyHeaders"]["response"] = std::move(response);
  return FingerprintStreamInternal(stream, {}, fingerprints);
}

bool FingerprintTorrentRoute(const ParsedUrl& parsed, std::vector<std::string>& fingerprints)
{
  const std::size_t slash = parsed.path.find('/');
  if (slash == std::string::npos || parsed.path.find('/', slash + 1) != std::string::npos)
    return false;

  const std::string infoHash = parsed.path.substr(0, slash);
  const std::string fileIndexValue = parsed.path.substr(slash + 1);
  int fileIndex = 0;
  if (infoHash.size() != 40 || !std::all_of(infoHash.begin(), infoHash.end(), IsHex) ||
      !ParseFileIndexString(fileIndexValue, fileIndex))
  {
    return false;
  }

  std::vector<std::string> filters;
  if (parsed.hasQuery)
  {
    std::vector<std::pair<std::string, std::string>> query;
    if (!ParseForm(parsed.query, true, query))
      return false;
    for (const auto& [key, value] : query)
    {
      if (key == "f")
      {
        if (!IsBoundedString(value, MAX_EXACT_VALUE_LENGTH, false, true, false) ||
            filters.size() >= MAX_CONTEXT_ARRAY_LENGTH)
        {
          return false;
        }
        filters.emplace_back(value);
      }
      else if (key != "tr" || !IsBoundedString(value, MAX_EXACT_VALUE_LENGTH, true, true, false))
      {
        return false;
      }
    }
  }

  CVariant stream{CVariant::VariantTypeObject};
  stream["infoHash"] = infoHash;
  stream["fileIdx"] = fileIndex;
  if (!filters.empty())
    stream["fileMustInclude"] = ToStringArray(filters);
  return FingerprintStreamInternal(stream, {}, fingerprints);
}

bool FingerprintYoutubeRoute(const ParsedUrl& parsed, std::vector<std::string>& fingerprints)
{
  constexpr std::string_view PREFIX{"yt/"};
  if (parsed.hasQuery || parsed.path.size() <= PREFIX.size() ||
      parsed.path.find('/', PREFIX.size()) != std::string::npos)
  {
    return false;
  }

  std::string ytId;
  if (!PercentDecode(std::string_view{parsed.path}.substr(PREFIX.size()), false, ytId) ||
      ytId.empty())
  {
    return false;
  }

  CVariant stream{CVariant::VariantTypeObject};
  stream["ytId"] = std::move(ytId);
  return FingerprintStreamInternal(stream, {}, fingerprints);
}

bool FingerprintPlaybackUrlInternal(std::string_view playbackUrl,
                                    std::vector<std::string>& fingerprints)
{
  fingerprints.clear();
  ParsedUrl parsed;
  if (!ParseUrl(playbackUrl, MAX_PLAYBACK_URL_BYTES, parsed) ||
      !IsSupportedDirectProtocol(parsed.protocol))
    return false;

  const HostKind hostKind = ClassifyHost(parsed);
  if (hostKind == HostKind::UNSAFE)
    return false;
  if (hostKind == HostKind::REMOTE)
  {
    std::string fingerprint;
    if (!FingerprintTypedValue("url", playbackUrl, fingerprint))
      return false;
    fingerprints.emplace_back(std::move(fingerprint));
    return true;
  }

  if (parsed.protocol != "http" || !parsed.hasPort || parsed.port != 11470 || parsed.hasUserInfo ||
      parsed.hasFragment)
  {
    return false;
  }

  if (parsed.path.starts_with("proxy/"))
    return FingerprintProxyRoute(parsed, fingerprints);
  if (parsed.path == "rar/create")
    return FingerprintCreateRoute("rar", parsed, fingerprints);
  if (parsed.path == "zip/create")
    return FingerprintCreateRoute("zip", parsed, fingerprints);
  if (parsed.path == "7zip/create")
    return FingerprintCreateRoute("7zip", parsed, fingerprints);
  if (parsed.path == "tgz/create")
    return FingerprintCreateRoute("tgz", parsed, fingerprints);
  if (parsed.path == "tar/create")
    return FingerprintCreateRoute("tar", parsed, fingerprints);
  if (parsed.path == "nzb/create")
    return FingerprintCreateRoute("nzb", parsed, fingerprints);
  if (parsed.path.starts_with("yt/"))
    return FingerprintYoutubeRoute(parsed, fingerprints);
  return FingerprintTorrentRoute(parsed, fingerprints);
}

} // unnamed namespace

bool CJumpgateSourceFingerprint::FingerprintExactUrl(std::string_view url, std::string& fingerprint)
{
  fingerprint.clear();
  try
  {
    return FingerprintTypedValue("url", url, fingerprint);
  }
  catch (...)
  {
    fingerprint.clear();
    return false;
  }
}

bool CJumpgateSourceFingerprint::FingerprintStream(const CVariant& stream,
                                                   std::vector<std::string>& fingerprints)
{
  return FingerprintStream(stream, {}, fingerprints);
}

bool CJumpgateSourceFingerprint::FingerprintStream(
    const CVariant& stream,
    const std::vector<std::string>& extraFingerprints,
    std::vector<std::string>& fingerprints)
{
  fingerprints.clear();
  try
  {
    if (!FingerprintStreamInternal(stream, extraFingerprints, fingerprints))
    {
      fingerprints.clear();
      return false;
    }
    return true;
  }
  catch (...)
  {
    fingerprints.clear();
    return false;
  }
}

bool CJumpgateSourceFingerprint::FingerprintPlaybackUrl(std::string_view playbackUrl,
                                                        std::vector<std::string>& fingerprints)
{
  fingerprints.clear();
  try
  {
    if (!FingerprintPlaybackUrlInternal(playbackUrl, fingerprints) || fingerprints.empty())
    {
      fingerprints.clear();
      return false;
    }
    return true;
  }
  catch (...)
  {
    fingerprints.clear();
    return false;
  }
}

} // namespace UTILITY
} // namespace KODI
