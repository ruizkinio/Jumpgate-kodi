/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/PlayerOpenPublication.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

enum class RemoteState
{
  PLAYING,
  STOPPING,
  STOPPED,
};

struct FakePlaybackItem
{
  uint64_t token;
};

class FakeRenderer
{
public:
  explicit FakeRenderer(std::vector<std::string>& trace) : m_trace(trace) {}

  bool GetPositionInfo()
  {
    m_trace.emplace_back("get-position");
    return positionRequestSucceeds;
  }

  bool GetMediaInfo()
  {
    m_trace.emplace_back("get-media");
    return mediaRequestSucceeds;
  }

  bool Stop()
  {
    m_trace.emplace_back("stop");
    if (!stopRequestSucceeds)
      return false;

    state = stopReachesTerminalState ? RemoteState::STOPPED : RemoteState::STOPPING;
    return true;
  }

  bool VerifyStopped()
  {
    m_trace.emplace_back("verify-stopped");
    return state == RemoteState::STOPPED;
  }

  bool positionRequestSucceeds{true};
  bool mediaRequestSucceeds{true};
  bool stopRequestSucceeds{true};
  bool stopReachesTerminalState{true};
  RemoteState state{RemoteState::PLAYING};

private:
  std::vector<std::string>& m_trace;
};

struct CallbackEvent
{
  std::string name;
  uint64_t token;
};

class FakePlayerCallbacks
{
public:
  explicit FakePlayerCallbacks(std::vector<std::string>& trace) : m_trace(trace) {}

  void RetainLocalOwnership(const FakePlaybackItem& item)
  {
    m_trace.emplace_back("retain-local");
    ownsRemote = true;
    retainedToken = item.token;
  }

  void OnPlayBackStarted(const FakePlaybackItem& item)
  {
    m_trace.emplace_back("started");
    events.push_back({"started", item.token});
  }

  void OnAVStarted(const FakePlaybackItem& item)
  {
    m_trace.emplace_back("av-started");
    events.push_back({"av-started", item.token});
  }

  bool ownsRemote{false};
  uint64_t retainedToken{0};
  std::vector<CallbackEvent> events;
  std::size_t terminalCallbacks{0};

private:
  std::vector<std::string>& m_trace;
};

bool OpenAgainstRenderer(FakeRenderer& renderer,
                         FakePlayerCallbacks& callbacks,
                         uint64_t token,
                         bool rollbackAllowed = true)
{
  const FakePlaybackItem item{token};
  return KODI::PLAYER::PrepareOpenAndPublishStarted(
      item, rollbackAllowed, [&] { return renderer.GetPositionInfo(); },
      [&] { return renderer.GetMediaInfo(); }, [&] { return renderer.Stop(); },
      [&] { return renderer.VerifyStopped(); },
      [&](const FakePlaybackItem& callbackItem) { callbacks.RetainLocalOwnership(callbackItem); },
      [&](const FakePlaybackItem& callbackItem) { callbacks.OnPlayBackStarted(callbackItem); },
      [&](const FakePlaybackItem& callbackItem) { callbacks.OnAVStarted(callbackItem); });
}

void ExpectStartedCallbacks(const FakePlayerCallbacks& callbacks, uint64_t token)
{
  ASSERT_EQ(callbacks.events.size(), 2u);
  EXPECT_EQ(callbacks.events[0].name, "started");
  EXPECT_EQ(callbacks.events[0].token, token);
  EXPECT_EQ(callbacks.events[1].name, "av-started");
  EXPECT_EQ(callbacks.events[1].token, token);
  EXPECT_EQ(callbacks.terminalCallbacks, 0u);
}

} // namespace

