/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/JumpgateProfileStore.h"

#include <string>

namespace KODI::JUMPGATE
{

class CJumpgateProfileStorage final : public IJumpgateProfileStorage
{
public:
  explicit CJumpgateProfileStorage(std::string specialPath);

  bool Read(std::string& contents, bool& exists, std::string& error) override;
  bool WriteAtomic(const std::string& contents, std::string& error) override;

private:
  std::string m_specialPath;
};

} // namespace KODI::JUMPGATE
