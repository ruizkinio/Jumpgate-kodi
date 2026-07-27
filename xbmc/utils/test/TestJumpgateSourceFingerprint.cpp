/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/Digest.h"
#include "utils/JSONVariantParser.h"
#include "utils/JumpgateSourceFingerprint.h"
#include "utils/Variant.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using KODI::UTILITY::CDigest;
using KODI::UTILITY::CJumpgateSourceFingerprint;

namespace
{

constexpr std::string_view FIXTURE_SHA256{
    "33f7213ff6188cf216222e9ab1315af2aa95949e812aee5053c4e8fea58b91c9"};

bool LoadFixtures(std::string& bytes, CVariant& fixtures)
{
  const std::filesystem::path fixturePath =
      std::filesystem::path{__FILE__}.parent_path() / "fixtures" / "source-fingerprint-v1.json";
  std::ifstream input{fixturePath, std::ios::binary};
  if (!input)
    return false;

  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
  return CJSONVariantParser::Parse(bytes, fixtures) && fixtures.isObject();
}

bool ReadStringArray(const CVariant& value, std::vector<std::string>& result)
{
  result.clear();
  if (!value.isArray())
    return false;
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    if (!item->isString())
      return false;
    result.emplace_back(item->asString());
  }
  return true;
}

bool ReadOptionalStringArray(const CVariant& object,
                             const std::string& key,
                             std::vector<std::string>& result)
{
  result.clear();
  return !object.isMember(key) || ReadStringArray(object[key], result);
}

} // unnamed namespace

TEST(TestJumpgateSourceFingerprint, FixtureBytesArePinned)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));
  EXPECT_EQ(FIXTURE_SHA256, CDigest::Calculate(CDigest::Type::SHA256, bytes));
  EXPECT_EQ(std::string::npos, bytes.find('\r'));
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ('\n', bytes.back());
  ASSERT_TRUE(fixtures["formatVersion"].isInteger());
  EXPECT_EQ(1, fixtures["formatVersion"].asInteger());
  EXPECT_EQ("source-fingerprint-v1", fixtures["algorithm"].asString());
}

TEST(TestJumpgateSourceFingerprint, NzbFileSelectorsDoNotCollide)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));

  std::set<std::string> fingerprints;
  std::size_t matchingCases = 0;
  for (auto fixture = fixtures["validCases"].begin_array();
       fixture != fixtures["validCases"].end_array(); ++fixture)
  {
    const std::string id = (*fixture)["id"].asString();
    if (!id.starts_with("nzb-create-route"))
      continue;
    ++matchingCases;
    ASSERT_TRUE((*fixture)["expected"].isArray());
    ASSERT_EQ(1, (*fixture)["expected"].size());
    fingerprints.emplace((*fixture)["expected"][0].asString());
  }

  EXPECT_EQ(4, matchingCases);
  EXPECT_EQ(matchingCases, fingerprints.size());
}

TEST(TestJumpgateSourceFingerprint, StructuredSourcesMatchSharedFixtures)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));
  ASSERT_TRUE(fixtures["validCases"].isArray());

  for (auto fixture = fixtures["validCases"].begin_array();
       fixture != fixtures["validCases"].end_array(); ++fixture)
  {
    SCOPED_TRACE(fixture->isMember("id") ? (*fixture)["id"].asString() : "missing fixture id");
    ASSERT_TRUE(fixture->isObject());
    ASSERT_TRUE(fixture->isMember("stream"));

    std::vector<std::string> extraFingerprints;
    std::vector<std::string> expected;
    ASSERT_TRUE(ReadOptionalStringArray(*fixture, "extraFingerprints", extraFingerprints));
    ASSERT_TRUE(ReadStringArray((*fixture)["expected"], expected));

    std::vector<std::string> actual{"must-be-cleared"};
    ASSERT_TRUE(CJumpgateSourceFingerprint::FingerprintStream((*fixture)["stream"],
                                                              extraFingerprints, actual));
    EXPECT_EQ(expected, actual);

    if (fixture->isMember("exactUrl"))
    {
      std::string exact{"must-be-cleared"};
      ASSERT_TRUE(CJumpgateSourceFingerprint::FingerprintExactUrl((*fixture)["exactUrl"].asString(),
                                                                  exact));
      EXPECT_EQ((*fixture)["exactExpected"].asString(), exact);
    }
  }
}

