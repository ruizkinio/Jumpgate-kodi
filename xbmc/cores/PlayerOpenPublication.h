/*
 *  Copyright (C) 2026 Team Jumpgate
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <functional>
#include <utility>

namespace KODI::PLAYER
{

template<typename Item,
         typename PreparePosition,
         typename PrepareMedia,
         typename StopRemote,
         typename VerifyRemoteStopped,
         typename RetainLocalOwnership,
         typename PublishStarted,
         typename PublishAvStarted>
bool PrepareOpenAndPublishStarted(const Item& item,
                                  bool rollbackAllowed,
                                  PreparePosition&& preparePosition,
                                  PrepareMedia&& prepareMedia,
                                  StopRemote&& stopRemote,
                                  VerifyRemoteStopped&& verifyRemoteStopped,
                                  RetainLocalOwnership&& retainLocalOwnership,
                                  PublishStarted&& publishStarted,
                                  PublishAvStarted&& publishAvStarted)
{
  bool metadataRequestsStarted = std::invoke(std::forward<PreparePosition>(preparePosition));
  if (metadataRequestsStarted)
    metadataRequestsStarted = std::invoke(std::forward<PrepareMedia>(prepareMedia));

  if (!metadataRequestsStarted && rollbackAllowed &&
      std::invoke(std::forward<StopRemote>(stopRemote)) &&
      std::invoke(std::forward<VerifyRemoteStopped>(verifyRemoteStopped)))
  {
    return false;
  }

  std::invoke(std::forward<RetainLocalOwnership>(retainLocalOwnership), item);
  std::invoke(std::forward<PublishStarted>(publishStarted), item);
  std::invoke(std::forward<PublishAvStarted>(publishAvStarted), item);
  return true;
}

template<typename Begin, typename Open>
bool BeginDeferredOpenAndOpen(Begin&& begin, Open&& open)
{
  std::invoke(std::forward<Begin>(begin));
  return std::invoke(std::forward<Open>(open));
}

} // namespace KODI::PLAYER
