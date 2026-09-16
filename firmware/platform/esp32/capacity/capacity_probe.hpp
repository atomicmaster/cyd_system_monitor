// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "firmware/domain/capacity.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace firmware::platform::esp32::capacity {

// Records a FreeRTOS task so Sample() reports its stack low-water mark.
// FreeRTOS has no reliable API to enumerate every live task by name, so
// each task this build cares about (main, dev_console, and any future
// display/UI task) must register itself once, right after creation. Safe
// to call for at most kMaxTrackedTasks tasks; further registrations are
// dropped rather than overflowing a fixed table, since this is diagnostic
// instrumentation, not something that can fail boot.
void RegisterTask(const char* name, TaskHandle_t handle);

// Takes one point-in-time capacity reading: LVGL pool peak/available (via
// lv_mem_monitor), current and minimum-ever free heap (via
// esp_get_free_heap_size/esp_get_minimum_free_heap_size), and every
// registered task's stack low-water mark converted from FreeRTOS's native
// word count to bytes. `build_identity` is copied straight into the result
// (see firmware::domain::DiagnosticState::build_identity for the same
// convention).
firmware::domain::capacity::CapacitySample Sample(const std::string& build_identity);

}  // namespace firmware::platform::esp32::capacity
