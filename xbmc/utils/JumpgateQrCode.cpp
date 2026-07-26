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
#include "platform/Filesystem.h"
#include "qrcodegen.hpp"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <system_error>

using namespace KODI::JUMPGATE;

namespace
{
constexpr int QUIET_ZONE_MODULES = 4;
constexpr int PIXELS_PER_MODULE = 8;
constexpr int BYTES_PER_PIXEL = 4;

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

  std::error_code error;
  const std::string temporaryDirectory = KODI::PLATFORM::FILESYSTEM::temp_directory_path(error);
  const std::string uniqueId = StringUtils::CreateUUID();
  if (error || temporaryDirectory.empty() || uniqueId.empty())
    return {};
  const std::string pngPath =
      URIUtils::AddFileToFolder(temporaryDirectory, "jumpgate-pairing-" + uniqueId + ".png");

  const int stride = imageSize * BYTES_PER_PIXEL;
  if (!CPicture::CreateThumbnailFromSurface(pixels.data(), imageSize, imageSize, stride, pngPath))
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
