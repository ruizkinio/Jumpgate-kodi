/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "filesystem/File.h"
#include "utils/JumpgateQrCode.h"
#include "utils/URIUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

using namespace KODI::JUMPGATE;

TEST(TestJumpgateQrCode, KnownUrlHasDeterministicMatrix)
{
  constexpr std::string_view URL = "https://bridge.example/pair";
  constexpr std::array<std::string_view, 29> EXPECTED{{
      "11111110010011001110001111111", "10000010110010011100101000001",
      "10111010111001001010001011101", "10111010000011110100001011101",
      "10111010000010000001001011101", "10000010001101011111001000001",
      "11111110101010101010101111111", "00000000011000101001100000000",
      "01110110001100111011100000110", "01000101110100010011111010001",
      "11000010110110001110001001110", "10111001011001110001000100001",
      "11011110111011110101110100111", "11000001011110100101001101101",
      "00101010000100100001010111011", "10000101101110010001001011010",
      "10111110001001100100110111001", "01101101111001100010110100100",
      "10001111011011000100101011000", "00101001011001011110111011111",
      "01011111011000111111111110100", "00000000100010001110100011001",
      "11111110010101000011101010110", "10000010100000010100100010011",
      "10111010001111001000111111100", "10111010110011100101000010110",
      "10111010101000000010100101001", "10000010101000010000110011010",
      "11111110010010100001100100010",
  }};

  JumpgateQrMatrix matrix;
  ASSERT_TRUE(CJumpgateQrCode::EncodeMatrix(URL, matrix));
  ASSERT_EQ(matrix.size, static_cast<int>(EXPECTED.size()));
  ASSERT_EQ(matrix.modules.size(), EXPECTED.size() * EXPECTED.size());

  for (int y = 0; y < matrix.size; ++y)
  {
    for (int x = 0; x < matrix.size; ++x)
      EXPECT_EQ(matrix.IsDark(x, y), EXPECTED[y][x] == '1') << "module " << x << ',' << y;
  }
}

TEST(TestJumpgateQrCode, WritesPngAndCleansUpOwnedArtifact)
{
  const std::string pngPath = RenderJumpgatePairingQr("https://bridge.example/pair");
  ASSERT_FALSE(pngPath.empty());
  EXPECT_TRUE(URIUtils::IsSpecial(pngPath));
  EXPECT_TRUE(URIUtils::PathHasParent(pngPath, "special://temp/"));
  ASSERT_TRUE(XFILE::CFile::Exists(pngPath, false));

  const std::string secondPngPath = RenderJumpgatePairingQr("https://bridge.example/pair");
  ASSERT_FALSE(secondPngPath.empty());
  EXPECT_NE(secondPngPath, pngPath);
  EXPECT_TRUE(URIUtils::PathHasParent(secondPngPath, "special://temp/"));
  ASSERT_TRUE(XFILE::CFile::Exists(secondPngPath, false));

  XFILE::CFile file;
  ASSERT_TRUE(file.Open(pngPath));
  std::array<unsigned char, 24> header{};
  ASSERT_EQ(file.Read(header.data(), header.size()), static_cast<ssize_t>(header.size()));
  file.Close();

  constexpr std::array<unsigned char, 8> PNG_SIGNATURE{0x89, 0x50, 0x4e, 0x47,
                                                       0x0d, 0x0a, 0x1a, 0x0a};
  EXPECT_TRUE(std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), header.begin()));

  // A 29-module symbol plus the mandatory four-module quiet zone at 8 px/module.
  constexpr std::array<unsigned char, 4> EXPECTED_DIMENSION{0x00, 0x00, 0x01, 0x28};
  EXPECT_TRUE(
      std::equal(EXPECTED_DIMENSION.begin(), EXPECTED_DIMENSION.end(), header.begin() + 16));
  EXPECT_TRUE(
      std::equal(EXPECTED_DIMENSION.begin(), EXPECTED_DIMENSION.end(), header.begin() + 20));

  EXPECT_TRUE(XFILE::CFile::Delete(pngPath));
  EXPECT_FALSE(XFILE::CFile::Exists(pngPath, false));
  EXPECT_TRUE(XFILE::CFile::Delete(secondPngPath));
  EXPECT_FALSE(XFILE::CFile::Exists(secondPngPath, false));
}

TEST(TestJumpgateQrCode, RejectsEmptyAndOversizedInput)
{
  JumpgateQrMatrix matrix;
  EXPECT_FALSE(CJumpgateQrCode::EncodeMatrix({}, matrix));
  EXPECT_EQ(matrix.size, 0);
  EXPECT_TRUE(matrix.modules.empty());

  const std::string oversized(4096, 'x');
  EXPECT_FALSE(CJumpgateQrCode::EncodeMatrix(oversized, matrix));
  EXPECT_EQ(matrix.size, 0);
  EXPECT_TRUE(matrix.modules.empty());
  EXPECT_TRUE(CJumpgateQrCode::RenderPng(oversized).empty());
}
