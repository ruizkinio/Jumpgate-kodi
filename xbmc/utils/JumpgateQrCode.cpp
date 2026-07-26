/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "JumpgateQrCode.h"

#include "filesystem/File.h"
#include "pictures/Picture.h"
#include "qrcodegen.hpp"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>

using namespace KODI::JUMPGATE;

namespace
{
constexpr int QUIET_ZONE_MODULES = 4;
constexpr int PIXELS_PER_MODULE = 8;
constexpr int BYTES_PER_PIXEL = 4;
constexpr std::array<unsigned char, 8> PNG_SIGNATURE{0x89, 0x50, 0x4e, 0x47,
                                                     0x0d, 0x0a, 0x1a, 0x0a};

bool RenderPixels(const JumpgateQrMatrix& matrix,
                  int& imageSize,
                  std::vector<unsigned char>& pixels)
{
  if (matrix.size <= 0 || matrix.modules.size() != static_cast<std::size_t>(matrix.size) *
                                                       static_cast<std::size_t>(matrix.size))
  {
    return false;
  }

  imageSize = (matrix.size + 2 * QUIET_ZONE_MODULES) * PIXELS_PER_MODULE;
  const std::size_t stride = static_cast<std::size_t>(imageSize) * BYTES_PER_PIXEL;
  pixels.assign(stride * static_cast<std::size_t>(imageSize), 0xff);

  for (int moduleY = 0; moduleY < matrix.size; ++moduleY)
  {
    for (int moduleX = 0; moduleX < matrix.size; ++moduleX)
    {
      if (!matrix.IsDark(moduleX, moduleY))
        continue;

      const int firstPixelX = (moduleX + QUIET_ZONE_MODULES) * PIXELS_PER_MODULE;
      const int firstPixelY = (moduleY + QUIET_ZONE_MODULES) * PIXELS_PER_MODULE;
      for (int y = 0; y < PIXELS_PER_MODULE; ++y)
      {
        unsigned char* pixel = pixels.data() + static_cast<std::size_t>(firstPixelY + y) * stride +
                               static_cast<std::size_t>(firstPixelX) * BYTES_PER_PIXEL;
        for (int x = 0; x < PIXELS_PER_MODULE; ++x)
        {
          pixel[0] = 0;
          pixel[1] = 0;
          pixel[2] = 0;
          pixel += BYTES_PER_PIXEL;
        }
      }
    }
  }
  return true;
}

void RemoveIfPresent(const std::string& path) noexcept
{
  if (!path.empty() && XFILE::CFile::Exists(path, false))
    XFILE::CFile::Delete(path);
}

bool IsReadablePng(const std::string& path)
{
  if (!XFILE::CFile::Exists(path, false))
    return false;

  XFILE::CFile file;
  if (!file.Open(path))
    return false;

  std::array<unsigned char, PNG_SIGNATURE.size()> signature{};
  const bool readable =
      file.Read(signature.data(), signature.size()) == static_cast<ssize_t>(signature.size());
  file.Close();
  return readable && std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), signature.begin());
}
} // namespace

bool JumpgateQrMatrix::IsDark(int x, int y) const noexcept
{
  if (x < 0 || x >= size || y < 0 || y >= size)
    return false;

  const std::size_t index =
      static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
  return index < modules.size() && modules[index] != 0;
}

bool CJumpgateQrCode::EncodeMatrix(std::string_view verificationUrl, JumpgateQrMatrix& matrix)
{
  matrix = {};
  if (verificationUrl.empty())
    return false;

  try
  {
    const std::vector<std::uint8_t> data(verificationUrl.begin(), verificationUrl.end());
    const qrcodegen::QrCode qr =
        qrcodegen::QrCode::encodeBinary(data, qrcodegen::QrCode::Ecc::MEDIUM);

    matrix.size = qr.getSize();
    matrix.modules.reserve(static_cast<std::size_t>(matrix.size) * matrix.size);
    for (int y = 0; y < matrix.size; ++y)
    {
      for (int x = 0; x < matrix.size; ++x)
        matrix.modules.push_back(qr.getModule(x, y) ? 1 : 0);
    }
    return true;
  }
  catch (const std::exception&)
  {
    matrix = {};
    return false;
  }
}

std::string CJumpgateQrCode::RenderPng(std::string_view verificationUrl)
{
  JumpgateQrMatrix matrix;
  if (!EncodeMatrix(verificationUrl, matrix))
    return {};

  int imageSize{0};
  std::vector<unsigned char> pixels;
  if (!RenderPixels(matrix, imageSize, pixels))
    return {};

  const std::string uniqueId = StringUtils::CreateUUID();
  if (uniqueId.empty())
    return {};
  const std::string pngPath = URIUtils::AddFileToFolder(
      "special://temp/", "jumpgate-pairing-" + uniqueId + ".png");

  const int stride = imageSize * BYTES_PER_PIXEL;
  if (!CPicture::CreateThumbnailFromSurface(pixels.data(), imageSize, imageSize, stride, pngPath))
  {
    RemoveIfPresent(pngPath);
    return {};
  }

  if (!IsReadablePng(pngPath))
  {
    RemoveIfPresent(pngPath);
    return {};
  }

  return pngPath;
}

std::string RenderJumpgatePairingQr(const std::string& verificationUrl)
{
  return CJumpgateQrCode::RenderPng(verificationUrl);
}
