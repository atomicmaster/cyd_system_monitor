// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace firmware::domain::capacity {

// F08a's declared upper bounds for the bounded operational UI capacity
// slice (Work item 3). These are enforced by firmware::ui::capacity's
// screen builders, not just documented: a caller can hand in more alerts
// or settings than this, and the slice still renders no more than these
// counts. Values are provisional until the physical board session records
// real responsiveness/memory behavior against them.
inline constexpr uint32_t kMaxAlertListRows = 8;
inline constexpr uint32_t kMaxAlertLabelLength = 48;
inline constexpr uint32_t kMaxAlertDetailLength = 256;
inline constexpr uint32_t kMaxSettingsRows = 8;
inline constexpr uint32_t kMaxSettingLabelLength = 32;
inline constexpr uint32_t kMaxSettingValueLength = 32;
inline constexpr uint32_t kMaxLiveStatusTextLength = 96;
// Matches the MVP's one-second Host snapshot cadence (see
// IMPLEMENTATION_PLAN.md's M1a/M3 sections): the live status screen never
// redraws more often than the data it shows can actually change.
inline constexpr uint32_t kMinLiveStatusRefreshIntervalMs = 1000;

// One FreeRTOS task's stack low-water mark, in bytes (already converted
// from FreeRTOS's native word count -- see
// firmware/platform/esp32/capacity/capacity_probe.cpp).
struct TaskStackSample {
  std::string task_name;
  uint32_t stack_low_water_mark_bytes = 0;
};

// One point-in-time capacity measurement taken on the flashed build (Work
// item 1: LVGL pool peak/available, minimum heap, task-stack low-water
// marks). platform/esp32 populates this from real hardware; there is no
// simulator equivalent because none of these readings mean anything off
// real hardware.
struct CapacitySample {
  std::string build_identity;

  uint32_t lvgl_pool_total_bytes = 0;
  uint32_t lvgl_pool_peak_used_bytes = 0;
  uint32_t lvgl_pool_available_bytes = 0;

  uint32_t free_heap_bytes = 0;
  uint32_t minimum_free_heap_bytes = 0;

  std::vector<TaskStackSample> task_stacks;
};

// Derives a conservative UI/runtime DRAM budget from a CapacitySample (Work
// item 6): how much of the measured headroom the UI can safely hand back
// to radio, after excluding allocations the capacity slice declares it
// cannot release once built (e.g. the LVGL pool itself).
struct CapacityBudget {
  CapacitySample sample;
  uint32_t non_reclaimable_bytes = 0;

  // The portion of minimum_free_heap_bytes not already claimed by
  // non_reclaimable_bytes. Saturates at zero rather than going negative --
  // a non_reclaimable declaration larger than the measured headroom means
  // there is no safe reclaimable capacity, not a signed deficit.
  uint32_t reclaimable_bytes() const;

  // True if any measured task stack has less than `floor_bytes` of
  // low-water-mark headroom left -- a signal that the capacity slice's
  // task stacks are undersized for the exercised load, independent of
  // overall heap headroom.
  bool any_task_stack_below(uint32_t floor_bytes) const;
};

}  // namespace firmware::domain::capacity