TEST(TestUPnPPlayerOpenPublication, PositionFailureWithVerifiedStopReturnsFalseAndStopsRemote)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};
  renderer.positionRequestSucceeds = false;

  EXPECT_FALSE(OpenAgainstRenderer(renderer, callbacks, 7001));
  EXPECT_EQ(renderer.state, RemoteState::STOPPED);
  EXPECT_FALSE(callbacks.ownsRemote);
  EXPECT_TRUE(callbacks.events.empty());
  EXPECT_EQ(callbacks.terminalCallbacks, 0u);
  EXPECT_EQ(trace, (std::vector<std::string>{"get-position", "stop", "verify-stopped"}));
}

TEST(TestUPnPPlayerOpenPublication, MediaFailureWithVerifiedStopReturnsFalseAndStopsRemote)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};
  renderer.mediaRequestSucceeds = false;

  EXPECT_FALSE(OpenAgainstRenderer(renderer, callbacks, 7002));
  EXPECT_EQ(renderer.state, RemoteState::STOPPED);
  EXPECT_FALSE(callbacks.ownsRemote);
  EXPECT_TRUE(callbacks.events.empty());
  EXPECT_EQ(callbacks.terminalCallbacks, 0u);
  EXPECT_EQ(trace,
            (std::vector<std::string>{"get-position", "get-media", "stop", "verify-stopped"}));
}

TEST(TestUPnPPlayerOpenPublication, StopFailureRetainsPlayingRemoteAndPublishesExactToken)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};
  renderer.positionRequestSucceeds = false;
  renderer.stopRequestSucceeds = false;

  EXPECT_TRUE(OpenAgainstRenderer(renderer, callbacks, 7003));
  EXPECT_EQ(renderer.state, RemoteState::PLAYING);
  EXPECT_TRUE(callbacks.ownsRemote);
  EXPECT_EQ(callbacks.retainedToken, 7003u);
  ExpectStartedCallbacks(callbacks, 7003);
  EXPECT_EQ(trace, (std::vector<std::string>{"get-position", "stop", "retain-local", "started",
                                             "av-started"}));
}

TEST(TestUPnPPlayerOpenPublication, UnverifiedStopRetainsRemoteAndPublishesExactToken)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};
  renderer.mediaRequestSucceeds = false;
  renderer.stopReachesTerminalState = false;

  EXPECT_TRUE(OpenAgainstRenderer(renderer, callbacks, 7004));
  EXPECT_EQ(renderer.state, RemoteState::STOPPING);
  EXPECT_TRUE(callbacks.ownsRemote);
  EXPECT_EQ(callbacks.retainedToken, 7004u);
  ExpectStartedCallbacks(callbacks, 7004);
  EXPECT_EQ(trace, (std::vector<std::string>{"get-position", "get-media", "stop", "verify-stopped",
                                             "retain-local", "started", "av-started"}));
}

TEST(TestUPnPPlayerOpenPublication, AttachMetadataFailureDoesNotStopPreexistingPlayback)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};
  renderer.positionRequestSucceeds = false;

  EXPECT_TRUE(OpenAgainstRenderer(renderer, callbacks, 7005, false));
  EXPECT_EQ(renderer.state, RemoteState::PLAYING);
  EXPECT_TRUE(callbacks.ownsRemote);
  EXPECT_EQ(callbacks.retainedToken, 7005u);
  ExpectStartedCallbacks(callbacks, 7005);
  EXPECT_EQ(trace,
            (std::vector<std::string>{"get-position", "retain-local", "started", "av-started"}));
}

TEST(TestUPnPPlayerOpenPublication, NormalSuccessPublishesCallbacksOnceInKodiOrder)
{
  std::vector<std::string> trace;
  FakeRenderer renderer{trace};
  FakePlayerCallbacks callbacks{trace};

  EXPECT_TRUE(OpenAgainstRenderer(renderer, callbacks, 7006));
  EXPECT_EQ(renderer.state, RemoteState::PLAYING);
  EXPECT_TRUE(callbacks.ownsRemote);
  EXPECT_EQ(callbacks.retainedToken, 7006u);
  ExpectStartedCallbacks(callbacks, 7006);
  EXPECT_EQ(trace, (std::vector<std::string>{"get-position", "get-media", "retain-local", "started",
                                             "av-started"}));
}
