/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JumpgatePlaybackContext.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

namespace
{
CVariant Array()
{
  return CVariant{CVariant::VariantTypeArray};
}

CVariant Object()
{
  return CVariant{CVariant::VariantTypeObject};
}

CVariant ValidContext()
{
  CVariant context = Object();
  context["schemaVersion"] = 1;
  context["contextId"] = "context-00000001";
  context["profileId"] = "profile-00000001";
  context["contentKey"] = std::string(64, 'a');
  context["canonicalIdentity"] = CVariant{};
  context["traktEligible"] = false;

  CVariant request = Object();
  request["resource"] = "stream";
  request["type"] = "movie";
  request["metaId"] = "local-only-item";
  request["videoId"] = "local-only-item";
  request["metaProvider"] = "stremio";
  request["streamProvider"] = "provider-a";
  CVariant streamProviders = Array();
  streamProviders.push_back("provider-a");
  streamProviders.push_back("provider-b");
  request["streamProviders"] = std::move(streamProviders);
  context["request"] = std::move(request);

  CVariant display = Object();
  display["title"] = "Local playback";
  display["year"] = CVariant{};
  display["season"] = CVariant{};
  display["episode"] = CVariant{};
  display["poster"] = CVariant{};
  display["background"] = CVariant{};
  display["logo"] = CVariant{};
  context["display"] = std::move(display);

  CVariant source = Object();
  source["type"] = "url";
  source["provider"] = "provider-a";
  CVariant providers = Array();
  providers.push_back("provider-a");
  providers.push_back("provider-b");
  source["providers"] = std::move(providers);
  context["source"] = std::move(source);

  CVariant fingerprints = Array();
  fingerprints.push_back("v1:url:sha256:" + std::string(64, 'b'));
  context["fingerprints"] = std::move(fingerprints);
  context["inlineSubtitles"] = Array();
  context["createdAt"] = "2026-07-13T10:00:00.123Z";
  context["expiresAt"] = "2026-07-13T10:05:00.123Z";
  return context;
}

void SetCanonicalMovie(CVariant& context,
                       const std::string& provider,
                       const std::string& id,
                       const std::string& provenance = "metadata-request")
{
  CVariant identity = Object();
  identity["provider"] = provider;
  identity["id"] = id;
  identity["mediaType"] = "movie";
  identity["season"] = CVariant{};
  identity["episode"] = CVariant{};
  identity["confidence"] = "canonical";
  identity["provenance"] = provenance;
  context["canonicalIdentity"] = std::move(identity);
}

void SetCanonicalEpisode(CVariant& context, int season, int episode)
{
  CVariant identity = Object();
  identity["provider"] = "imdb";
  identity["id"] = "tt1234567";
  identity["mediaType"] = "episode";
  identity["season"] = season;
  identity["episode"] = episode;
  identity["confidence"] = "canonical";
  identity["provenance"] = "verified-external-id";
  context["canonicalIdentity"] = std::move(identity);
}

CVariant Subtitle(const std::string& id, const std::string& language, const std::string& url)
{
  CVariant subtitle = Object();
  subtitle["id"] = id;
  subtitle["lang"] = language;
  subtitle["url"] = url;
  return subtitle;
}

bool Parses(const CVariant& context)
{
  return CJumpgatePlaybackContextParser::Parse(context).has_value();
}

} // namespace

TEST(TestJumpgatePlaybackContext, ParsesLocalOnlyContextWithoutCanonicalAuthority)
{
  const std::optional<JumpgatePlaybackContext> parsed =
      CJumpgatePlaybackContextParser::Parse(ValidContext());

  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->schemaVersion, 1);
  EXPECT_EQ(parsed->profileId, "profile-00000001");
  ASSERT_TRUE(parsed->contentKey);
  EXPECT_EQ(*parsed->contentKey, std::string(64, 'a'));
  EXPECT_FALSE(parsed->canonicalIdentity);
  EXPECT_FALSE(parsed->traktEligible);
  ASSERT_TRUE(parsed->display.title);
  EXPECT_EQ(*parsed->display.title, "Local playback");
  EXPECT_TRUE(parsed->inlineSubtitles.empty());
}