TEST(TestJumpgateSourceFingerprint, PlaybackRoutesMatchSharedFixtures)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));

  for (auto fixture = fixtures["validCases"].begin_array();
       fixture != fixtures["validCases"].end_array(); ++fixture)
  {
    if (!fixture->isMember("playbackUrl"))
      continue;

    SCOPED_TRACE((*fixture)["id"].asString());
    std::vector<std::string> expected;
    ASSERT_TRUE(ReadStringArray((*fixture)["expected"], expected));

    std::vector<std::string> actual{"must-be-cleared"};
    ASSERT_TRUE(CJumpgateSourceFingerprint::FingerprintPlaybackUrl(
        (*fixture)["playbackUrl"].asString(), actual));
    EXPECT_EQ(expected, actual);
  }
}

TEST(TestJumpgateSourceFingerprint, InvalidStructuredSourcesFailClosed)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));

  for (auto fixture = fixtures["invalidStreams"].begin_array();
       fixture != fixtures["invalidStreams"].end_array(); ++fixture)
  {
    SCOPED_TRACE((*fixture)["id"].asString());
    std::vector<std::string> actual{"must-be-cleared"};
    EXPECT_FALSE(CJumpgateSourceFingerprint::FingerprintStream((*fixture)["stream"], actual));
    EXPECT_TRUE(actual.empty());
  }
}

TEST(TestJumpgateSourceFingerprint, InvalidPlaybackUrlsFailClosed)
{
  std::string bytes;
  CVariant fixtures;
  ASSERT_TRUE(LoadFixtures(bytes, fixtures));

  for (auto fixture = fixtures["invalidPlaybackUrls"].begin_array();
       fixture != fixtures["invalidPlaybackUrls"].end_array(); ++fixture)
  {
    SCOPED_TRACE((*fixture)["id"].asString());
    std::vector<std::string> actual{"must-be-cleared"};
    EXPECT_FALSE(CJumpgateSourceFingerprint::FingerprintPlaybackUrl(
        (*fixture)["playbackUrl"].asString(), actual));
    EXPECT_TRUE(actual.empty());
  }
}

TEST(TestJumpgateSourceFingerprint, AmbiguousLoopbackRoutesFailClosed)
{
  constexpr std::array<std::string_view, 3> UNSAFE_ROUTES{{
      "http://127.0.0.2:11470/unknown/source",
      "http://[::1]:11470/unknown/source",
      "http://127.0.0.1:11470/proxy/d=https%3A%2F%2Fmedia.example/",
  }};

  for (std::string_view route : UNSAFE_ROUTES)
  {
    SCOPED_TRACE(route);
    std::vector<std::string> actual{"must-be-cleared"};
    EXPECT_FALSE(CJumpgateSourceFingerprint::FingerprintPlaybackUrl(route, actual));
    EXPECT_TRUE(actual.empty());
  }
}

TEST(TestJumpgateSourceFingerprint, LoopbackLookalikesKeepExactUrlBytes)
{
  constexpr std::array<std::string_view, 2> REMOTE_URLS{{
      "http://127.example:11470/unknown/source?token=a%2Bb",
      "http://[2001:db8::127.0.0.1]:11470/unknown/source?token=a+b",
  }};

  for (std::string_view url : REMOTE_URLS)
  {
    SCOPED_TRACE(url);
    std::string expected;
    ASSERT_TRUE(CJumpgateSourceFingerprint::FingerprintExactUrl(url, expected));

    std::vector<std::string> actual;
    ASSERT_TRUE(CJumpgateSourceFingerprint::FingerprintPlaybackUrl(url, actual));
    EXPECT_EQ(std::vector<std::string>{expected}, actual);
  }
}
