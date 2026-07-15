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
  const std::size_t deinitializeScrobbler =
      finalDrain.find("m_traktScrobbler->Deinitialize(true);");
  const std::size_t boundedWorkerDrain =
      finalDrain.find("CJumpgateThreadRegistry::Global()->JoinAll(");
  ASSERT_NE(deinitializeScrobbler, std::string::npos);
  ASSERT_NE(boundedWorkerDrain, std::string::npos);
  EXPECT_LT(deinitializeScrobbler, boundedWorkerDrain);
  EXPECT_EQ(Count(trakt, "XFILE::CCurlFile "), Count(trakt, ".SetTotalTimeout("));
}

TEST(TestJumpgateApplicationLifecycleStatic,
     TraktWritesRequirePairedClaimsAndBridgeIssuedClientIdentity)
{
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string header = ReadKodiSource("platform/android/activity/TraktScrobbler.h");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(header.empty());

  EXPECT_EQ(source.find("client_secret"), std::string::npos);
  EXPECT_EQ(source.find("/oauth/device"), std::string::npos);
  EXPECT_NE(source.find("special://profile/trakt.json"), std::string::npos);
  EXPECT_NE(source.find("CFile::Delete(legacyTokenPath)"), std::string::npos);
  EXPECT_EQ(source.find("LoadFile(legacyTokenPath"), std::string::npos);
  EXPECT_EQ(source.find("OpenForWrite(legacyTokenPath"), std::string::npos);
  EXPECT_EQ(header.find("TRAKT_CLIENT_SECRET"), std::string::npos);
  EXPECT_EQ(header.find("TRAKT_CLIENT_ID"), std::string::npos);
  EXPECT_EQ(source.find("?query="), std::string::npos);

  const std::string authority =
      FunctionBody(source, "bool TraktScrobbler::IsTraktIdentityAuthorized() const");
  const std::string tokenFetch =
      FunctionBody(source, "bool TraktScrobbler::FetchAccessTokenFromBridge()");
  ASSERT_FALSE(authority.empty());
  ASSERT_FALSE(tokenFetch.empty());
  EXPECT_NE(authority.find("m_bridgeProfileBacked"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimResolved"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimAuthorized"), std::string::npos);
  EXPECT_NE(tokenFetch.find("/v1/trakt/token"), std::string::npos);
  EXPECT_NE(tokenFetch.find("data[\"client_id\"]"), std::string::npos);
  EXPECT_NE(tokenFetch.find("IsValidTraktClientId(clientId)"), std::string::npos);
  EXPECT_NE(tokenFetch.find("m_traktClientId = clientId"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     NativeRuntimeHasNoUnauthenticatedLegacyIdentityTransport)
{
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string header = ReadKodiSource("platform/android/activity/TraktScrobbler.h");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(header.empty());
  ASSERT_FALSE(app.empty());

  EXPECT_EQ(source.find("QueryBridgeServer"), std::string::npos);
  EXPECT_EQ(header.find("QueryBridgeServer"), std::string::npos);
  EXPECT_EQ(source.find("\"/identify\""), std::string::npos);
  EXPECT_EQ(source.find("\"/resume\""), std::string::npos);
  EXPECT_EQ(app.find("\"/identify\""), std::string::npos);
  EXPECT_EQ(app.find("\"/resume\""), std::string::npos);
  EXPECT_EQ(source.find("GetTraktResumePosition"), std::string::npos);
  EXPECT_EQ(header.find("GetTraktResumePosition"), std::string::npos);
  EXPECT_EQ(header.find("GetBridgeResumePosition"), std::string::npos);
  EXPECT_EQ(header.find("m_bridgeResumePositionMs"), std::string::npos);

  const std::string saveResume =
      FunctionBody(app, "void CXBMCApp::SaveResumePosition(bool explicitEnd)");
  const std::string identify = FunctionBody(source, "bool TraktScrobbler::IdentifyContent()");
  ASSERT_FALSE(saveResume.empty());
  ASSERT_FALSE(identify.empty());
  EXPECT_EQ(saveResume.find("XFILE::CCurlFile"), std::string::npos);
  EXPECT_EQ(saveResume.find(".Post("), std::string::npos);
  EXPECT_NE(app.find("special://profile/jumpgate_resume.json"), std::string::npos);

  EXPECT_NE(identify.find("GetValue(\"Content-Disposition\")"), std::string::npos);
  EXPECT_NE(identify.find("std::regex fnPattern"), std::string::npos);
  EXPECT_EQ(identify.find("Content-Disposition: {}"), std::string::npos);
  EXPECT_EQ(identify.find("Filename from header: {}"), std::string::npos);
  EXPECT_EQ(Count(identify, "contentDisp"), 3u);
  EXPECT_EQ(Count(identify, "cdFilename"), 2u);
}

TEST(TestJumpgateApplicationLifecycleStatic, RuntimeLogsNeverExposeAndroidUrisOrUrlCredentials)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(source.empty());

  const std::string startActivity = FunctionBody(app, "bool CXBMCApp::StartActivity(");
  const std::string redactionGuards =
      FunctionSection(source, "struct LogUrlOrigin", "std::string RedactUrlForLog(");
  const std::string redact = FunctionBody(source, "std::string RedactUrlForLog(");
  const std::string setMediaUrl = FunctionBody(source, "void TraktScrobbler::SetMediaUrl(");
  const std::string identify = FunctionBody(source, "bool TraktScrobbler::IdentifyContent()");
  ASSERT_FALSE(startActivity.empty());
  ASSERT_FALSE(redactionGuards.empty());
  ASSERT_FALSE(redact.empty());
  ASSERT_FALSE(setMediaUrl.empty());
  ASSERT_FALSE(identify.empty());

  EXPECT_NE(startActivity.find("Starting Android activity"), std::string::npos);
  EXPECT_NE(startActivity.find("Sharing content through FileProvider"), std::string::npos);
  EXPECT_NE(startActivity.find("std::string(CCompileInfo::GetPackage()) + \".fileprovider\""),
            std::string::npos);
  EXPECT_EQ(startActivity.find("dataURI: {}"), std::string::npos);
  EXPECT_EQ(startActivity.find("jniURI.toString()"), std::string::npos);
  EXPECT_EQ(startActivity.find("Share using FileProvider:"), std::string::npos);
  EXPECT_EQ(startActivity.find("Putting extra key:"), std::string::npos);
  EXPECT_EQ(startActivity.find("ExceptionDescribe()"), std::string::npos);
  EXPECT_EQ(startActivity.find("{}"), std::string::npos);

  EXPECT_NE(redactionGuards.find("character == '@' || character == '%'"), std::string::npos);
  EXPECT_NE(redactionGuards.find("IsUnsafeLogUrlCharacter(character)"), std::string::npos);
  EXPECT_NE(redactionGuards.find("authority.find(':', colon + 1)"), std::string::npos);
  EXPECT_NE(redactionGuards.find("IsValidLogUrlHost(host)"), std::string::npos);
  EXPECT_NE(redactionGuards.find("value >= 1 && value <= 65535"), std::string::npos);
  EXPECT_GE(Count(redactionGuards, "static_assert("), 10u);
  EXPECT_NE(redactionGuards.find("HTTPS://Example.TEST:443/private/media?token=secret#fragment"),
            std::string::npos);
  EXPECT_NE(redactionGuards.find("viewer:opaque@example.test"), std::string::npos);
  EXPECT_NE(redactionGuards.find("viewer@example.test"), std::string::npos);
  EXPECT_NE(redactionGuards.find("viewer%40example.test"), std::string::npos);
  EXPECT_NE(redactionGuards.find("viewer:opaque/private"), std::string::npos);
  EXPECT_NE(redactionGuards.find("https:///private"), std::string::npos);
  EXPECT_NE(redactionGuards.find("not-a-url"), std::string::npos);
  EXPECT_NE(redactionGuards.find("example.test:70000/private"), std::string::npos);
  EXPECT_NE(redactionGuards.find("attacker.test/private"), std::string::npos);
  EXPECT_NE(redactionGuards.find("example..test/private"), std::string::npos);

  EXPECT_NE(redact.find("ParseUrlOriginForLog(rawUrl)"), std::string::npos);
  EXPECT_NE(redact.find("appendLower(origin.scheme)"), std::string::npos);
  EXPECT_NE(redact.find("appendLower(origin.host)"), std::string::npos);
  EXPECT_NE(redact.find("redacted.append(origin.port)"), std::string::npos);
  EXPECT_EQ(redact.find("/<redacted>"), std::string::npos);
  EXPECT_EQ(redact.find("redacted.append(rawUrl)"), std::string::npos);

  EXPECT_NE(setMediaUrl.find("RedactUrlForLog(url)"), std::string::npos);
  EXPECT_NE(identify.find("RedactUrlForLog(newResolvedUrl)"), std::string::npos);
  EXPECT_NE(identify.find("RedactUrlForLog(mediaUrl)"), std::string::npos);
  EXPECT_EQ(Count(source, "RedactUrlForLog("), 7u);
}

TEST(TestJumpgateApplicationLifecycleStatic, UnpairedHeuristicsCannotAuthorizeTraktOrProfileHistory)
{
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(app.empty());

  const std::string setInfo = FunctionBody(source, "void TraktScrobbler::SetContentInfo(");
  const std::string authority =
      FunctionBody(source, "bool TraktScrobbler::IsTraktIdentityAuthorized() const");
  const std::string identify = FunctionBody(source, "bool TraktScrobbler::IdentifyContent()");
  const std::string saveResume =
      FunctionBody(app, "void CXBMCApp::SaveResumePosition(bool explicitEnd)");
  const std::string processClaim = FunctionBody(app, "void CXBMCApp::ProcessPlaybackSourceClaim()");
  const std::string onDestroy = FunctionBody(app, "void CXBMCApp::onDestroy()");
  ASSERT_FALSE(setInfo.empty());
  ASSERT_FALSE(authority.empty());
  ASSERT_FALSE(identify.empty());
  ASSERT_FALSE(saveResume.empty());
  ASSERT_FALSE(processClaim.empty());
  ASSERT_FALSE(onDestroy.empty());

  EXPECT_NE(setInfo.find("m_sourceClaimResolved = false"), std::string::npos);
  EXPECT_NE(setInfo.find("m_sourceClaimAuthorized = false"), std::string::npos);
  EXPECT_NE(setInfo.find("m_contentIdentified = !m_bridgeProfileBacked"), std::string::npos);
  EXPECT_NE(authority.find("m_bridgeProfileBacked"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimResolved"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimAuthorized"), std::string::npos);

  const std::string pairedIdentify = FunctionBody(identify, "if (m_bridgeProfileBacked)");
  ASSERT_FALSE(pairedIdentify.empty());
  EXPECT_NE(pairedIdentify.find("return authorized;"), std::string::npos);
  EXPECT_EQ(pairedIdentify.find("FetchLogoFromBridge"), std::string::npos);
  EXPECT_LT(identify.find("if (m_bridgeProfileBacked)"), identify.find("std::regex imdbPattern"));
  EXPECT_LT(identify.find("if (m_bridgeProfileBacked)"),
            identify.find("GetValue(\"Content-Disposition\")"));

  const std::string pairedHistory =
      FunctionBody(saveResume, "if (m_traktScrobbler->IsBridgeProfileBacked())");
  ASSERT_FALSE(pairedHistory.empty());
  EXPECT_NE(pairedHistory.find("SavePairedPlaybackHistory(explicitEnd)"), std::string::npos);
  EXPECT_NE(pairedHistory.find("return;"), std::string::npos);
  EXPECT_LT(saveResume.find("if (m_traktScrobbler->IsBridgeProfileBacked())"),
            saveResume.find("CSpecialProtocol::TranslatePath(RESUME_STORE_FILE)"));
  EXPECT_EQ(Count(app, "m_playbackHistoryState.Activate("), 1u);
  EXPECT_NE(processClaim.find("m_playbackHistoryState.Activate("), std::string::npos);

  const std::string destroyHistory = FunctionBody(
      onDestroy, "if (m_externalPlayerMode.load(std::memory_order_relaxed) && m_traktScrobbler)");
  const std::string pairedDestroy =
      FunctionBody(destroyHistory, "if (m_traktScrobbler->IsBridgeProfileBacked())");
  const std::string unpairedDestroy = FunctionBody(destroyHistory, "else");
  ASSERT_FALSE(destroyHistory.empty());
  ASSERT_FALSE(pairedDestroy.empty());
  ASSERT_FALSE(unpairedDestroy.empty());
  EXPECT_NE(pairedDestroy.find("SavePairedPlaybackHistory(false)"), std::string::npos);
  EXPECT_EQ(pairedDestroy.find("SaveLegacyResumeForContentLocal"), std::string::npos);
  EXPECT_NE(unpairedDestroy.find("SaveLegacyResumeForContentLocal"), std::string::npos);
  EXPECT_EQ(unpairedDestroy.find("SavePairedPlaybackHistory"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     PairedLocalOnlyClaimsUseExactHistoryAndFailuresCannotUseLegacyIdentity)
{
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(app.empty());

  const std::string setClaim = FunctionBody(source, "bool TraktScrobbler::SetClaimedContentInfo(");
  const std::string identify = FunctionBody(source, "bool TraktScrobbler::IdentifyContent()");
  const std::string processClaim = FunctionBody(app, "void CXBMCApp::ProcessPlaybackSourceClaim()");
  const std::string loadPaired =
      FunctionBody(app, "void CXBMCApp::LoadAndApplyPairedPlaybackResume(uint64_t generation)");
  const std::string onContent = FunctionBody(app, "void CXBMCApp::OnContentIdentified()");
  const std::string intent = FunctionBody(app, "void CXBMCApp::onNewIntent(CJNIIntent intent)");
  ASSERT_FALSE(setClaim.empty());
  ASSERT_FALSE(identify.empty());
  ASSERT_FALSE(processClaim.empty());
  ASSERT_FALSE(loadPaired.empty());
  ASSERT_FALSE(onContent.empty());
  ASSERT_FALSE(intent.empty());

  EXPECT_NE(setClaim.find("const bool authorized = traktEligible"), std::string::npos);
  EXPECT_NE(setClaim.find("m_sourceClaimAuthorized = authorized"), std::string::npos);
  EXPECT_NE(setClaim.find("Source claim is local-only; Trakt remains disabled"), std::string::npos);
  EXPECT_NE(setClaim.find("return true;"), std::string::npos);
  EXPECT_EQ(Count(processClaim, "context->traktEligible"), 1u);
  EXPECT_NE(processClaim.find("historyIdentity.profileId = context->profileId"), std::string::npos);
  EXPECT_NE(processClaim.find("historyIdentity.contentKey = *context->contentKey"),
            std::string::npos);
  EXPECT_NE(processClaim.find("const std::string title = context->display.title.value_or"),
            std::string::npos);
  EXPECT_NE(processClaim.find("const std::string logoUrl = context->display.logo.value_or"),
            std::string::npos);
  EXPECT_NE(setClaim.find("m_title = title"), std::string::npos);
  EXPECT_NE(setClaim.find("m_logoUrl = logoUrl"), std::string::npos);

  const std::size_t applyClaim = processClaim.find("SetClaimedContentInfo(");
  const std::size_t bindProfile =
      processClaim.find("historyIdentity.profileId = context->profileId");
  const std::size_t bindContent =
      processClaim.find("historyIdentity.contentKey = *context->contentKey");
  const std::size_t activate = processClaim.find("m_playbackHistoryState.Activate(");
  const std::size_t loadResume = processClaim.find("LoadAndApplyPairedPlaybackResume(generation)");
  ASSERT_NE(applyClaim, std::string::npos);
  ASSERT_NE(bindProfile, std::string::npos);
  ASSERT_NE(bindContent, std::string::npos);
  ASSERT_NE(activate, std::string::npos);
  ASSERT_NE(loadResume, std::string::npos);
  EXPECT_LT(applyClaim, bindProfile);
  EXPECT_LT(bindProfile, activate);
  EXPECT_LT(bindContent, activate);
  EXPECT_LT(activate, loadResume);
  EXPECT_NE(loadPaired.find("Get(token->profileId, token->contentKey"), std::string::npos);

  const std::string failedClaim =
      FunctionBody(processClaim, "if (!completion->result.IsClaimed())");
  const std::string invalidContext =
      FunctionBody(processClaim, "if (!context || context->profileId != expectedProfileId ||");
  ASSERT_FALSE(failedClaim.empty());
  ASSERT_FALSE(invalidContext.empty());
  EXPECT_NE(failedClaim.find("return;"), std::string::npos);
  EXPECT_EQ(failedClaim.find("SetClaimedContentInfo"), std::string::npos);
  EXPECT_EQ(failedClaim.find("m_playbackHistoryState.Activate"), std::string::npos);
  EXPECT_NE(invalidContext.find("return;"), std::string::npos);
  EXPECT_EQ(invalidContext.find("SetClaimedContentInfo"), std::string::npos);
  EXPECT_EQ(invalidContext.find("m_playbackHistoryState.Activate"), std::string::npos);

  const std::string pairedIdentify = FunctionBody(identify, "if (m_bridgeProfileBacked)");
  const std::string legacyResume =
      FunctionBody(onContent, "if (!m_traktScrobbler->IsBridgeProfileBacked())");
  ASSERT_FALSE(pairedIdentify.empty());
  ASSERT_FALSE(legacyResume.empty());
  EXPECT_NE(pairedIdentify.find("return authorized;"), std::string::npos);
  EXPECT_EQ(pairedIdentify.find("FetchLogoFromBridge"), std::string::npos);
  EXPECT_NE(legacyResume.find("LoadResumePosition(imdb, season, episode)"), std::string::npos);
  EXPECT_EQ(Count(onContent, "LoadResumePosition("), 1u);

  const std::string legacyIntentMetadata = FunctionBody(intent, "if (!pairedProfileBacked)");
  const std::string legacyIntentResume = FunctionBody(
      intent, "if (!pairedProfileBacked && resumePositionMs <= 0 && !imdbId.empty() &&");
  ASSERT_FALSE(legacyIntentMetadata.empty());
  ASSERT_FALSE(legacyIntentResume.empty());
  EXPECT_NE(legacyIntentMetadata.find("intent.hasExtra(\"imdb_id\")"), std::string::npos);
  EXPECT_NE(legacyIntentResume.find("LoadResumePosition(imdbId, season, episode)"),
            std::string::npos);
  EXPECT_EQ(Count(intent, "LoadResumePosition("), 1u);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     AndroidSubtitlesCommitTerminalAndQuiesceBeforeCleanupOrServiceDrain)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string subtitles =
      ReadKodiSource("platform/android/activity/AndroidJumpgateSubtitleTransport.cpp");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(subtitles.empty());

  const std::string terminal = FunctionBody(app, "CXBMCApp::CommitExternalPlaybackTerminal(");
  const std::string onDestroy = FunctionBody(app, "void CXBMCApp::onDestroy()");
  const std::string deinitialize = FunctionBody(app, "void CXBMCApp::Deinitialize()");
  const std::string transition =
      FunctionBody(app, "CXBMCApp::PrepareJumpgateProfileAuthorityTransition()");
  const std::string finishTransition =
      FunctionBody(app, "CXBMCApp::FinishJumpgateProfileAuthorityTransition(");
  const std::string stop =
      FunctionBody(subtitles, "void CAndroidJumpgateSubtitleController::Stop(");
  ASSERT_FALSE(terminal.empty());
  ASSERT_FALSE(onDestroy.empty());
  ASSERT_FALSE(deinitialize.empty());
  ASSERT_FALSE(transition.empty());
  ASSERT_FALSE(finishTransition.empty());
  ASSERT_FALSE(stop.empty());

  const std::size_t terminalCommit = terminal.find("CommitPlaybackTerminal(token, started)");
  const std::size_t terminalSubtitles = terminal.find("OnPlaybackTerminal(terminal->generation)");
  const std::size_t deinitializeSubtitles =
      deinitialize.find("StopJumpgateSubtitleController(false)");
  const std::size_t deinitializeDrain =
      deinitialize.find("CJumpgateThreadRegistry::Global()->JoinAll(");
  const std::size_t transitionSubtitles = transition.find("StopJumpgateSubtitleController(false)");
  const std::size_t transitionClaim = transition.find("ReleasePlaybackSourceClaim()");
  const std::size_t coordinatorStop = stop.find("coordinator->Stop(");
  const std::size_t lifecycleShutdown = stop.find("m_lifecycle.Shutdown(playerMayRead)");
  const std::size_t stageStop = stop.find("worker->Stop(");
  ASSERT_NE(terminalCommit, std::string::npos);
  ASSERT_NE(terminalSubtitles, std::string::npos);
  ASSERT_NE(deinitializeSubtitles, std::string::npos);
  ASSERT_NE(deinitializeDrain, std::string::npos);
  ASSERT_NE(transitionSubtitles, std::string::npos);
  ASSERT_NE(transitionClaim, std::string::npos);
  ASSERT_NE(coordinatorStop, std::string::npos);
  ASSERT_NE(lifecycleShutdown, std::string::npos);
  ASSERT_NE(stageStop, std::string::npos);
  ASSERT_NE(finishTransition.find("m_jumpgateSubtitleController->Restart()"), std::string::npos);
  EXPECT_LT(terminalCommit, terminalSubtitles);
  EXPECT_NE(onDestroy.find("StopJumpgateSubtitleController(true, false)"), std::string::npos);
  EXPECT_LT(deinitializeSubtitles, deinitializeDrain);
  EXPECT_LT(transitionSubtitles, transitionClaim);
  EXPECT_LT(coordinatorStop, lifecycleShutdown);
  EXPECT_LT(lifecycleShutdown, stageStop);
}

TEST(TestJumpgateApplicationLifecycleStatic, AndroidSubtitleCurlUsesOnlySafeCancellationControls)
{
  const std::string transport =
      ReadKodiSource("platform/android/activity/AndroidJumpgateSubtitleTransport.cpp");
  ASSERT_FALSE(transport.empty());

  const std::string execute =
      FunctionBody(transport, "bool Execute(const JumpgateSubtitleHttpRequest& request,");
  const std::string cancel = FunctionBody(transport, "void RequestSafeCancellation() override");
  ASSERT_FALSE(execute.empty());
  ASSERT_FALSE(cancel.empty());
  EXPECT_NE(execute.find("if (m_activeCurl)"), std::string::npos);
  EXPECT_NE(execute.find("curl.SetRetry(false);"), std::string::npos);
  EXPECT_NE(execute.find("curl.SetAcceptEncoding(\"identity\");"), std::string::npos);
  EXPECT_NE(execute.find("curl.SetRequestHeader(\"Range\", \"\");"), std::string::npos);
  EXPECT_NE(execute.find("redirect-limit\", \"0\""), std::string::npos);
  EXPECT_NE(cancel.find("m_activeCurl->Cancel();"), std::string::npos);
  EXPECT_EQ(cancel.find("Close("), std::string::npos);
  EXPECT_EQ(cancel.find("ClearSensitiveState("), std::string::npos);
  EXPECT_EQ(cancel.find("reset("), std::string::npos);
}
