/*
 *  Copyright (C) 2024 Team Jumpgate
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

namespace Jumpgate
{
// ============================================================================
// Completion Thresholds
// ============================================================================
//
// These thresholds are INTENTIONALLY DIFFERENT per use case:
//
// TRAKT_HISTORY_SYNC_PCT (80%):
//   Trakt convention -- mark as "watched" when most of the content has been
//   viewed. Lower than resume thresholds because users may skip credits but
//   still consider the content "watched". Matches behavior of official Kodi
//   Trakt addon.
//
// RESUME_CLEAR_RATIO (90%):
//   Clear the resume bookmark on save. The user is close enough to done that
//   resuming would land in credits or post-credits. Higher than the Trakt
//   threshold because we want to allow resume at 85% but still mark as
//   watched on Trakt at 80%.
//
// RESUME_DISCARD_RATIO (95%):
//   Discard stale resume data on LOAD. If the user was at 95%+ they
//   effectively finished. Separate from RESUME_CLEAR because Bridge resume
//   data may arrive slightly stale (saved at 93%, actual was 96%).
//
// See also: stremio-addon/index.js top-level constants (JS equivalents)
// ============================================================================

constexpr float TRAKT_HISTORY_SYNC_PCT = 80.0f;   // percentage (0-100 scale)
constexpr float RESUME_CLEAR_RATIO = 0.9f;         // ratio (0.0-1.0 scale)
constexpr float RESUME_DISCARD_RATIO = 0.95f;      // ratio (0.0-1.0 scale)

} // namespace Jumpgate
