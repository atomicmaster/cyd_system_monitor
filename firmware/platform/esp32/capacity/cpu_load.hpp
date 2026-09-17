// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace firmware::platform::esp32::capacity {

// One task's share of total CPU runtime over the sampling window (F08 Work
// item 2: CPU load under combined radio/UI load). ESP32-only, like the
// rest of this directory -- FreeRTOS's runtime-stats counters have no
// simulator equivalent, so there is no firmware::domain::capacity type for
// this.
struct CpuLoadSample {
  std::string task_name;
  uint8_t core_id = 0;        // 0, 1, or 0xFF if unpinned/unknown.
  uint32_t percent_x100 = 0;  // Percent of the window, times 100 (two
                              // implied decimal digits, since this is
                              // logged as plain text over UART).
};

// Samples FreeRTOS's per-task runtime counters twice, `window_ms` apart,
// and returns each task's share of total run time over that window
// (idle-task entries included, so idle percentage is directly the
// inverse of busy CPU load). Requires
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; the caller decides how the
// result is surfaced (DEV:CPU_STATUS logs it directly).
//
// Blocks the calling task for `window_ms`: not safe to call from an ISR
// or a task with a tight deadline. Intended for the dev-console command
// path only.
std::vector<CpuLoadSample> SampleCpuLoad(uint32_t window_ms);

}  // namespace firmware::platform::esp32::capacity
