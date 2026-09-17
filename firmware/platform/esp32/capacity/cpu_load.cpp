// SPDX-License-Identifier: Apache-2.0
#include "capacity/cpu_load.hpp"

#include <algorithm>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

namespace firmware::platform::esp32::capacity {

namespace {

// uxTaskGetSystemState needs a caller-owned array sized to the number of
// live tasks; this build's task set (main, dev_console, LVGL/BLE/Wi-Fi
// internals) stays well under this bound. Truncating (rather than
// dynamically sizing) keeps this diagnostic path allocation-free.
constexpr UBaseType_t kMaxTasks = 24;

}  // namespace

std::vector<CpuLoadSample> SampleCpuLoad(uint32_t window_ms) {
  static TaskStatus_t start[kMaxTasks];
  static TaskStatus_t end[kMaxTasks];

  uint32_t total_runtime_start = 0;
  const UBaseType_t start_count = uxTaskGetSystemState(start, kMaxTasks, &total_runtime_start);

  vTaskDelay(pdMS_TO_TICKS(window_ms));

  uint32_t total_runtime_end = 0;
  const UBaseType_t end_count = uxTaskGetSystemState(end, kMaxTasks, &total_runtime_end);

  const uint32_t total_delta = total_runtime_end - total_runtime_start;
  std::vector<CpuLoadSample> samples;
  if (total_delta == 0) {
    return samples;  // Window too short relative to the runtime-stats clock tick.
  }

  for (UBaseType_t i = 0; i < end_count; ++i) {
    // Match this task across the two snapshots by its stable numeric
    // handle-derived task number, not array index: FreeRTOS does not
    // promise the same task occupies the same slot between calls.
    uint32_t runtime_start = 0;
    bool matched = false;
    for (UBaseType_t j = 0; j < start_count; ++j) {
      if (start[j].xTaskNumber == end[i].xTaskNumber) {
        runtime_start = start[j].ulRunTimeCounter;
        matched = true;
        break;
      }
    }
    if (!matched) {
      continue;  // Task created during the window: no baseline to diff against.
    }
    const uint32_t task_delta = end[i].ulRunTimeCounter - runtime_start;
    // total_delta counts both cores' ticks summed, so a fully busy single
    // core reports up to (100 / core_count) here; the caller sums entries
    // per core if a per-core figure is wanted. Kept as a raw share of the
    // combined counter since that is what CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    // actually measures on this dual-core target.
    const uint32_t percent_x100 =
        static_cast<uint32_t>((static_cast<uint64_t>(task_delta) * 10000ULL) / total_delta);
    samples.push_back(CpuLoadSample{
        .task_name = end[i].pcTaskName,
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        .core_id = end[i].xCoreID == tskNO_AFFINITY ? static_cast<uint8_t>(0xFF)
                                                    : static_cast<uint8_t>(end[i].xCoreID),
#else
        .core_id = 0xFF,
#endif
        .percent_x100 = percent_x100,
    });
  }

  std::sort(samples.begin(), samples.end(), [](const CpuLoadSample& a, const CpuLoadSample& b) {
    return a.percent_x100 > b.percent_x100;
  });
  return samples;
}

}  // namespace firmware::platform::esp32::capacity

#else

namespace firmware::platform::esp32::capacity {

std::vector<CpuLoadSample> SampleCpuLoad(uint32_t /*window_ms*/) { return {}; }

}  // namespace firmware::platform::esp32::capacity

#endif
