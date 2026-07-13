/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace
{

std::filesystem::path FindSourceFile()
{
  const std::filesystem::path compiledPath{__FILE__};
  if (compiledPath.is_absolute() && std::filesystem::exists(compiledPath))
    return compiledPath;

  for (std::filesystem::path base = std::filesystem::current_path(); !base.empty();
       base = base.parent_path())
  {
    const std::filesystem::path candidate = base / compiledPath;
    if (std::filesystem::exists(candidate))
      return candidate;
    if (base == base.root_path())
      break;
  }
  return {};
}

std::string ReadKodiSource(const std::filesystem::path& relativePath)
{
  const std::filesystem::path testSource = FindSourceFile();
  if (testSource.empty())
    return {};
  const std::filesystem::path xbmcRoot = testSource.parent_path().parent_path().parent_path();
  std::ifstream input{xbmcRoot / relativePath, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string FunctionSection(const std::string& source,
                            const std::string& beginMarker,
                            const std::string& endMarker)
{
  const std::size_t begin = source.find(beginMarker);
  if (begin == std::string::npos)
    return {};
  const std::size_t end = source.find(endMarker, begin + beginMarker.size());
  return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string FunctionBody(const std::string& source, const std::string& symbol)
{
  const std::size_t declaration = source.find(symbol);
  if (declaration == std::string::npos)
    return {};

  const std::size_t body = source.find('{', declaration + symbol.size());
  if (body == std::string::npos)
    return {};

  std::size_t depth = 0;
  for (std::size_t position = body; position < source.size(); ++position)
  {
    if (source[position] == '{')
      ++depth;
    else if (source[position] == '}' && --depth == 0)
      return source.substr(declaration, position - declaration + 1);
  }
  return {};
}

std::size_t Count(const std::string& source, const std::string& needle)
{
  std::size_t count = 0;
  for (std::size_t position = 0; (position = source.find(needle, position)) != std::string::npos;
       position += needle.size())
  {
    ++count;
  }
  return count;
}

} // namespace

TEST(TestJumpgateApplicationLifecycleStatic, DrainsWorkersBeforeKodiServicesOnEveryRunExit)
{
  const std::string application = ReadKodiSource("application/Application.cpp");
  const std::string platform = ReadKodiSource("platform/xbmc.cpp");
  ASSERT_FALSE(application.empty());
  ASSERT_FALSE(platform.empty());

  const std::string cleanup =
      FunctionSection(application, "bool CApplication::Cleanup()", "bool CApplication::Stop(");
  const std::string stop =
      FunctionSection(application, "bool CApplication::Stop(", "void CApplication::SetRenderGUI(");
  ASSERT_FALSE(cleanup.empty());
  ASSERT_FALSE(stop.empty());
  ASSERT_NE(cleanup.find("CXBMCApp::Get().Deinitialize();"), std::string::npos);
  ASSERT_NE(cleanup.find("CServiceBroker::UnregisterDNSNameCache();"), std::string::npos);
  EXPECT_LT(cleanup.find("CXBMCApp::Get().Deinitialize();"),
            cleanup.find("CServiceBroker::UnregisterDNSNameCache();"));
  ASSERT_NE(stop.find("CXBMCApp::Get().Deinitialize();"), std::string::npos);
  ASSERT_NE(stop.find("CServiceBroker::UnregisterAppPort();"), std::string::npos);
  EXPECT_LT(stop.find("CXBMCApp::Get().Deinitialize();"),
            stop.find("CServiceBroker::UnregisterAppPort();"));

  const std::size_t guard = platform.find("CAndroidJumpgateShutdownGuard jumpgateShutdownGuard;");
  const std::size_t create = platform.find("g_application.Create()");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(create, std::string::npos);
  EXPECT_LT(guard, create);
  EXPECT_NE(platform.find("~CAndroidJumpgateShutdownGuard()"), std::string::npos);
  EXPECT_NE(platform.find("CXBMCApp::Get().Deinitialize();"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     ExternalTerminalIsCommittedBeforeAnnouncementAndDelivery)
{
  const std::string handling = ReadKodiSource("application/ApplicationMessageHandling.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string callbacks = ReadKodiSource("application/ApplicationPlayerCallback.cpp");
  const std::string videoPlayer = ReadKodiSource("cores/VideoPlayer/VideoPlayer.cpp");
  const std::string upnpPlayer = ReadKodiSource("network/upnp/UPnPPlayer.cpp");
  ASSERT_FALSE(handling.empty());
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(callbacks.empty());
  ASSERT_FALSE(videoPlayer.empty());
  ASSERT_FALSE(upnpPlayer.empty());

  const std::string stopped =
      FunctionSection(handling, "case GUI_MSG_PLAYBACK_STOPPED:", "case GUI_MSG_PLAYBACK_ENDED:");
  const std::string ended = FunctionSection(
      handling, "case GUI_MSG_PLAYBACK_ENDED:", "case GUI_MSG_PLAYLISTPLAYER_STOPPED:");
  const std::string started =
      FunctionSection(handling, "case GUI_MSG_PLAYBACK_STARTED:", "case GUI_MSG_QUEUE_NEXT_ITEM:");
  ASSERT_FALSE(stopped.empty());
  ASSERT_FALSE(ended.empty());
  ASSERT_FALSE(started.empty());
  ASSERT_NE(stopped.find("CommitExternalPlaybackTerminal("), std::string::npos);
  ASSERT_NE(ended.find("CommitExternalPlaybackTerminal("), std::string::npos);
  EXPECT_LT(stopped.find("CommitExternalPlaybackTerminal("), stopped.find("\"OnStop\""));
  EXPECT_LT(stopped.find("CommitExternalPlaybackTerminal("),
            stopped.find("DeliverPendingExternalPlayerResult();"));
  EXPECT_LT(ended.find("CommitExternalPlaybackTerminal("), ended.find("\"OnStop\""));
  EXPECT_LT(ended.find("CommitExternalPlaybackTerminal("), ended.find("PlayNext(1, true)"));

  const std::string terminal = FunctionSection(
      app,
      "void CXBMCApp::CommitExternalPlaybackTerminal(bool completed, uint64_t token, bool started)",
      "void CXBMCApp::DeliverPendingExternalPlayerResult()");
  const std::string announcedStop =
      FunctionSection(app, "void CXBMCApp::OnPlayBackStopped(bool completed)",
                      "void CXBMCApp::CommitExternalPlaybackTerminal(bool completed,");
  const std::string errorCallback =
      FunctionSection(callbacks, "void CApplicationPlayerCallback::OnPlayBackError()",
                      "void CApplicationPlayerCallback::OnQueueNextItem()");
  ASSERT_FALSE(terminal.empty());
  ASSERT_FALSE(announcedStop.empty());
  ASSERT_FALSE(errorCallback.empty());
  EXPECT_NE(terminal.find("CommitPlaybackTerminal(token, started)"), std::string::npos);
  EXPECT_NE(terminal.find("HasNewerPlayback(terminal->token)"), std::string::npos);
  EXPECT_LT(terminal.find("m_playbackResultState.Capture("),
            terminal.find("m_playbackResultState.Finish("));
  EXPECT_EQ(terminal.find("m_pendingPlaybackResult"), std::string::npos);
  EXPECT_EQ(announcedStop.find("CommitPlaybackStopped()"), std::string::npos);
  EXPECT_EQ(announcedStop.find("m_playbackResultState.Finish("), std::string::npos);
  EXPECT_NE(errorCallback.find("GUI_MSG_PLAYBACK_ERROR"), std::string::npos);
  EXPECT_NE(errorCallback.find("CreatePlaybackTerminal(false, file)"), std::string::npos);
  EXPECT_EQ(Count(callbacks, "m_playbackAttempts.EmitTerminal("), 1u);
  EXPECT_NE(callbacks.find("m_playbackAttempts.MarkStarted("), std::string::npos);
  EXPECT_NE(callbacks.find("m_playbackAttempts.IsSuperseded("), std::string::npos);
  EXPECT_NE(stopped.find("terminal->superseded"), std::string::npos);
  EXPECT_LT(stopped.find("terminal->superseded"), stopped.find("\"OnStop\""));
  EXPECT_NE(stopped.find("m_app.PlaybackCleanup();"), std::string::npos);
  EXPECT_NE(videoPlayer.find("cb->OnPlayBackStoppedWithItem(fileItem);"), std::string::npos);
  EXPECT_NE(videoPlayer.find("cb->OnPlayBackErrorWithItem(fileItem);"), std::string::npos);
  EXPECT_NE(videoPlayer.find("cb->OnPlayBackEndedWithItem(fileItem);"), std::string::npos);
  EXPECT_NE(upnpPlayer.find("m_callback.OnPlayBackStoppedWithItem(*callbackFile);"),
            std::string::npos);
  EXPECT_NE(upnpPlayer.find("m_callback.OnPlayBackEndedWithItem(*callbackFile);"),
            std::string::npos);
  EXPECT_EQ(upnpPlayer.find("m_callback.OnPlayBackStopped();"), std::string::npos);
  EXPECT_EQ(upnpPlayer.find("m_callback.OnPlayBackEnded();"), std::string::npos);
  const std::string upnpOpen = FunctionBody(upnpPlayer, "bool CUPnPPlayer::OpenFile(");
  ASSERT_FALSE(upnpOpen.empty());
  EXPECT_NE(upnpOpen.find("PrepareOpenAndPublishStarted("), std::string::npos);
  EXPECT_NE(upnpOpen.find("file, !file.GetPath().empty()"), std::string::npos);
  EXPECT_NE(upnpOpen.find("StopRemotePlayback()"), std::string::npos);
  EXPECT_NE(upnpOpen.find("VerifyRemotePlaybackStopped()"), std::string::npos);
  EXPECT_LT(upnpOpen.find("GetPositionInfo("), upnpOpen.find("OnPlayBackStarted(callbackFile)"));
  EXPECT_LT(upnpOpen.find("GetMediaInfo("), upnpOpen.find("OnPlayBackStarted(callbackFile)"));
  EXPECT_LT(upnpOpen.find("OnPlayBackStarted(callbackFile)"),
            upnpOpen.find("OnAVStarted(callbackFile)"));
  EXPECT_NE(callbacks.find("CApplicationPlayerCallback::OnPlayBackOpening"), std::string::npos);
  EXPECT_NE(started.find("message.GetParam1AsI64()"), std::string::npos);
  EXPECT_NE(app.find("CommitPlaybackStarted(continuationGeneration, token)"), std::string::npos);
  EXPECT_NE(app.find("OnPlayBackStarted(true);"), std::string::npos);
  EXPECT_NE(app.find("authorityTransaction.CommitPlaybackResumed()"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic, DeferredOpenFailureWiringPreservesExactAdmission)
{
  const std::string application = ReadKodiSource("application/Application.cpp");
  const std::string applicationPlayer = ReadKodiSource("application/ApplicationPlayer.cpp");
  const std::string callbacks = ReadKodiSource("application/ApplicationPlayerCallback.cpp");
  const std::string playlist = ReadKodiSource("PlayListPlayer.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(application.empty());
  ASSERT_FALSE(applicationPlayer.empty());
  ASSERT_FALSE(callbacks.empty());
  ASSERT_FALSE(playlist.empty());
  ASSERT_FALSE(app.empty());

  const std::string playFile = FunctionSection(application, "bool CApplication::PlayFile(",
                                               "void CApplication::PlaybackCleanup()");
  const std::string mediaMessage =
      FunctionSection(playlist, "case TMSG_MEDIA_PLAY:", "case TMSG_MEDIA_RESTART:");
  const std::string intent = FunctionSection(app, "void CXBMCApp::onNewIntent(CJNIIntent intent)",
                                             "void CXBMCApp::onActivityResult(");
  const std::string failure =
      FunctionSection(app, "void CXBMCApp::CommitExternalPlaybackAdmissionFailure(uint64_t token)",
                      "void CXBMCApp::ProcessPlaybackSourceClaim()");
  const std::string playbackCleanup =
      FunctionBody(application, "void CApplication::PlaybackCleanup()");
  const std::string openNext = FunctionBody(
      applicationPlayer, "CApplicationPlayer::OpenNextResult CApplicationPlayer::OpenNext");
  const std::string openFile =
      FunctionBody(applicationPlayer, "bool CApplicationPlayer::OpenFileInternal(");
  const std::string openFailure =
      FunctionBody(callbacks, "void CApplicationPlayerCallback::OnPlayBackOpenFailed(");
  const std::string rejection =
      FunctionSection(app, "void CXBMCApp::DeliverRejectedExternalPlaybackResult(",
                      "void CXBMCApp::ProcessPlaybackSourceClaim()");
  ASSERT_FALSE(playFile.empty());
  ASSERT_FALSE(mediaMessage.empty());
  ASSERT_FALSE(intent.empty());
  ASSERT_FALSE(failure.empty());
  ASSERT_FALSE(playbackCleanup.empty());
  ASSERT_FALSE(openNext.empty());
  ASSERT_FALSE(openFile.empty());
  ASSERT_FALSE(openFailure.empty());
  ASSERT_FALSE(rejection.empty());
  EXPECT_NE(playFile.find("const bool opened = appPlayer->OpenFile("), std::string::npos);
  EXPECT_NE(playFile.find("if (!opened && playbackItem.HasProperty(\"jumpgate.playback_token\"))"),
            std::string::npos);
  EXPECT_NE(playFile.find("return !item.HasProperty(\"jumpgate.playback_token\");"),
            std::string::npos);
  EXPECT_NE(mediaMessage.find("pMsg->SetResult(opened ? 1 : 0);"), std::string::npos);
  EXPECT_NE(mediaMessage.find("if (jumpgateAdmission)"), std::string::npos);
  EXPECT_NE(intent.find("GetAppMessenger()->SendMsg("), std::string::npos);
  EXPECT_NE(intent.find("ACTION_VIEW && targetFile.empty()"), std::string::npos);
  EXPECT_NE(intent.find("static_cast<void*>(item.release())"), std::string::npos);
  EXPECT_NE(intent.find("item->SetProperty(\"jumpgate.playback_token\""), std::string::npos);
  EXPECT_NE(intent.find("CommitExternalPlaybackAdmissionFailure(admissionToken);"),
            std::string::npos);
  EXPECT_NE(failure.find("CancelPendingAdmissionByToken(token)"), std::string::npos);
  EXPECT_EQ(failure.find("CancelPendingAdmission(generation)"), std::string::npos);
  EXPECT_NE(failure.find("m_playbackResultState.Finish(canceled->generation, false)"),
            std::string::npos);
  EXPECT_LT(openFile.find("callback.OnPlayBackOpening(item)"),
            openFile.find("player->OpenFile(item, options)"));
  EXPECT_LT(openFile.find("callback.OnPlayBackOpening(item, true)"), openFile.find("CloseFile();"));
  EXPECT_NE(openFile.find("callback.OnPlayBackOpenFailed(item)"), std::string::npos);
  EXPECT_NE(openNext.find("OpenNextResult::Failed"), std::string::npos);
  EXPECT_NE(openNext.find("OnPlayBackOpenNext(*nextItem.pItem)"), std::string::npos);
  EXPECT_NE(openNext.find("BeginDeferredOpenAndOpen("), std::string::npos);
  EXPECT_NE(playbackCleanup.find("CApplicationPlayer::OpenNextResult::Failed"), std::string::npos);
  EXPECT_NE(playbackCleanup.find("DeliverPendingExternalPlayerResult()"), std::string::npos);
  EXPECT_NE(openFailure.find("CommitExternalPlaybackOpenFailure(token)"), std::string::npos);
  EXPECT_NE(playbackCleanup.find("OpenNext(m_ServiceManager->GetPlayerCoreFactory())"),
            std::string::npos);
  EXPECT_NE(playFile.find("removedMessages"), std::string::npos);
  EXPECT_NE(playFile.find("AcknowledgePlaybackTerminal("), std::string::npos);
  EXPECT_EQ(playFile.find("TakeUnacknowledgedPlaybackTerminals()"), std::string::npos);
  EXPECT_NE(
      intent.find("DeliverRejectedExternalPlaybackResult(resultRequestId, lifecycleOperation);"),
      std::string::npos);
  EXPECT_LT(rejection.find("m_playbackResultState.Finish(generation, 0, 0, false)"),
            rejection.find("call_method<void>(m_context, \"exitExternalPlayerMode\""));
}

TEST(TestJumpgateApplicationLifecycleStatic, AndroidExternalResultOwnerSurvivesWarmAndColdTasks)
{
  const std::string manifest =
      ReadKodiSource("../tools/android/packaging/xbmc/AndroidManifest.xml.in");
  const std::string splash = ReadKodiSource("../tools/android/packaging/xbmc/src/Splash.java.in");
  const std::string main = ReadKodiSource("../tools/android/packaging/xbmc/src/Main.java.in");
  const std::string activity =
      ReadKodiSource("../tools/android/packaging/xbmc/src/ExternalPlayerActivity.java.in");
  const std::string coordinator =
      ReadKodiSource("../tools/android/packaging/xbmc/src/ExternalPlayerResultCoordinator.java.in");
  const std::string store =
      ReadKodiSource("../tools/android/packaging/xbmc/src/ExternalPlayerResultStore.java.in");
  ASSERT_FALSE(manifest.empty());
  ASSERT_FALSE(splash.empty());
  ASSERT_FALSE(main.empty());
  ASSERT_FALSE(activity.empty());
  ASSERT_FALSE(coordinator.empty());
  ASSERT_FALSE(store.empty());

  const std::string deliver = FunctionBody(activity, "synchronized boolean deliver(");
  const std::string setTerminalResult = FunctionBody(activity, "public void setTerminalResult(");
  const std::string finishOwner = FunctionBody(activity, "public void finishOwner(");
  const std::string activityAcknowledge = FunctionBody(activity, "public boolean acknowledge(");
  const std::string fallback = FunctionBody(activity, "private void scheduleProcessExitFallback(");
  const std::string start = FunctionBody(splash, "protected void startXBMC()");
  const std::string exit = FunctionBody(main, "public synchronized void exitExternalPlayerMode(");
  const std::string acknowledge =
      FunctionBody(main, "private synchronized boolean acknowledgeExternalPlayerResultInternal(");
  const std::string executeExit =
      FunctionBody(store, "public static synchronized boolean executeAcknowledgedProcessExit(");
  ASSERT_FALSE(deliver.empty());
  ASSERT_FALSE(setTerminalResult.empty());
  ASSERT_FALSE(finishOwner.empty());
  ASSERT_FALSE(activityAcknowledge.empty());
  ASSERT_FALSE(fallback.empty());
  ASSERT_FALSE(start.empty());
  ASSERT_FALSE(exit.empty());
  ASSERT_FALSE(acknowledge.empty());
  ASSERT_FALSE(executeExit.empty());

  EXPECT_EQ(Count(manifest, "<action android:name=\"android.intent.action.VIEW\""), 1u);
  EXPECT_NE(manifest.find("android:name=\".ExternalPlayerActivity\""), std::string::npos);
  EXPECT_NE(manifest.find("android:finishOnTaskLaunch=\"false\""), std::string::npos);
  EXPECT_NE(manifest.find("android:launchMode=\"standard\""), std::string::npos);
  EXPECT_NE(manifest.find("android:launchMode=\"singleTask\""), std::string::npos);
  EXPECT_NE(start.find("startActivity(intent);"), std::string::npos);
  EXPECT_EQ(start.find("startActivityForResult"), std::string::npos);
  EXPECT_NE(start.find("Intent.FLAG_GRANT_READ_URI_PERMISSION"), std::string::npos);
  EXPECT_LT(deliver.find("mHost.setTerminalResult(terminal);"),
            deliver.find("mHost.finishOwner();"));
  EXPECT_NE(setTerminalResult.find("setResult("), std::string::npos);
  EXPECT_NE(finishOwner.find("finish();"), std::string::npos);
  EXPECT_LT(activityAcknowledge.find("ExternalPlayerResultStore.acknowledgeExit("),
            activityAcknowledge.find("Main.acknowledgeExternalPlayerResult("));
  EXPECT_LT(activityAcknowledge.find("Main.acknowledgeExternalPlayerResult("),
            activityAcknowledge.find("scheduleProcessExitFallback("));
  EXPECT_NE(fallback.find("ExternalPlayerResultStore.executeAcknowledgedProcessExit("),
            std::string::npos);
  EXPECT_EQ(exit.find("setResult("), std::string::npos);
  EXPECT_NE(exit.find("ExternalPlayerResultStore.publish(this, terminal)"), std::string::npos);
  EXPECT_NE(acknowledge.find("mExternalResultProducer.acknowledgeExit("), std::string::npos);
  EXPECT_LT(executeExit.find("processExitState.claim("), executeExit.find("editor.commit()"));
  EXPECT_LT(executeExit.find("editor.commit()"), executeExit.find("terminator.run();"));
  EXPECT_NE(coordinator.find("claimProcessExit("), std::string::npos);
  EXPECT_NE(coordinator.find("claim(Terminal terminal)"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic, AuthorityLocksDoNotSpanAnnouncerReconfiguration)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(app.empty());

  const std::string begin =
      FunctionBody(app, "BeginJumpgateProfileAuthorityTransition(std::string& error)");
  const std::string prepare =
      FunctionBody(app, "CXBMCApp::PrepareJumpgateProfileAuthorityTransition()");
  const std::string finish =
      FunctionBody(app, "CXBMCApp::FinishJumpgateProfileAuthorityTransition");
  const std::string admission = FunctionBody(app, "BeginExternalPlaybackAdmission(");
  const std::string intent = FunctionBody(app, "void CXBMCApp::onNewIntent(CJNIIntent intent)");
  const std::string delivery =
      FunctionSection(app, "void CXBMCApp::DeliverPendingExternalPlayerResult(\n",
                      "void CXBMCApp::ExitExternalPlayerMode");
  ASSERT_FALSE(begin.empty());
  ASSERT_FALSE(prepare.empty());
  ASSERT_FALSE(finish.empty());
  ASSERT_FALSE(admission.empty());
  ASSERT_FALSE(intent.empty());
  ASSERT_FALSE(delivery.empty());

  EXPECT_NE(begin.find("BeginTransaction()"), std::string::npos);
  EXPECT_NE(begin.find("BeginProfileMutation()"), std::string::npos);
  EXPECT_EQ(begin.find("AnnouncementManager"), std::string::npos);
  EXPECT_EQ(prepare.find("BeginTransaction()"), std::string::npos);
  EXPECT_EQ(prepare.find("Initialize()"), std::string::npos);
  EXPECT_LT(finish.find("ApplyActiveJumpgateProfile();"), finish.find("BeginTransaction()"));
  EXPECT_LT(finish.find("m_traktScrobbler->Initialize();"), finish.find("BeginTransaction()"));
  EXPECT_NE(admission.find("CommitAdmission(generation)"), std::string::npos);
  EXPECT_EQ(admission.find("BeginLifecycleOperation()"), std::string::npos);
  EXPECT_LT(intent.find("BeginLifecycleOperation()"),
            intent.find("BeginExternalPlaybackAdmission("));
  EXPECT_EQ(admission.find("Initialize()"), std::string::npos);
  EXPECT_EQ(delivery.find("BeginTransaction()"), std::string::npos);
  EXPECT_EQ(delivery.find("BeginLifecycleOperation()"), std::string::npos);
  EXPECT_LT(delivery.find("TakeFinished(lifecycleOperation)"),
            delivery.find("ExitExternalPlayerMode(*result, lifecycleOperation)"));
  EXPECT_NE(app.find("m_playbackResultState.CloseAdmissions();"), std::string::npos);
  EXPECT_NE(admission.find("m_playbackResultState.AdmissionsClosed()"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic, AndroidDestroyQuiescesBeforeTheFinalServiceDrain)
{
  const std::string trakt = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(trakt.empty());
  ASSERT_FALSE(app.empty());

  const std::string deinitialize = FunctionSection(
      trakt, "void TraktScrobbler::Deinitialize(bool drainScrobble)",
      "// ---------------------------------------------------------------------------");
  ASSERT_FALSE(deinitialize.empty());
  const std::size_t nonDrainingBranch = deinitialize.find("if (!drainScrobble)");
  const std::size_t nonDrainingReturn = deinitialize.find("return;", nonDrainingBranch);
  const std::size_t removeAnnouncer = deinitialize.find("RemoveAnnouncer(this)");
  const std::size_t serviceBarrier = deinitialize.find("serviceIoLock");
  const std::size_t fullyDrained = deinitialize.find("if (!m_scrobbleDispatcher)");
  ASSERT_NE(nonDrainingBranch, std::string::npos);
  ASSERT_NE(nonDrainingReturn, std::string::npos);
  ASSERT_NE(removeAnnouncer, std::string::npos);
  ASSERT_NE(serviceBarrier, std::string::npos);
  ASSERT_NE(fullyDrained, std::string::npos);
  EXPECT_LT(nonDrainingReturn, removeAnnouncer);
  EXPECT_LT(nonDrainingReturn, serviceBarrier);
  EXPECT_LT(fullyDrained, removeAnnouncer);

  const std::string onDestroy =
      FunctionSection(app, "void CXBMCApp::onDestroy()", "void CXBMCApp::onSaveState(");
  const std::string finalDrain =
      FunctionSection(app, "void CXBMCApp::Deinitialize()", "bool CXBMCApp::Stop(");
  ASSERT_FALSE(onDestroy.empty());
  ASSERT_FALSE(finalDrain.empty());
  EXPECT_NE(onDestroy.find("m_traktScrobbler->Deinitialize(false);"), std::string::npos);
  EXPECT_NE(finalDrain.find("m_traktScrobbler->Deinitialize(true);"), std::string::npos);
  EXPECT_LT(finalDrain.find("m_traktScrobbler->Deinitialize(true);"),
            finalDrain.find("CJumpgateThreadRegistry::Global()->JoinAll();"));
  EXPECT_EQ(Count(trakt, "XFILE::CCurlFile "), Count(trakt, ".SetTotalTimeout("));
}
