/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/JumpgateProfileStore.h"

#include <androidjni/jutils-details.hpp>

class CJNIContext;

namespace KODI::JUMPGATE
{

class CAndroidJumpgateCredentialStore final : public IJumpgateCredentialStore
{
public:
  explicit CAndroidJumpgateCredentialStore(const CJNIContext& context);

  bool Store(const std::string& profileId,
             const std::string& deviceId,
             const std::string& secretJson,
             std::string& credentialRef,
             std::string& error) override;
  bool Load(const std::string& profileId,
            const std::string& deviceId,
            const std::string& credentialRef,
            std::string& secretJson,
            std::string& error) override;
  bool Remove(const std::string& credentialRef, std::string& error) override;

private:
  jni::jhobject m_context;
};

} // namespace KODI::JUMPGATE
