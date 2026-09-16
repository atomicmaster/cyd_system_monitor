// SPDX-License-Identifier: Apache-2.0
#include "capacity/capacity_probe.hpp"

#include <lvgl.h>

#include "esp_system.h"

namespace firmware::platform::esp32::capacity {

namespace {

constexpr size_t kMaxTrackedTasks = 8;

struct TrackedTask {
  const char* name = nullptr;
  TaskHandle_t handle = nullptr;
};

TrackedTask g_tracked_tasks[kMaxTrackedTasks];
size_t g_tracked_task_count = 0;

}  // namespace

void RegisterTask(const char* name, TaskHandle_t handle) {
  if (g_tracked_task_count >= kMaxTrackedTasks) {
    return;
  }
  g_tracked_tasks[g_tracked_task_count++] = TrackedTask{name, handle};
}

firmware::domain::capacity::CapacitySample Sample(const std::string& build_identity) {
  firmware::domain::capacity::CapacitySample sample;
  sample.build_identity = build_identity;

  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  sample.lvgl_pool_total_bytes = static_cast<uint32_t>(mon.total_size);
  sample.lvgl_pool_peak_used_bytes = static_cast<uint32_t>(mon.max_used);
  sample.lvgl_pool_available_bytes = static_cast<uint32_t>(mon.free_size);

  sample.free_heap_bytes = esp_get_free_heap_size();
  sample.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();

  for (size_t i = 0; i < g_tracked_task_count; ++i) {
    const TrackedTask& task = g_tracked_tasks[i];
    // uxTaskGetStackHighWaterMark returns the low-water mark in
    // StackType_t words, not bytes; FreeRTOS's own docs specify this.
    const UBaseType_t low_water_words = uxTaskGetStackHighWaterMark(task.handle);
    sample.task_stacks.push_back(firmware::domain::capacity::TaskStackSample{
        .task_name = task.name,
        .stack_low_water_mark_bytes =
            static_cast<uint32_t>(low_water_words) * static_cast<uint32_t>(sizeof(StackType_t)),
    });
  }

  return sample;
}

}  // namespace firmware::platform::esp32::capacity
