/**
 * @file ui/present_statistics.h
 * @brief Display-side pacing from swap-chain frame statistics (V299)
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::ui {

// Accumulates DXGI_FRAME_STATISTICS samples taken after each Present into a
// window: how many presented images reached the screen and over how many
// refreshes. With a vsync interval of N every displayed image should stay on
// screen for exactly N refreshes; each extra refresh repeated the previous
// image (a missed refresh). This is measured at presentation, not inferred
// from renderer timing. Tearing (immediate) presents are counted but have no
// refresh expectation.
struct PresentStatisticsWindow {
  static constexpr uint32_t kMaxMissRecords = 32;
  struct Miss {
    int64_t sync_qpc;       // scheduler QPC sample of the statistics
    uint32_t refreshes;     // refreshes the repeated image stayed on screen
  };

  uint64_t begin_qpc = 0;
  uint32_t presents = 0;      // Present() calls
  uint32_t samples = 0;       // statistics samples with a newly displayed image
  uint32_t displayed = 0;     // images displayed (PresentCount advance)
  uint32_t refreshes = 0;     // refreshes spanned (PresentRefreshCount advance)
  uint32_t missed = 0;        // refreshes beyond interval * displayed
  uint32_t glitches = 0;      // samples with at least one missed refresh
  uint32_t max_refreshes_per_image = 0;
  uint32_t disjoint = 0;      // statistics unavailable/disjoint (rebaselined)
  uint32_t max_queue = 0;     // presents queued but not yet displayed
  uint32_t miss_records = 0;
  Miss misses[kMaxMissRecords]{};

  bool have_last = false;
  uint32_t last_present_count = 0;
  uint32_t last_present_refresh = 0;

  // Scheduler-side vblank count (SyncRefreshCount) and the displayed-image
  // count at the first and last sample of the window: refreshes elapsed vs
  // images displayed, independent of PresentRefreshCount (which some
  // presentation paths leave unchanged).
  bool have_sync = false;
  uint32_t first_sync_refresh = 0, last_sync_refresh = 0;
  int64_t first_sync_qpc = 0, last_sync_qpc = 0;
  uint32_t first_sync_present = 0, last_sync_present = 0;
  uint32_t first_present_refresh_raw = 0, last_present_refresh_raw = 0;
  int32_t composition_mode = -1;  // DXGI_FRAME_PRESENTATION_MODE, -1 unknown

  // Desktop compositor vblank counter (DwmGetCompositionTimingInfo cRefresh)
  // against the displayed-image count: the refreshes of the window, even when
  // the swap chain reports no refresh counts (composed presentation).
  bool have_dwm = false;
  uint64_t first_dwm_refresh = 0, last_dwm_refresh = 0;
  uint32_t first_dwm_present = 0, last_dwm_present = 0;
  uint64_t dwm_refresh_period_qpc = 0;

  void DwmSample(uint32_t present_count, uint64_t dwm_refresh, uint64_t period_qpc) {
    if (!have_dwm) {
      have_dwm = true;
      first_dwm_refresh = dwm_refresh;
      first_dwm_present = present_count;
    }
    last_dwm_refresh = dwm_refresh;
    last_dwm_present = present_count;
    dwm_refresh_period_qpc = period_qpc;
  }
  uint64_t DwmRefreshes() const {
    return have_dwm && last_dwm_refresh >= first_dwm_refresh
               ? last_dwm_refresh - first_dwm_refresh
               : 0;
  }
  uint32_t DwmDisplayed() const {
    return have_dwm && last_dwm_present >= first_dwm_present
               ? last_dwm_present - first_dwm_present
               : 0;
  }
  // Refreshes that showed no new image for a vsync interval (0: tearing).
  uint64_t DwmMissed(uint32_t interval) const {
    const uint64_t refreshes = DwmRefreshes();
    const uint64_t expected = uint64_t(DwmDisplayed()) * interval;
    return interval && refreshes > expected ? refreshes - expected : 0;
  }

  void SyncSample(uint32_t present_count, uint32_t present_refresh_count,
                  uint32_t sync_refresh_count, int64_t sync_qpc) {
    if (!have_sync) {
      have_sync = true;
      first_sync_refresh = sync_refresh_count;
      first_sync_qpc = sync_qpc;
      first_sync_present = present_count;
      first_present_refresh_raw = present_refresh_count;
    }
    last_sync_refresh = sync_refresh_count;
    last_sync_qpc = sync_qpc;
    last_sync_present = present_count;
    last_present_refresh_raw = present_refresh_count;
  }
  uint32_t SyncRefreshes() const {
    return have_sync && last_sync_refresh >= first_sync_refresh
               ? last_sync_refresh - first_sync_refresh
               : 0;
  }
  uint32_t SyncDisplayed() const {
    return have_sync && last_sync_present >= first_sync_present
               ? last_sync_present - first_sync_present
               : 0;
  }
  // Refreshes that showed no new image, for a vsync interval.
  uint32_t SyncMissed(uint32_t interval) const {
    const uint32_t refreshes = SyncRefreshes();
    const uint32_t expected = SyncDisplayed() * interval;
    return interval && refreshes > expected ? refreshes - expected : 0;
  }

  void CountPresent(uint32_t queued) {
    ++presents;
    max_queue = std::max(max_queue, queued);
  }

  void Disjoint() {
    ++disjoint;
    have_last = false;
  }

  // present_count / present_refresh_count: the last displayed image's
  // PresentCount and PresentRefreshCount. interval: vsync interval of the
  // presents (0 = tearing, no refresh expectation).
  void Sample(uint32_t present_count, uint32_t present_refresh_count, int64_t sync_qpc,
              uint32_t interval) {
    if (!have_last || present_count < last_present_count ||
        present_refresh_count < last_present_refresh) {
      have_last = true;
      last_present_count = present_count;
      last_present_refresh = present_refresh_count;
      return;
    }
    if (present_count == last_present_count) {
      return;  // the newest presents are still queued
    }
    const uint32_t images = present_count - last_present_count;
    const uint32_t spanned = present_refresh_count - last_present_refresh;
    ++samples;
    displayed += images;
    refreshes += spanned;
    if (images == 1) {
      max_refreshes_per_image = std::max(max_refreshes_per_image, spanned);
    }
    if (interval) {
      const uint32_t expected = images * interval;
      if (spanned > expected) {
        missed += spanned - expected;
        ++glitches;
        if (miss_records < kMaxMissRecords) {
          misses[miss_records++] = {sync_qpc, spanned - expected + interval};
        }
      }
    }
    last_present_count = present_count;
    last_present_refresh = present_refresh_count;
  }

  // Starts the next window; the displayed-image baseline carries over, and
  // the next window's scheduler span starts where this one ended.
  void Reset(uint64_t qpc) {
    const bool keep = have_last;
    const uint32_t count = last_present_count;
    const uint32_t refresh = last_present_refresh;
    const bool keep_sync = have_sync;
    const uint32_t sync_refresh = last_sync_refresh;
    const int64_t sync_qpc = last_sync_qpc;
    const uint32_t sync_present = last_sync_present;
    const uint32_t present_refresh_raw = last_present_refresh_raw;
    const bool keep_dwm = have_dwm;
    const uint64_t dwm_refresh = last_dwm_refresh;
    const uint32_t dwm_present = last_dwm_present;
    const uint64_t dwm_period = dwm_refresh_period_qpc;
    *this = {};
    begin_qpc = qpc;
    have_last = keep;
    last_present_count = count;
    last_present_refresh = refresh;
    if (keep_sync) {
      SyncSample(sync_present, present_refresh_raw, sync_refresh, sync_qpc);
    }
    if (keep_dwm) {
      DwmSample(dwm_present, dwm_refresh, dwm_period);
    }
  }
};

}  // namespace rex::ui
