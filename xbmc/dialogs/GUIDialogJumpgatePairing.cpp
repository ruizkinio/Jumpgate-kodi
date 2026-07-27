/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogJumpgatePairing.h"

#include "ServiceBroker.h"
#include "guilib/GUIImage.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIProgressControl.h"
#include "utils/JumpgatePairingCoordinator.h"
#include "utils/JumpgatePairingPresenter.h"
#include "utils/log.h"

namespace
{
constexpr int CONTROL_QR = 10;
constexpr int CONTROL_HEADING = 11;
constexpr int CONTROL_INSTRUCTION = 12;
constexpr int CONTROL_URL = 13;
constexpr int CONTROL_CODE = 14;
constexpr int CONTROL_COUNTDOWN = 15;
constexpr int CONTROL_STATUS = 16;
constexpr int CONTROL_PROGRESS = 17;
constexpr int CONTROL_CANCEL = 20;
constexpr int CONTROL_RETRY = 21;
} // namespace

CGUIDialogJumpgatePairing::CGUIDialogJumpgatePairing()
  : CGUIDialog(WINDOW_DIALOG_JUMPGATE_PAIRING,
               "special://xbmc/addons/script.jumpgate.manager/resources/skins/default/1080i/"
               "DialogJumpgatePairing.xml")
{
  SetCoordsRes(RESOLUTION_INFO{1920, 1080, 16.0f / 9.0f});
  m_loadType = KEEP_IN_MEMORY;
}

void CGUIDialogJumpgatePairing::SetCoordinator(
    std::shared_ptr<KODI::JUMPGATE::CJumpgatePairingCoordinator> coordinator)
{
  SetImage({});
  if (m_coordinator)
    m_coordinator->ReleasePendingQrArtifacts();
  m_coordinator = std::move(coordinator);
  m_lastRevision = 0;
  m_lastRemainingSeconds = -1;
}

bool CGUIDialogJumpgatePairing::OnMessage(CGUIMessage& message)
{
  if (message.GetMessage() == GUI_MSG_WINDOW_INIT)
  {
    if (!CGUIDialog::OnMessage(message))
      return false;
    UpdateView();
    return true;
  }
  if (message.GetMessage() == GUI_MSG_CLICKED)
  {
    if (message.GetSenderId() == CONTROL_CANCEL)
    {
      CancelAndClose();
      return true;
    }
    if (message.GetSenderId() == CONTROL_RETRY && m_coordinator)
    {
      if (!m_coordinator->Restart())
        CLog::Log(LOGWARNING, "Jumpgate pairing dialog could not restart coordinator");
      m_lastRevision = 0;
      m_lastRemainingSeconds = -1;
      UpdateView();
      return true;
    }
  }
  return CGUIDialog::OnMessage(message);
}

bool CGUIDialogJumpgatePairing::OnBack(int actionID)
{
  if (m_coordinator &&
      m_coordinator->GetSnapshot().stage == KODI::JUMPGATE::JumpgatePairingStage::Applying)
    return true;
  CancelAndClose();
  return true;
}

void CGUIDialogJumpgatePairing::OnDeinitWindow(int nextWindowID)
{
  if (m_coordinator)
  {
    const auto snapshot = m_coordinator->GetSnapshot();
    if (snapshot.stage != KODI::JUMPGATE::JumpgatePairingStage::Applied)
      m_coordinator->Cancel();
  }
  SetImage({});
  if (m_coordinator)
    m_coordinator->ReleasePendingQrArtifacts();
  m_coordinator.reset();
  m_lastRevision = 0;
  m_lastRemainingSeconds = -1;
  CGUIDialog::OnDeinitWindow(nextWindowID);
}

void CGUIDialogJumpgatePairing::Process(unsigned int currentTime, CDirtyRegionList& dirtyRegions)
{
  UpdateView();
  CGUIDialog::Process(currentTime, dirtyRegions);
}

void CGUIDialogJumpgatePairing::UpdateView()
{
  if (!m_coordinator)
    return;
  const auto snapshot = m_coordinator->GetSnapshot();
  if (snapshot.revision == m_lastRevision && snapshot.remainingSeconds == m_lastRemainingSeconds)
    return;
  m_lastRevision = snapshot.revision;
  m_lastRemainingSeconds = snapshot.remainingSeconds;
  const auto view = KODI::JUMPGATE::CJumpgatePairingPresenter::Build(snapshot);
  ApplyView(view);
  if (view.shouldClose)
    Close();
}

void CGUIDialogJumpgatePairing::ApplyView(const KODI::JUMPGATE::JumpgatePairingDialogView& view)
{
  SET_CONTROL_LABEL(CONTROL_HEADING, view.heading);
  SET_CONTROL_LABEL(CONTROL_INSTRUCTION, view.instruction);
  SET_CONTROL_LABEL(CONTROL_URL, view.verificationUrl);
  SET_CONTROL_LABEL(CONTROL_CODE, view.userCode);
  SET_CONTROL_LABEL(CONTROL_COUNTDOWN, view.countdown);
  SET_CONTROL_LABEL(CONTROL_STATUS, view.status);

  if (auto* progress = dynamic_cast<CGUIProgressControl*>(GetControl(CONTROL_PROGRESS)))
    progress->SetPercentage(static_cast<float>(view.remainingPercent));

  if (view.showQr)
  {
    SET_CONTROL_VISIBLE(CONTROL_QR);
    SetImage(view.qrImagePath);
  }
  else
  {
    SET_CONTROL_HIDDEN(CONTROL_QR);
    SetImage({});
    if (m_coordinator)
      m_coordinator->ReleasePendingQrArtifacts();
  }

  if (view.showCancel)
    SET_CONTROL_VISIBLE(CONTROL_CANCEL);
  else
    SET_CONTROL_HIDDEN(CONTROL_CANCEL);
  if (view.showRetry)
  {
    SET_CONTROL_VISIBLE(CONTROL_RETRY);
    SET_CONTROL_FOCUS(CONTROL_RETRY, 0);
  }
  else
    SET_CONTROL_HIDDEN(CONTROL_RETRY);
}

void CGUIDialogJumpgatePairing::CancelAndClose()
{
  if (m_coordinator)
  {
    m_coordinator->Cancel();
    if (m_coordinator->GetSnapshot().stage == KODI::JUMPGATE::JumpgatePairingStage::Applying)
      return;
  }
  Close();
}

void CGUIDialogJumpgatePairing::SetImage(const std::string& path)
{
  if (path == m_lastImage)
    return;
  std::string releasedPath = std::move(m_lastImage);
  auto* image = dynamic_cast<CGUIImage*>(GetControl(CONTROL_QR));
  if (image)
  {
    image->FreeResources(true);
    image->SetFileName(path, true, false);
  }
  m_lastImage = path;
  if (!releasedPath.empty() && m_coordinator)
    m_coordinator->ReleaseQrArtifact(releasedPath);
}
