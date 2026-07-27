/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace KODI::JUMPGATE
{

struct JumpgateQrMatrix final
{
  [[nodiscard]] bool IsDark(int x, int y) const noexcept;

  int size{0};
  std::vector<std::uint8_t> modules;
};

class CJumpgateQrCode final
{
public:
  [[nodiscard]] static bool EncodeMatrix(std::string_view verificationUrl,
                                         JumpgateQrMatrix& matrix);
  [[nodiscard]] static std::string RenderPng(std::string_view verificationUrl);
};

} // namespace KODI::JUMPGATE

// The caller owns the generated path and must delete it after releasing its GUI texture.
[[nodiscard]] std::string RenderJumpgatePairingQr(const std::string& verificationUrl);
