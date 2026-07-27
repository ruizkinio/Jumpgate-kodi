/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIUserMessages.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIWindowManager.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

TEST(TestJumpgateGuiMessageRemoval, RemovedPlaybackTerminalsPreserveOrderAndExactTokens)
{
  CGUIWindowManager windowManager;
  windowManager.SendThreadMessage(CGUIMessage{GUI_MSG_PLAYBACK_STARTED, 0, 0, 7001});
  windowManager.SendThreadMessage(CGUIMessage{GUI_MSG_PLAYBACK_STOPPED, 0, 0, 7001, 1});
  windowManager.SendThreadMessage(CGUIMessage{GUI_MSG_PLAYBACK_ENDED, 0, 0, 7002, 1});

  static constexpr int terminalMessageIds[]{GUI_MSG_PLAYBACK_STOPPED, GUI_MSG_PLAYBACK_ENDED, 0};
  std::vector<CGUIMessage> removed;
  ASSERT_EQ(windowManager.RemoveThreadMessageByMessageIds(terminalMessageIds, &removed), 2);
  ASSERT_EQ(removed.size(), 2u);
  EXPECT_EQ(removed[0].GetMessage(), GUI_MSG_PLAYBACK_STOPPED);
  EXPECT_EQ(removed[0].GetParam1AsI64(), static_cast<int64_t>(7001));
  EXPECT_EQ(removed[1].GetMessage(), GUI_MSG_PLAYBACK_ENDED);
  EXPECT_EQ(removed[1].GetParam1AsI64(), static_cast<int64_t>(7002));

  static constexpr int startedMessageIds[]{GUI_MSG_PLAYBACK_STARTED, 0};
  EXPECT_EQ(windowManager.RemoveThreadMessageByMessageIds(startedMessageIds), 1);
  EXPECT_EQ(windowManager.RemoveThreadMessageByMessageIds(terminalMessageIds), 0);
}