TEST(TestJumpgatePlaybackContext, ParsesEveryCanonicalProvider)
{
  struct ProviderCase
  {
    const char* name;
    const char* id;
    JumpgateCanonicalProvider expected;
  };
  constexpr std::array<ProviderCase, 4> cases = {
      ProviderCase{"imdb", "tt0133093", JumpgateCanonicalProvider::Imdb},
      ProviderCase{"tmdb", "603", JumpgateCanonicalProvider::Tmdb},
      ProviderCase{"tvdb", "169", JumpgateCanonicalProvider::Tvdb},
      ProviderCase{"trakt", "603", JumpgateCanonicalProvider::Trakt},
  };

  for (const ProviderCase& item : cases)
  {
    CVariant context = ValidContext();
    SetCanonicalMovie(context, item.name, item.id, "verified-external-id");
    context["traktEligible"] = true;

    const std::optional<JumpgatePlaybackContext> parsed =
        CJumpgatePlaybackContextParser::Parse(context);

    ASSERT_TRUE(parsed) << item.name;
    ASSERT_TRUE(parsed->canonicalIdentity) << item.name;
    EXPECT_EQ(parsed->canonicalIdentity->provider, item.expected) << item.name;
    EXPECT_EQ(parsed->canonicalIdentity->id, item.id) << item.name;
    EXPECT_EQ(parsed->canonicalIdentity->mediaType, JumpgateMediaType::Movie) << item.name;
    EXPECT_STREQ(ToString(item.expected), item.name);
    EXPECT_TRUE(parsed->traktEligible);
  }
}

TEST(TestJumpgatePlaybackContext, ProjectsBoundedDisplayAndInlineSubtitles)
{
  CVariant context = ValidContext();
  SetCanonicalEpisode(context, 2, 3);
  context["traktEligible"] = true;
  context["display"]["title"] = "Episode title";
  context["display"]["year"] = 2026;
  context["display"]["season"] = 2;
  context["display"]["episode"] = 3;
  context["display"]["poster"] = "https://images.example/poster.jpg";
  context["display"]["background"] = "https://images.example/background.jpg";
  context["display"]["logo"] = "https://images.example/logo.png";
  context["inlineSubtitles"].push_back(
      Subtitle("inline-en", "eng", "https://subs.example/en.vtt?token=subtitle"));
  CVariant urlOnly = Object();
  urlOnly["url"] = "https://subs.example/forced.srt";
  context["inlineSubtitles"].push_back(std::move(urlOnly));

  const std::optional<JumpgatePlaybackContext> parsed =
      CJumpgatePlaybackContextParser::Parse(context);

  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->canonicalIdentity);
  EXPECT_EQ(parsed->canonicalIdentity->mediaType, JumpgateMediaType::Episode);
  EXPECT_EQ(parsed->canonicalIdentity->season, 2);
  EXPECT_EQ(parsed->canonicalIdentity->episode, 3);
  EXPECT_EQ(parsed->display.title, "Episode title");
  EXPECT_EQ(parsed->display.year, 2026);
  EXPECT_EQ(parsed->display.season, 2);
  EXPECT_EQ(parsed->display.episode, 3);
  EXPECT_EQ(parsed->display.poster, "https://images.example/poster.jpg");
  EXPECT_EQ(parsed->display.background, "https://images.example/background.jpg");
  EXPECT_EQ(parsed->display.logo, "https://images.example/logo.png");
  ASSERT_EQ(parsed->inlineSubtitles.size(), 2u);
  EXPECT_EQ(parsed->inlineSubtitles[0].id, "inline-en");
  EXPECT_EQ(parsed->inlineSubtitles[0].language, "eng");
  EXPECT_EQ(parsed->inlineSubtitles[0].url, "https://subs.example/en.vtt?token=subtitle");
  EXPECT_FALSE(parsed->inlineSubtitles[1].id);
  EXPECT_FALSE(parsed->inlineSubtitles[1].language);
}

