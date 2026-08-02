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
     ExternalTerminalIsCommittedBeforeAnnouncementAndAsyncDelivery)
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
            stopped.find("QueuePendingExternalPlayerResult();"));
  EXPECT_LT(ended.find("CommitExternalPlaybackTerminal("), ended.find("\"OnStop\""));
  EXPECT_LT(ended.find("CommitExternalPlaybackTerminal("), ended.find("PlayNext(1, true)"));
  EXPECT_NE(ended.find("QueuePendingExternalPlayerResult();"), std::string::npos);
  EXPECT_EQ(stopped.find("DeliverPendingExternalPlayerResult();"), std::string::npos);
  EXPECT_EQ(ended.find("DeliverPendingExternalPlayerResult();"), std::string::npos);

  const std::string terminal = FunctionSection(
      app,
      "void CXBMCApp::CommitExternalPlaybackTerminal(bool completed, uint64_t token, bool started)",
      "void CXBMCApp::QueuePendingExternalPlayerResult()");
  const std::string queueDelivery =
      FunctionSection(app, "void CXBMCApp::QueuePendingExternalPlayerResult()",
                      "void CXBMCApp::DeliverPendingExternalPlayerResult(");
  const std::string compatibilityDelivery =
      FunctionBody(app, "void CXBMCApp::DeliverPendingExternalPlayerResult()");
  const std::string announcedStop =
      FunctionSection(app, "void CXBMCApp::OnPlayBackStopped(bool completed)",
                      "void CXBMCApp::CommitExternalPlaybackTerminal(bool completed,");
  const std::string errorCallback =
      FunctionSection(callbacks, "void CApplicationPlayerCallback::OnPlayBackError()",
                      "void CApplicationPlayerCallback::OnQueueNextItem()");
  ASSERT_FALSE(terminal.empty());
  ASSERT_FALSE(queueDelivery.empty());
  ASSERT_FALSE(compatibilityDelivery.empty());
  ASSERT_FALSE(announcedStop.empty());
  ASSERT_FALSE(errorCallback.empty());
  EXPECT_NE(terminal.find("CommitPlaybackTerminal(token, started)"), std::string::npos);
  EXPECT_NE(terminal.find("HasNewerPlayback(terminal->token)"), std::string::npos);
  EXPECT_LT(terminal.find("m_playbackResultState.Capture("),
            terminal.find("m_playbackResultState.Finish("));
  EXPECT_EQ(terminal.find("m_pendingPlaybackResult"), std::string::npos);
  EXPECT_NE(queueDelivery.find("BeginLifecycleOperation()"), std::string::npos);
  EXPECT_NE(queueDelivery.find("CurrentOwner(lifecycleOperation)"), std::string::npos);
  EXPECT_NE(queueDelivery.find("QueueExternalPlayerResult("), std::string::npos);
  EXPECT_EQ(queueDelivery.find("TakeFinished("), std::string::npos);
  EXPECT_NE(compatibilityDelivery.find("QueuePendingExternalPlayerResult();"), std::string::npos);
  EXPECT_EQ(compatibilityDelivery.find("TakeFinished("), std::string::npos);
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
  const std::string intent =
      FunctionSection(app, "void CXBMCApp::onNewIntent(", "void CXBMCApp::onActivityResult(");
  const std::string failure =
      FunctionSection(app, "bool CXBMCApp::CommitExternalPlaybackAdmissionFailure(uint64_t token)",
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
  EXPECT_NE(intent.find("QueueExternalPlayback("), std::string::npos);
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
      intent.find("DeliverRejectedExternalPlaybackResult(resultRequestId, *lifecycleOperation);"),
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
  EXPECT_NE(exit.find("ExternalPlayerResultStore.publish(this, reservation.terminal())"),
            std::string::npos);
  EXPECT_LT(exit.find("ExternalPlayerResultStore.publish(this, reservation.terminal())"),
            exit.find("reservation.commit()"));
  EXPECT_NE(acknowledge.find("mExternalResultProducer.acknowledgeExit("), std::string::npos);
  EXPECT_LT(executeExit.find("processExitState.claim("), executeExit.find("editor.commit()"));
  EXPECT_LT(executeExit.find("editor.commit()"), executeExit.find("terminator.run();"));
  EXPECT_NE(coordinator.find("claimProcessExit("), std::string::npos);
  EXPECT_NE(coordinator.find("claim(Terminal terminal)"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     AndroidLoadingPortalIsClaimBoundBoundedAndFrameReady)
{
  const std::string main =
      ReadKodiSource("../tools/android/packaging/xbmc/src/Main.java.in");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(main.empty());
  ASSERT_FALSE(app.empty());

  const std::string show = FunctionBody(main, "private void showLoadingOverlay(Intent sourceIntent)");
  const std::string hide = FunctionBody(main, "public void hideLoadingOverlay()");
  const std::string allowed = FunctionBody(main, "private boolean isAllowedArtworkUrl(");
  const std::string download = FunctionBody(main, "private byte[] downloadOverlayArtwork(");
  const std::string decode = FunctionBody(main, "private Bitmap decodeOverlayArtwork(");
  ASSERT_FALSE(show.empty());
  ASSERT_FALSE(hide.empty());
  ASSERT_FALSE(allowed.empty());
  ASSERT_FALSE(download.empty());
  ASSERT_FALSE(decode.empty());

  EXPECT_NE(show.find("ImageView.ScaleType.CENTER_CROP"), std::string::npos);
  EXPECT_NE(show.find("dp(200), dp(80)"), std::string::npos);
  EXPECT_NE(main.find("mOverlayBackdropView.animate().alpha(0.50f)"), std::string::npos);
  EXPECT_NE(main.find("mOverlayPulseAnimator.setDuration(750L)"), std::string::npos);
  EXPECT_NE(main.find("R.drawable.jumpgate_wordmark"), std::string::npos);
  EXPECT_EQ(main.find("postDelayed(() -> hideLoadingOverlay(), 30000)"), std::string::npos);
  EXPECT_EQ(hide.find("mOverlayArtworkSequence = 0"), std::string::npos);
  EXPECT_EQ(hide.find("mOverlayLogoRequestId = 0"), std::string::npos);
  EXPECT_EQ(hide.find("mOverlayBackgroundRequestId = 0"), std::string::npos);

  for (const char* contract : {"\"https\".equalsIgnoreCase(uri.getScheme())",
                               "\"image.tmdb.org\".equalsIgnoreCase(uri.getHost())",
                               "uri.getPort() == -1", "uri.getEncodedUserInfo() == null",
                               "uri.getQuery() == null", "uri.getFragment() == null"})
    EXPECT_NE(allowed.find(contract), std::string::npos) << contract;
  EXPECT_NE(download.find("setInstanceFollowRedirects(false)"), std::string::npos);
  EXPECT_NE(download.find("OVERLAY_ARTWORK_MAX_ENCODED_BYTES"), std::string::npos);
  EXPECT_NE(decode.find("OVERLAY_ARTWORK_MAX_DIMENSION"), std::string::npos);
  EXPECT_NE(decode.find("OVERLAY_ARTWORK_MAX_PIXELS"), std::string::npos);
  EXPECT_NE(main.find("output.write(encoded)"), std::string::npos);
  EXPECT_EQ(main.find("bmp.compress("), std::string::npos);

  EXPECT_NE(app.find("context->display.background.value_or"), std::string::npos);
  EXPECT_NE(app.find("m_lastOverlayBackgroundUrl"), std::string::npos);
  EXPECT_NE(app.find("Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"),
            std::string::npos);
  EXPECT_NE(app.find("message == \"OnAVStart\""), std::string::npos);
  EXPECT_NE(app.find("call_method<void>(m_context, \"hideLoadingOverlay\", \"()V\")"),
            std::string::npos);
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
  const std::string intent = FunctionBody(app, "void CXBMCApp::onNewIntent(");
  const std::string delivery =
      FunctionSection(app, "void CXBMCApp::DeliverPendingExternalPlayerResult(\n",
                      "bool CXBMCApp::ExitExternalPlayerMode");
  const std::string exitExternalPlayerMode =
      "ExitExternalPlayerMode(*result, lifecycleOperation, deferPlayerCleanup)";
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
            delivery.find(exitExternalPlayerMode));
  EXPECT_LT(delivery.find(exitExternalPlayerMode),
            delivery.find("m_playbackResultState.Reset(lifecycleOperation, result->generation)"));
  EXPECT_LT(delivery.find("m_playbackResultState.Reset(lifecycleOperation, result->generation)"),
            delivery.find("HandoffWarmExternalPlayerTask(result->generation, result->requestId)"));
  EXPECT_NE(app.find("m_playbackResultState.CloseAdmissions();"), std::string::npos);
  EXPECT_NE(admission.find("m_playbackResultState.AdmissionsClosed()"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     NativeCallbacksHoldLifecycleCurrencyAndExternalBackOwnsEarlyCancellation)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string jni = ReadKodiSource("platform/android/activity/JNIMainActivity.cpp");
  const std::string jniHeader = ReadKodiSource("platform/android/activity/JNIMainActivity.h");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(jni.empty());
  ASSERT_FALSE(jniHeader.empty());

  const std::string externalBack = FunctionBody(app, "bool CXBMCApp::DispatchExternalBack(");
  const std::string kodiBack = FunctionBody(app, "bool CXBMCApp::DispatchKodiBack(");
  const std::string backInvoked = FunctionBody(jni, "void CJNIMainActivity::_onBackInvoked(");
  const std::string acquire = FunctionBody(jni, "CJNIMainActivity::AcquireAppInstance(");
  const std::string moveAssignment =
      FunctionBody(jniHeader, "AppInstanceOperation& operator=(AppInstanceOperation&& other)");
  const std::string queuedBack = FunctionSection(app, "struct CXBMCApp::QueuedBackCommand final",
                                                 "struct CXBMCApp::QueuedExternalPlayback final");
  const std::string queuedResult =
      FunctionBody(app, "void CXBMCApp::ExecuteQueuedExternalPlayerResult(");
  ASSERT_FALSE(externalBack.empty());
  ASSERT_FALSE(kodiBack.empty());
  ASSERT_FALSE(backInvoked.empty());
  ASSERT_FALSE(acquire.empty());
  ASSERT_FALSE(moveAssignment.empty());
  ASSERT_FALSE(queuedBack.empty());
  ASSERT_FALSE(queuedResult.empty());

  EXPECT_EQ(Count(app, "recordEarlyExternalPlayerBack"), 1u);
  EXPECT_NE(externalBack.find("context.Execute("), std::string::npos);
  EXPECT_NE(externalBack.find("recordEarlyExternalPlayerBack"), std::string::npos);
  EXPECT_EQ(kodiBack.find("recordEarlyExternalPlayerBack"), std::string::npos);
  EXPECT_EQ(backInvoked.find("recordEarlyExternalPlayerBack"), std::string::npos);
  EXPECT_NE(backInvoked.find("OnApi36BackInvoked(token)"), std::string::npos);

  EXPECT_NE(jniHeader.find("class AppInstanceOperation final"), std::string::npos);
  EXPECT_LT(jniHeader.find("BackLifecycleOperation m_lifecycleOperation;"),
            jniHeader.find("std::shared_ptr<CJNIMainActivity> m_appInstance;"));
  EXPECT_LT(acquire.find("TryAcquireLifecycleOperation(lifecycleToken)"),
            acquire.find("GetAppTargetRegistry().Acquire(lifecycleToken)"));
  EXPECT_LT(moveAssignment.find("m_appInstance.reset();"),
            moveAssignment.find("m_lifecycleOperation = std::move("));
  EXPECT_NE(queuedBack.find("context->Reject();"), std::string::npos);
  EXPECT_EQ(queuedBack.find("context->Cancel();"), std::string::npos);
  EXPECT_EQ(queuedResult.find("IsCurrentLifecycle(payload.lifecycleToken)"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic, AndroidDestroyQuiescesBeforeTheFinalServiceDrain)
{
  const std::string trakt = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(trakt.empty());
  ASSERT_FALSE(app.empty());

  const std::string deinitialize =
      FunctionBody(trakt, "void TraktScrobbler::Deinitialize(bool drainHistory)");
  ASSERT_FALSE(deinitialize.empty());
  const std::size_t terminal = deinitialize.find("StopForReplacement(false)");
  const std::size_t moveDispatcher = deinitialize.find("dispatcher = std::move(m_dispatcher)");
  const std::size_t boundedStop = deinitialize.find("dispatcher->Stop(drainHistory)");
  ASSERT_NE(terminal, std::string::npos);
  ASSERT_NE(moveDispatcher, std::string::npos);
  ASSERT_NE(boundedStop, std::string::npos);
  EXPECT_LT(terminal, moveDispatcher);
  EXPECT_LT(moveDispatcher, boundedStop);

  const std::string onDestroy =
      FunctionSection(app, "void CXBMCApp::onDestroy()", "void CXBMCApp::onSaveState(");
  const std::string finalDrain =
      FunctionSection(app, "void CXBMCApp::Deinitialize()", "bool CXBMCApp::Stop(");
  ASSERT_FALSE(onDestroy.empty());
  ASSERT_FALSE(finalDrain.empty());
  const std::size_t releaseClaim = onDestroy.find("ReleasePlaybackSourceClaim()");
  const std::size_t stopClaimWorker = onDestroy.find("StopPlaybackClaimCoordinator(false)");
  const std::size_t deinitializeOnDestroy =
      onDestroy.find("m_traktScrobbler->Deinitialize(false);");
  ASSERT_NE(releaseClaim, std::string::npos);
  ASSERT_NE(stopClaimWorker, std::string::npos);
  ASSERT_NE(deinitializeOnDestroy, std::string::npos);
  EXPECT_LT(releaseClaim, stopClaimWorker);
  EXPECT_LT(stopClaimWorker, deinitializeOnDestroy);
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
     HistoryWritesRequirePairedClaimsAndBridgeOwnedTraktAuthority)
{
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string header = ReadKodiSource("platform/android/activity/TraktScrobbler.h");
  const std::string client = ReadKodiSource("utils/JumpgateHistoryEventClient.cpp");
  const std::string state = ReadKodiSource("utils/JumpgateHistoryEventState.cpp");
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(header.empty());
  ASSERT_FALSE(client.empty());
  ASSERT_FALSE(state.empty());
  ASSERT_FALSE(app.empty());

  const std::string owned = source + header + client + state + app;
  EXPECT_EQ(owned.find("api.trakt.tv"), std::string::npos);
  EXPECT_EQ(owned.find("/v1/trakt/token"), std::string::npos);
  EXPECT_EQ(owned.find("/sync/history"), std::string::npos);
  EXPECT_EQ(owned.find("client_secret"), std::string::npos);
  EXPECT_EQ(owned.find("/oauth/device"), std::string::npos);
  EXPECT_EQ(source.find("/scrobble/"), std::string::npos);
  EXPECT_NE(client.find("request.bridgeOrigin + \"/v1/history/events\""), std::string::npos);
  EXPECT_NE(source.find("special://profile/trakt.json"), std::string::npos);
  EXPECT_NE(source.find("CFile::Delete(legacyTokenPath)"), std::string::npos);
  EXPECT_EQ(source.find("LoadFile(legacyTokenPath"), std::string::npos);
  EXPECT_EQ(source.find("OpenForWrite(legacyTokenPath"), std::string::npos);
  EXPECT_EQ(header.find("TRAKT_CLIENT_SECRET"), std::string::npos);
  EXPECT_EQ(header.find("TRAKT_CLIENT_ID"), std::string::npos);
  EXPECT_EQ(source.find("?query="), std::string::npos);

  const std::string authority =
      FunctionBody(source, "bool TraktScrobbler::IsTraktIdentityAuthorized() const");
  const std::string setClaim = FunctionBody(source, "bool TraktScrobbler::SetClaimedContentInfo(");
  const std::string processClaim = FunctionBody(app, "void CXBMCApp::ProcessPlaybackSourceClaim()");
  ASSERT_FALSE(authority.empty());
  ASSERT_FALSE(setClaim.empty());
  ASSERT_FALSE(processClaim.empty());
  EXPECT_NE(authority.find("m_bridgeProfileBacked"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimResolved"), std::string::npos);
  EXPECT_NE(authority.find("m_sourceClaimAuthorized"), std::string::npos);
  EXPECT_NE(setClaim.find("generation == m_playbackGeneration"), std::string::npos);
  EXPECT_NE(setClaim.find("profileId == m_bridgeProfileId"), std::string::npos);
  EXPECT_NE(setClaim.find("deviceId == m_bridgeDeviceId"), std::string::npos);
  EXPECT_NE(setClaim.find("normalizedOrigin == m_bridgeOrigin"), std::string::npos);
  EXPECT_NE(setClaim.find("binding.deviceToken = deviceToken"), std::string::npos);
  EXPECT_NE(setClaim.find("binding.sessionId = sessionId"), std::string::npos);
  EXPECT_NE(setClaim.find("binding.historyGrant = historyGrant"), std::string::npos);
  EXPECT_NE(setClaim.find("binding.historyGrantKind = historyGrantKind"), std::string::npos);
  EXPECT_NE(setClaim.find("binding.sessionRevision = sessionRevision"), std::string::npos);

  const std::size_t bind = processClaim.find("SetClaimedContentInfo(");
  ASSERT_NE(bind, std::string::npos);
  const std::size_t accept =
      processClaim.find("m_playbackClaimCoordinator->AcceptCompletion(generation)", bind);
  ASSERT_NE(accept, std::string::npos);
  const std::size_t clearCompletion =
      processClaim.find("completion->result.ClearSensitive()", accept);
  ASSERT_NE(clearCompletion, std::string::npos);
  EXPECT_LT(bind, accept);
  EXPECT_LT(accept, clearCompletion);
  EXPECT_NE(processClaim.find("completion->result.claim.sessionRevision"), std::string::npos);
  EXPECT_NE(processClaim.find("completion->result.claim.historyGrant"), std::string::npos);
  EXPECT_NE(processClaim.find("completion->result.claim.historyGrantKind"), std::string::npos);
  EXPECT_EQ(processClaim.find("claim.context[\"contextId\"]"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic,
     BridgeTraktLifecycleWiresBackgroundPeriodicAndTerminalBeforeRelease)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string dispatcher = ReadKodiSource("utils/JumpgateHistoryEventDispatcher.cpp");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(dispatcher.empty());

  const std::string onResume = FunctionBody(app, "void CXBMCApp::onResume()");
  const std::string onPause = FunctionBody(app, "void CXBMCApp::onPause()");
  const std::string playbackStarted = FunctionBody(app, "void CXBMCApp::OnPlayBackStarted(");
  const std::string playbackPaused = FunctionBody(app, "void CXBMCApp::OnPlayBackPaused()");
  const std::string processSlow = FunctionBody(app, "void CXBMCApp::ProcessSlow()");
  const std::string release =
      FunctionBody(app, "bool CXBMCApp::ReleasePlaybackSourceClaim(bool completed)");
  ASSERT_FALSE(onResume.empty());
  ASSERT_FALSE(onPause.empty());
  ASSERT_FALSE(playbackStarted.empty());
  ASSERT_FALSE(playbackPaused.empty());
  ASSERT_FALSE(processSlow.empty());
  ASSERT_FALSE(release.empty());

  EXPECT_NE(onResume.find("SetBackgrounded(false)"), std::string::npos);
  EXPECT_NE(onPause.find("SetBackgrounded(true)"), std::string::npos);
  EXPECT_NE(playbackStarted.find("OnPlaybackStarted(resumed)"), std::string::npos);
  EXPECT_NE(playbackPaused.find("OnPlaybackPaused()"), std::string::npos);
  EXPECT_NE(processSlow.find("m_traktScrobbler->ProcessSlow()"), std::string::npos);

  const std::size_t terminal = release.find("StopForReplacement(completed)");
  const std::size_t releaseRequest = release.find("PlaybackReleaseRequest release");
  const std::size_t queueRelease = release.find("QueueRelease(release)");
  ASSERT_NE(terminal, std::string::npos);
  EXPECT_EQ(releaseRequest, std::string::npos);
  EXPECT_EQ(queueRelease, std::string::npos);
  EXPECT_NE(dispatcher.find("work->terminal && result.IsAccepted()"), std::string::npos);
  EXPECT_NE(dispatcher.find("work->request.idempotencyKey"), std::string::npos);
  EXPECT_NE(dispatcher.find("SafeRelease(claimClient, release)"), std::string::npos);

  EXPECT_EQ(source.find("m_bridgeUrl"), std::string::npos);
  EXPECT_EQ(source.find("bridgeBaseUrl"), std::string::npos);
  EXPECT_NE(source.find("dispatcher->Stop(drainHistory)"), std::string::npos);
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
  EXPECT_NE(identify.find("std::regex imdbPattern"), std::string::npos);
  EXPECT_EQ(identify.find("Content-Disposition: {}"), std::string::npos);
  EXPECT_EQ(identify.find("Filename from header: {}"), std::string::npos);
  EXPECT_EQ(Count(identify, "contentDisposition"), 2u);
  EXPECT_EQ(identify.find("m_sourceClaimAuthorized = true"), std::string::npos);
}

TEST(TestJumpgateApplicationLifecycleStatic, RuntimeLogsNeverExposeAndroidUrisOrUrlCredentials)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string source = ReadKodiSource("platform/android/activity/TraktScrobbler.cpp");
  const std::string client = ReadKodiSource("utils/JumpgateHistoryEventClient.cpp");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(client.empty());

  const std::string startActivity = FunctionBody(app, "bool CXBMCApp::StartActivity(");
  const std::string setMediaUrl = FunctionBody(source, "void TraktScrobbler::SetMediaUrl(");
  const std::string identify = FunctionBody(source, "bool TraktScrobbler::IdentifyContent()");
  ASSERT_FALSE(startActivity.empty());
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

  EXPECT_NE(setMediaUrl.find("Media source set"), std::string::npos);
  EXPECT_EQ(setMediaUrl.find("{}"), std::string::npos);
  EXPECT_NE(identify.find("Local compatibility metadata identified"), std::string::npos);
  EXPECT_EQ(identify.find("CLog::Log(LOGINFO, mediaUrl"), std::string::npos);
  EXPECT_EQ(source.find("deviceToken.c_str()"), std::string::npos);
  EXPECT_EQ(source.find("m_mediaUrl.c_str()"), std::string::npos);
  EXPECT_EQ(client.find("CLog::Log"), std::string::npos);
  EXPECT_GE(Count(client, "ClearSensitive()"), 6u);
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
  EXPECT_NE(pairedIdentify.find("return m_sourceClaimResolved;"), std::string::npos);
  EXPECT_EQ(pairedIdentify.find("FetchLogoFromBridge"), std::string::npos);
  EXPECT_LT(identify.find("if (m_bridgeProfileBacked)"), identify.find("std::regex imdbPattern"));
  EXPECT_LT(identify.find("if (m_bridgeProfileBacked)"),
            identify.find("GetValue(\"Content-Disposition\")"));

  EXPECT_NE(saveResume.find("SavePairedPlaybackHistory(explicitEnd)"), std::string::npos);
  EXPECT_LT(saveResume.find("SavePairedPlaybackHistory(explicitEnd)"),
            saveResume.find("if (!m_traktScrobbler)"));
  EXPECT_LT(saveResume.find("if (m_traktScrobbler->IsBridgeProfileBacked())"),
            saveResume.find("CSpecialProtocol::TranslatePath(RESUME_STORE_FILE)"));
  EXPECT_EQ(Count(app, "m_playbackHistoryState.ActivateLocalSource("), 1u);
  EXPECT_EQ(processClaim.find("m_playbackHistoryState.Activate("), std::string::npos);
  EXPECT_NE(processClaim.find("m_playbackHistoryState.Promote("), std::string::npos);

  const std::string destroyHistory =
      FunctionBody(onDestroy, "if (m_externalPlayerMode.load(std::memory_order_relaxed))");
  ASSERT_FALSE(destroyHistory.empty());
  EXPECT_NE(destroyHistory.find("SavePairedPlaybackHistory(false)"), std::string::npos);
  EXPECT_NE(destroyHistory.find("!m_traktScrobbler->IsBridgeProfileBacked()"), std::string::npos);
  EXPECT_NE(destroyHistory.find("SaveLegacyResumeForContentLocal"), std::string::npos);
  EXPECT_LT(destroyHistory.find("SavePairedPlaybackHistory(false)"),
            destroyHistory.find("SaveLegacyResumeForContentLocal"));
}

TEST(TestJumpgateApplicationLifecycleStatic,
     LocalSourceAdmissionPrecedesClaimsAndEveryFailureRetainsLocalOnlyAuthority)
{
  const std::string app = ReadKodiSource("platform/android/activity/XBMCApp.cpp");
  const std::string header = ReadKodiSource("platform/android/activity/XBMCApp.h");
  ASSERT_FALSE(app.empty());
  ASSERT_FALSE(header.empty());

  const std::string queue = FunctionBody(app, "uint64_t CXBMCApp::QueuePlaybackSourceClaim(");
  const std::string process = FunctionBody(app, "void CXBMCApp::ProcessPlaybackSourceClaim()");
  const std::string load = FunctionBody(app, "void CXBMCApp::LoadAndApplyPairedPlaybackResume(");
  const std::string statusName = FunctionBody(app, "static const char* PlaybackClaimStatusName(");
  const std::string pending = FunctionBody(header, "struct PendingPlaybackClaim");
  ASSERT_FALSE(queue.empty());
  ASSERT_FALSE(process.empty());
  ASSERT_FALSE(load.empty());
  ASSERT_FALSE(statusName.empty());
  ASSERT_FALSE(pending.empty());

  const std::size_t fingerprint = queue.find("FingerprintPlaybackUrl(");
  const std::size_t activate = queue.find("m_playbackHistoryState.ActivateLocalSource(");
  const std::size_t pendingClaim = queue.find("PendingPlaybackClaim pending;");
  const std::size_t fingerprintFailure = queue.find("if (!fingerprinted || launchedAtMs <= 0)");
  ASSERT_NE(fingerprint, std::string::npos);
  ASSERT_NE(activate, std::string::npos);
  ASSERT_NE(pendingClaim, std::string::npos);
  ASSERT_NE(fingerprintFailure, std::string::npos);
  EXPECT_LT(fingerprint, activate);
  EXPECT_LT(activate, fingerprintFailure);
  EXPECT_LT(activate, pendingClaim);
  EXPECT_EQ(queue.find("m_traktScrobbler"), std::string::npos);
  EXPECT_EQ(load.find("m_traktScrobbler"), std::string::npos);
  EXPECT_EQ(pending.find("rawLaunchUri"), std::string::npos);
  EXPECT_NE(pending.find("intentUrlHash"), std::string::npos);

  const std::string unavailableProfile = FunctionBody(
      process, "if (active.selected && active.sourceBacked && active.credentialsValid");
  const std::string failedClaim = FunctionBody(process, "if (!completion->result.IsClaimed())");
  const std::string invalidContext =
      FunctionBody(process, "if (!context || context->profileId != expectedProfileId ||");
  ASSERT_FALSE(unavailableProfile.empty());
  ASSERT_FALSE(failedClaim.empty());
  ASSERT_FALSE(invalidContext.empty());
  EXPECT_EQ(unavailableProfile.find("AdvanceGeneration"), std::string::npos);
  EXPECT_EQ(failedClaim.find("m_playbackHistoryState.Promote"), std::string::npos);
  EXPECT_EQ(invalidContext.find("m_playbackHistoryState.Promote"), std::string::npos);
  EXPECT_EQ(Count(process, "m_playbackHistoryState.Promote("), 1u);
  EXPECT_EQ(Count(process, "SetClaimedContentInfo("), 1u);
  EXPECT_EQ(process.find("if (context->traktEligible)"), std::string::npos);

  constexpr std::array<const char*, 8> failureClasses{"ambiguous",         "expired",
                                                      "not_found",         "invalid_request",
                                                      "transport_failure", "authentication_failure",
                                                      "http_failure",      "invalid_response"};
  for (const char* failureClass : failureClasses)
    EXPECT_NE(statusName.find(failureClass), std::string::npos) << failureClass;
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
      FunctionBody(app, "void CXBMCApp::LoadAndApplyPairedPlaybackResume(");
  const std::string onContent = FunctionBody(app, "void CXBMCApp::OnContentIdentified()");
  const std::string intent = FunctionBody(app, "void CXBMCApp::onNewIntent(");
  const std::string playbackStarted = FunctionBody(app, "void CXBMCApp::OnPlayBackStarted(");
  ASSERT_FALSE(setClaim.empty());
  ASSERT_FALSE(identify.empty());
  ASSERT_FALSE(processClaim.empty());
  ASSERT_FALSE(loadPaired.empty());
  ASSERT_FALSE(onContent.empty());
  ASSERT_FALSE(intent.empty());
  ASSERT_FALSE(playbackStarted.empty());

  EXPECT_NE(setClaim.find("informationalTraktAuthority ="), std::string::npos);
  EXPECT_NE(setClaim.find("m_dispatcher->BindClaim("), std::string::npos);
  EXPECT_NE(setClaim.find("m_sourceClaimAuthorized = informationalTraktAuthority"),
            std::string::npos);
  EXPECT_NE(setClaim.find("Authenticated claim bound for local-only history"), std::string::npos);
  EXPECT_NE(setClaim.find("return true;"), std::string::npos);
  EXPECT_EQ(Count(processClaim, "context->traktEligible"), 1u);
  EXPECT_NE(processClaim.find("JumpgatePlaybackHistoryNamespace::AuthenticatedProfile"),
            std::string::npos);
  EXPECT_NE(processClaim.find("historyIdentity.profileId = context->profileId"), std::string::npos);
  EXPECT_NE(processClaim.find("historyIdentity.contentKey = *context->contentKey"),
            std::string::npos);
  EXPECT_NE(processClaim.find("const std::string title = context->display.title.value_or"),
            std::string::npos);
  EXPECT_NE(processClaim.find("const std::string logoUrl = context->display.logo.value_or"),
            std::string::npos);
  EXPECT_NE(processClaim.find(
                "const std::string backgroundUrl = context->display.background.value_or"),
            std::string::npos);
  EXPECT_NE(setClaim.find("m_title = title"), std::string::npos);
  EXPECT_NE(setClaim.find("m_logoUrl = logoUrl"), std::string::npos);
  EXPECT_NE(setClaim.find("m_backgroundUrl = backgroundUrl"), std::string::npos);
  EXPECT_EQ(Count(source, "m_backgroundUrl.clear();"), 2u);

  const std::size_t applyClaim = processClaim.find("SetClaimedContentInfo(");
  const std::size_t bindProfile =
      processClaim.find("historyIdentity.profileId = context->profileId");
  const std::size_t bindContent =
      processClaim.find("historyIdentity.contentKey = *context->contentKey");
  const std::size_t promote = processClaim.find("m_playbackHistoryState.Promote(");
  const std::size_t loadResume = processClaim.find("LoadAndApplyPairedPlaybackResume(generation)");
  ASSERT_NE(applyClaim, std::string::npos);
  ASSERT_NE(bindProfile, std::string::npos);
  ASSERT_NE(bindContent, std::string::npos);
  ASSERT_NE(promote, std::string::npos);
  ASSERT_NE(loadResume, std::string::npos);
  EXPECT_LT(applyClaim, bindProfile);
  EXPECT_LT(bindProfile, promote);
  EXPECT_LT(bindContent, promote);
  EXPECT_LT(promote, loadResume);
  EXPECT_NE(loadPaired.find("token->historyNamespace"), std::string::npos);
  EXPECT_NE(loadPaired.find("m_jumpgatePlaybackHistoryStore->Get(key, entry, error)"),
            std::string::npos);
  EXPECT_NE(loadPaired.find("token->previouslyAppliedPositionMs"), std::string::npos);
  EXPECT_NE(loadPaired.find("positionMs == 0"), std::string::npos);
  EXPECT_NE(loadPaired.find("IsJumpgateResumeCorrectionWithinWindow"), std::string::npos);
  EXPECT_EQ(loadPaired.find("appPlayer->GetTime()"), std::string::npos);
  EXPECT_NE(playbackStarted.find("m_externalPlaybackStartedAtSteadyMs.store"), std::string::npos);

  const std::string failedClaim =
      FunctionBody(processClaim, "if (!completion->result.IsClaimed())");
  const std::string invalidContext =
      FunctionBody(processClaim, "if (!context || context->profileId != expectedProfileId ||");
  ASSERT_FALSE(failedClaim.empty());
  ASSERT_FALSE(invalidContext.empty());
  EXPECT_NE(failedClaim.find("return;"), std::string::npos);
  EXPECT_EQ(failedClaim.find("SetClaimedContentInfo"), std::string::npos);
  EXPECT_EQ(failedClaim.find("m_playbackHistoryState.Promote"), std::string::npos);
  EXPECT_NE(invalidContext.find("return;"), std::string::npos);
  EXPECT_EQ(invalidContext.find("SetClaimedContentInfo"), std::string::npos);
  EXPECT_EQ(invalidContext.find("m_playbackHistoryState.Promote"), std::string::npos);

  const std::string pairedIdentify = FunctionBody(identify, "if (m_bridgeProfileBacked)");
  const std::string legacyResume =
      FunctionBody(onContent, "if (!m_traktScrobbler->IsBridgeProfileBacked())");
  ASSERT_FALSE(pairedIdentify.empty());
  ASSERT_FALSE(legacyResume.empty());
  EXPECT_NE(pairedIdentify.find("return m_sourceClaimResolved;"), std::string::npos);
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
  EXPECT_NE(intent.find("LoadAndApplyPairedPlaybackResume(admissionGeneration, false)"),
            std::string::npos);
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
