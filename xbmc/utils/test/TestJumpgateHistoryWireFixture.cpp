/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/JSONVariantParser.h"
#include "utils/JumpgateHistoryEventClient.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>

using namespace KODI::JUMPGATE;

namespace
{
constexpr const char* FIXTURE_SHA256 =
    "9c6f5eff5a0782bfc0be70ed8c23cf865c89ede5653d8f97c027573e3b003851";

bool LoadFixture(std::string& bytes, CVariant& fixture)
{
  const std::filesystem::path fixturePath =
      std::filesystem::path{__FILE__}.parent_path() / "fixtures" / "history-wire-v1.json";
  std::ifstream input{fixturePath, std::ios::binary};
  if (!input)
    return false;
  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
  return CJSONVariantParser::Parse(bytes, fixture) && fixture.isObject();
}

std::string Sha256(const std::string& bytes)
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
    return {};
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < size; ++index)
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  return output.str();
}

std::vector<std::string> Strings(const CVariant& value)
{
  std::vector<std::string> result;
  if (!value.isArray())
    return result;
  for (auto item = value.begin_array(); item != value.end_array(); ++item)
  {
    if (!item->isString())
      return {};
    result.emplace_back(item->asString());
  }
  return result;
}
} // namespace

TEST(TestJumpgateHistoryWireFixture, SharedBridgeContractBytesArePinned)
{
  std::string bytes;
  CVariant fixture;
  ASSERT_TRUE(LoadFixture(bytes, fixture));
  EXPECT_EQ(Sha256(bytes), FIXTURE_SHA256);
  EXPECT_EQ(fixture["formatVersion"].asInteger(), 1);
  EXPECT_EQ(fixture["algorithm"].asString(), "history-wire-v1");
  EXPECT_EQ(fixture["claim"]["method"].asString(), "POST");
  EXPECT_EQ(fixture["claim"]["path"].asString(), "/v1/playback/claim");
  EXPECT_EQ(fixture["history"]["method"].asString(), "POST");
  EXPECT_EQ(fixture["history"]["path"].asString(), "/v1/history/events");
  EXPECT_EQ(fixture["release"]["path"].asString(), "/v1/playback/release");
}

TEST(TestJumpgateHistoryWireFixture, EventsFieldsAndReceiptSourceMatchKodiWireTypes)
{
  std::string bytes;
  CVariant fixture;
  ASSERT_TRUE(LoadFixture(bytes, fixture));

  EXPECT_EQ(Strings(fixture["history"]["events"]),
            (std::vector<std::string>{"start", "progress", "pause", "background", "resume", "stop",
                                      "completion"}));
  EXPECT_EQ(Strings(fixture["history"]["requestRequiredFields"]),
            (std::vector<std::string>{"event", "sessionRevision", "positionMs", "durationMs",
                                      "watchedMs"}));
  EXPECT_EQ(Strings(fixture["history"]["grantKinds"]),
            (std::vector<std::string>{"canonical", "local", "negative"}));
  EXPECT_EQ(Strings(fixture["history"]["terminalEvents"]),
            (std::vector<std::string>{"stop", "completion"}));
  EXPECT_EQ(Strings(fixture["release"]["requestRequiredFields"]),
            (std::vector<std::string>{"sessionId", "terminalReceiptId"}));
  EXPECT_EQ(fixture["release"]["terminalReceiptSource"].asString(),
            "history-event-idempotency-key");
  EXPECT_EQ(Strings(fixture["retiredPaths"]),
            (std::vector<std::string>{"/v1/trakt/scrobble/start", "/v1/trakt/scrobble/pause",
                                      "/v1/trakt/scrobble/resume", "/v1/trakt/scrobble/stop",
                                      "/v1/trakt/scrobble/completion"}));
}