TEST(TestJumpgatePlaybackContext, RejectsUnknownOrNonCanonicalAuthority)
{
  std::vector<CVariant> invalid;
  CVariant context = ValidContext();
  SetCanonicalMovie(context, "unknown", "item");
  invalid.emplace_back(context);
  context = ValidContext();
  SetCanonicalMovie(context, "imdb", "0133093");
  invalid.emplace_back(context);
  context = ValidContext();
  SetCanonicalMovie(context, "tmdb", "603");
  context["canonicalIdentity"]["confidence"] = "inferred";
  invalid.emplace_back(context);
  context = ValidContext();
  SetCanonicalMovie(context, "tvdb", "169");
  context["canonicalIdentity"]["provenance"] = "stream-title";
  invalid.emplace_back(context);
  context = ValidContext();
  SetCanonicalMovie(context, "trakt", "movie");
  context["canonicalIdentity"]["authority"] = "provider-asserted";
  invalid.emplace_back(context);

  for (const CVariant& item : invalid)
    EXPECT_FALSE(Parses(item));
}

TEST(TestJumpgatePlaybackContext, EnforcesTraktEligibilityDependency)
{
  CVariant context = ValidContext();
  context["traktEligible"] = true;
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  SetCanonicalMovie(context, "imdb", "tt0133093");
  context["traktEligible"] = false;
  const std::optional<JumpgatePlaybackContext> parsed =
      CJumpgatePlaybackContextParser::Parse(context);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->canonicalIdentity);
  EXPECT_FALSE(parsed->traktEligible);

  context["traktEligible"] = "true";
  EXPECT_FALSE(Parses(context));
}

TEST(TestJumpgatePlaybackContext, RejectsNonPositiveOrNonCanonicalNumericProviderIds)
{
  for (const char* provider : {"tmdb", "tvdb", "trakt"})
  {
    for (const char* id : {"slug", "0", "001", "9223372036854775808"})
    {
      CVariant context = ValidContext();
      SetCanonicalMovie(context, provider, id);
      context["traktEligible"] = true;
      EXPECT_FALSE(Parses(context)) << provider << ":" << id;
    }
  }
}

TEST(TestJumpgatePlaybackContext, EnforcesEpisodeAndDisplayConsistency)
{
  CVariant valid = ValidContext();
  SetCanonicalEpisode(valid, 0, 0);
  valid["display"]["season"] = 0;
  valid["display"]["episode"] = 0;
  EXPECT_TRUE(Parses(valid));

  CVariant invalid = valid;
  invalid["canonicalIdentity"].erase("episode");
  EXPECT_FALSE(Parses(invalid));
  invalid = valid;
  invalid["canonicalIdentity"]["season"] = -1;
  EXPECT_FALSE(Parses(invalid));
  invalid = valid;
  invalid["canonicalIdentity"]["episode"] = 0.5;
  EXPECT_FALSE(Parses(invalid));
  invalid = valid;
  invalid["display"]["episode"] = 1;
  EXPECT_FALSE(Parses(invalid));

  invalid = ValidContext();
  SetCanonicalMovie(invalid, "imdb", "tt0133093");
  invalid["canonicalIdentity"]["season"] = 1;
  EXPECT_FALSE(Parses(invalid));
  invalid = ValidContext();
  SetCanonicalMovie(invalid, "imdb", "tt0133093");
  invalid["display"]["episode"] = 1;
  EXPECT_FALSE(Parses(invalid));
}

TEST(TestJumpgatePlaybackContext, RejectsMalformedOrRawSourceShapes)
{
  EXPECT_FALSE(Parses(CVariant{}));
  EXPECT_FALSE(Parses(Array()));

  CVariant context = ValidContext();
  context.erase("schemaVersion");
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["schemaVersion"] = 2;
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["unknown"] = true;
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["source"]["url"] = "https://media.example/private.mkv?token=secret";
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["display"]["authorization"] = "Bearer secret";
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["request"]["streamProviders"].push_back("provider-a");
  EXPECT_FALSE(Parses(context));
}

TEST(TestJumpgatePlaybackContext, RejectsNonCanonicalFingerprintsAndTimestamps)
{
  CVariant context = ValidContext();
  context["contentKey"] = std::string(64, 'A');
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["contentKey"] = std::string(63, 'a');
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["fingerprints"][0] = "v1:url:sha256:" + std::string(64, 'A');
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["fingerprints"].push_back(context["fingerprints"][0]);
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["fingerprints"][0] = "v1:info-hash:" + std::string(40, 'c') +
                               ":file-idx:-1:file-must-include:sha256:" + std::string(64, 'd');
  EXPECT_TRUE(Parses(context));
  context["fingerprints"][0] = "v1:info-hash:" + std::string(40, 'c') + ":file-idx:65536";
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  context["createdAt"] = "2026-02-30T10:00:00.123Z";
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["expiresAt"] = context["createdAt"];
  EXPECT_FALSE(Parses(context));
  context = ValidContext();
  context["expiresAt"] = "2026-07-21T10:00:00.123Z";
  EXPECT_FALSE(Parses(context));
}

