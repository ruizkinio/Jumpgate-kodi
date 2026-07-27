/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Variant.h"

#include <optional>
#include <string>
#include <vector>

namespace KODI::JUMPGATE
{

enum class JumpgateCanonicalProvider
{
  Imdb,
  Tmdb,
  Tvdb,
  Trakt,
};

enum class JumpgateMediaType
{
  Movie,
  Episode,
};

const char* ToString(JumpgateCanonicalProvider provider);
const char* ToString(JumpgateMediaType mediaType);

struct JumpgateCanonicalIdentity
{
  JumpgateCanonicalProvider provider{JumpgateCanonicalProvider::Imdb};
  std::string id;
  JumpgateMediaType mediaType{JumpgateMediaType::Movie};
  std::optional<int> season;
  std::optional<int> episode;
};

struct JumpgatePlaybackDisplay
{
  std::optional<std::string> title;
  std::optional<int> year;
  std::optional<int> season;
  std::optional<int> episode;
  std::optional<std::string> poster;
  std::optional<std::string> background;
  std::optional<std::string> logo;
};

struct JumpgateInlineSubtitle
{
  std::optional<std::string> id;
  std::optional<std::string> language;
  std::string url;
};

struct JumpgatePlaybackContext
{
  int schemaVersion{0};
  std::string profileId;
  std::optional<std::string> contentKey;
  std::optional<JumpgateCanonicalIdentity> canonicalIdentity;
  bool traktEligible{false};
  JumpgatePlaybackDisplay display;
  std::vector<JumpgateInlineSubtitle> inlineSubtitles;
};

class CJumpgatePlaybackContextParser final
{
public:
  static std::optional<JumpgatePlaybackContext> Parse(const CVariant& value);
};

} // namespace KODI::JUMPGATE
