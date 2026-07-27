/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

class CVariant;

namespace KODI
{
namespace UTILITY
{

class CJumpgateSourceFingerprint final
{
public:
  static bool FingerprintExactUrl(std::string_view url, std::string& fingerprint);

  static bool FingerprintStream(const CVariant& stream, std::vector<std::string>& fingerprints);
  static bool FingerprintStream(const CVariant& stream,
                                const std::vector<std::string>& extraFingerprints,
                                std::vector<std::string>& fingerprints);

  // Decodes only known Stremio loopback route shapes. Unknown loopback URLs fail closed.
  static bool FingerprintPlaybackUrl(std::string_view playbackUrl,
                                     std::vector<std::string>& fingerprints);
};

} // namespace UTILITY
} // namespace KODI