TEST(TestJumpgatePlaybackContext, IgnoresUnsupportedOptionalInlineSubtitles)
{
  std::vector<CVariant> invalidSubtitles;
  invalidSubtitles.emplace_back(CVariant{});
  invalidSubtitles.emplace_back(Object());
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "http://subs.example/a.vtt"));
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "file:///storage/emulated/0/a.srt"));
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "https://user@subs.example/a.vtt"));
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "https://:443/a.vtt"));
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "https://subs.example:bad/a.vtt"));
  invalidSubtitles.emplace_back(Subtitle("id", "eng", "https://subs.example/a.vtt\nnext"));
  invalidSubtitles.emplace_back(Subtitle("id", std::string(65, 'x'), "https://subs.example/a.vtt"));
  CVariant withHeaders = Subtitle("id", "eng", "https://subs.example/a.vtt");
  withHeaders["headers"] = Object();
  withHeaders["headers"]["Authorization"] = "Bearer secret";
  invalidSubtitles.emplace_back(std::move(withHeaders));
  CVariant withToken = Subtitle("id", "eng", "https://subs.example/a.vtt");
  withToken["token"] = "secret";
  invalidSubtitles.emplace_back(std::move(withToken));

  for (const CVariant& subtitle : invalidSubtitles)
  {
    CVariant context = ValidContext();
    context["inlineSubtitles"].push_back(subtitle);
    const auto parsed = CJumpgatePlaybackContextParser::Parse(context);
    ASSERT_TRUE(parsed);
    if (subtitle.isObject() && subtitle.isMember("url") &&
        subtitle["url"].asString() == "https://subs.example/a.vtt" &&
        (!subtitle.isMember("lang") || subtitle["lang"].asString().size() <= 64))
    {
      ASSERT_EQ(parsed->inlineSubtitles.size(), 1u);
      EXPECT_EQ(parsed->inlineSubtitles[0].url, "https://subs.example/a.vtt");
    }
    else
    {
      EXPECT_TRUE(parsed->inlineSubtitles.empty());
    }
  }

  CVariant context = ValidContext();
  context["inlineSubtitles"] = Object();
  EXPECT_FALSE(Parses(context));
}

TEST(TestJumpgatePlaybackContext, RejectsDeepOversizedAndOverBudgetShapes)
{
  CVariant nested = "leaf";
  for (int depth = 0; depth < 7; ++depth)
  {
    CVariant parent = Object();
    parent["nested"] = std::move(nested);
    nested = std::move(parent);
  }
  CVariant context = ValidContext();
  CVariant deepSubtitle = Subtitle("deep", "eng", "https://subs.example/deep.vtt");
  deepSubtitle["nested"] = std::move(nested);
  context["inlineSubtitles"].push_back(std::move(deepSubtitle));
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  context["display"]["title"] = std::string(8193, 'x');
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  for (int index = 0; index < 65; ++index)
  {
    context["inlineSubtitles"].push_back(Subtitle(
        std::to_string(index), "eng", "https://subs.example/" + std::to_string(index) + ".vtt"));
  }
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  const std::string prefix = "https://subs.example/";
  const std::string maximumUrl = prefix + std::string(8192 - prefix.size(), 'x');
  for (int index = 0; index < 40; ++index)
    context["inlineSubtitles"].push_back(Subtitle(std::to_string(index), "eng", maximumUrl));
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  context["display"]["__proto__"] = "unsafe";
  EXPECT_FALSE(Parses(context));

  context = ValidContext();
  CVariant manyNodes = Array();
  for (int outer = 0; outer < 64; ++outer)
  {
    CVariant row = Array();
    for (int inner = 0; inner < 64; ++inner)
      row.push_back(inner);
    manyNodes.push_back(std::move(row));
  }
  context["padding"] = std::move(manyNodes);
  EXPECT_FALSE(Parses(context));
}
