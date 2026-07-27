/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/GUIDialog.h"

#include <cstdint>
#include <memory>
#include <string>

namespace KODI::JUMPGATE
{
class CJumpgatePairingCoordinator;
struct JumpgatePairingDialogView;
} // namespace KODI::JUMPGATE

class CGUIDialogJumpgatePairing final : public CGUIDialog
{
public:
  CGUIDialogJumpgatePairing();
  ~CGUIDialogJumpgatePairing() override = default;

  void SetCoordinator(std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> coordinator);
  bool OnMessage(CGUIMessage& message) override;
  bool OnBack(int actionID) override;

protected:
  void OnDeinitWindow(int nextWindowID) override;
  void Process(unsigned int currentTime, CDirtyRegionList& dirtyRegions) override;

private:
  void UpdateView();
  void ApplyView(const KODI::JUMPGATE::JumpgatePairingDialogView& view);
  void CancelAndClose();
  void SetImage(const std::string& path);

  std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> m_coordinator;
  std::uint64_t m_lastRevision{0};
  int m_lastRemainingSeconds{-1};
  std::string m_lastImage;
};
