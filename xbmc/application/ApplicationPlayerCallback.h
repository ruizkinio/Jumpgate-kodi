/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/IPlayerCallback.h"
#include "threads/Event.h"
#include "utils/JumpgatePlaybackAttemptState.h"

#include <cstdint>
#include <optional>

class CApplicationStackHelper;
class CFileItem;

class CApplicationPlayerCallback : public IPlayerCallback
{
public:
  CApplicationPlayerCallback() = default;

  void OnPlayBackEnded() override;
  void OnPlayBackEndedWithItem(const CFileItem& file) override;
  void OnPlayBackOpening(const CFileItem& file, bool deferred = false) override;
  void OnPlayBackOpenNext(const CFileItem& file) override;
  void OnPlayBackOpenFailed(const CFileItem& file) override;
  void OnPlayBackStarted(const CFileItem& file) override;
  void OnPlayerCloseFile(const CFileItem& file, const CBookmark& bookmark) override;
  void OnPlayBackPaused() override;
  void OnPlayBackResumed() override;
  void OnPlayBackStopped() override;
  void OnPlayBackStoppedWithItem(const CFileItem& file) override;
  void OnPlayBackError() override;
  void OnPlayBackErrorWithItem(const CFileItem& file) override;
  void OnQueueNextItem() override;
  void OnPlayBackSeek(int64_t iTime, int64_t seekOffset) override;
  void OnPlayBackSeekChapter(int iChapter) override;
  void OnPlayBackSpeedChanged(int iSpeed) override;
  void OnAVChange() override;
  void OnAVStarted(const CFileItem& file) override;
  void RequestVideoSettings(const CFileItem& fileItem) override;
  void StoreVideoSettings(const CFileItem& fileItem, const CVideoSettings& vs) override;

  std::optional<KODI::JUMPGATE::JumpgatePlaybackTerminal> AcknowledgePlaybackTerminal(
      uint64_t token, bool completed);
  bool IsPlaybackAttemptSuperseded(uint64_t token) const;

private:
  static uint64_t GetPlaybackToken(const CFileItem& file);
  std::optional<KODI::JUMPGATE::JumpgatePlaybackTerminal> CreatePlaybackTerminal(
      bool completed, const CFileItem* file);
  static void SendPlaybackTerminal(const KODI::JUMPGATE::JumpgatePlaybackTerminal& terminal);
  void SendPlaybackError(const CFileItem* file);

  KODI::JUMPGATE::CJumpgatePlaybackAttemptState m_playbackAttempts;
};
