// SPDX-License-Identifier: Apache-2.0
//
// Development-only recovery path; TB02 replaces this with the negotiated
// product command. See dev_console.hpp. F07 additionally uses this console
// to trigger on-demand hardware exercises (LED, audio, peripheral status)
// during physical bring-up, since it is the only channel available before
// TB02's real product protocol exists.
#include "dev_console/dev_console.hpp"

#include <cstdio>
#include <cstring>

#include "audio/audio.hpp"
#include "battery/battery.hpp"
#include "build_identity.hpp"
#include "button/button.hpp"
#include "capacity/capacity_probe.hpp"
#include "esp_log.h"
#include "expansion/expansion.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led/led.hpp"
#include "microsd/microsd.hpp"
#include "nvs/calibration_store.hpp"

namespace firmware::platform::esp32::dev_console {

namespace {

constexpr const char* kTag = "fw.dev_console";
constexpr size_t kLineBufferSize = 128;

// Cycles red, green, blue for 800ms each, then off. F07's acceptance line
// "LEDs ... are visible and reproducible" needs an actual driven sequence
// to observe -- board_init only configures the PWM channels, it never
// sets a color, so without this the LED never lights at all.
void RunLedTest() {
  ESP_LOGI(kTag, "DEV:LED_TEST: red");
  led::SetColor(255, 0, 0);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: green");
  led::SetColor(0, 255, 0);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: blue");
  led::SetColor(0, 0, 255);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: off");
  led::SetColor(0, 0, 0);
}

// Deliberately enables the amplifier and plays a short tone, then disables
// it again -- optional sound defaults off per the profile, so this is the
// only way to exercise it. Matches F07's Work item 2 ("exercise optional
// sound deliberately").
void RunAudioTest() {
  ESP_LOGI(kTag, "DEV:AUDIO_TEST: enabling amplifier, playing 1kHz tone for 500ms");
  audio::SetAmplifierEnabled(true);
  audio::PlayTestTone(1000, 500);
  audio::SetAmplifierEnabled(false);
  ESP_LOGI(kTag, "DEV:AUDIO_TEST: amplifier disabled");
}

// Re-probes MicroSD/battery/BOOT-button/expansion live (unlike
// board_init.cpp's once-at-boot probe) so an operator can insert/remove a
// card, press/release BOOT, or jumper the expansion pin between runs and
// see the result change -- the concrete meaning of F07's "visible and
// reproducible" for these.
void RunPeripheralStatus() {
  const auto sd = microsd::Probe();
  const int battery_mv = battery::ReadMillivolts();
  const bool button_pressed = button::IsPressed();
  const bool expansion_high = expansion::IsHigh();
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: microsd_present=%d microsd_detail=\"%s\"", sd.present,
           sd.detail.c_str());
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: battery_mv=%d", battery_mv);
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: boot_button_pressed=%d", button_pressed);
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: expansion_high=%d", expansion_high);
}

// F08a Work item 1: surfaces LVGL pool peak/available, minimum free heap,
// and every registered task's stack low-water mark over UART, since this
// board has no other channel to report them before TB02's real product
// protocol exists. Re-probes live like RunPeripheralStatus, so an operator
// can request a fresh reading after exercising the capacity slice/radio
// load rather than only ever seeing a one-shot boot value.
void RunCapacityStatus() {
  const auto sample =
      firmware::platform::esp32::capacity::Sample(firmware::platform::esp32::kBuildIdentity);
  ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: build=%s", sample.build_identity.c_str());
  ESP_LOGI(kTag,
           "DEV:CAPACITY_STATUS: lvgl_pool_total_bytes=%lu lvgl_pool_peak_used_bytes=%lu "
           "lvgl_pool_available_bytes=%lu",
           static_cast<unsigned long>(sample.lvgl_pool_total_bytes),
           static_cast<unsigned long>(sample.lvgl_pool_peak_used_bytes),
           static_cast<unsigned long>(sample.lvgl_pool_available_bytes));
  ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: free_heap_bytes=%lu minimum_free_heap_bytes=%lu",
           static_cast<unsigned long>(sample.free_heap_bytes),
           static_cast<unsigned long>(sample.minimum_free_heap_bytes));
  for (const auto& task : sample.task_stacks) {
    ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: task=%s stack_low_water_mark_bytes=%lu",
             task.task_name.c_str(), static_cast<unsigned long>(task.stack_low_water_mark_bytes));
  }
}

struct Command {
  const char* line;
  void (*run)();
};

void RunClearCalibration() {
  ESP_LOGI(kTag, "received DEV:CLEAR_CALIBRATION: clearing calibration record");
  firmware::platform::esp32::nvs::ClearCalibrationRecord();
}

constexpr Command kCommands[] = {
    {"DEV:CLEAR_CALIBRATION", RunClearCalibration},
    {"DEV:LED_TEST", RunLedTest},
    {"DEV:AUDIO_TEST", RunAudioTest},
    {"DEV:PERIPHERAL_STATUS", RunPeripheralStatus},
    {"DEV:CAPACITY_STATUS", RunCapacityStatus},
};

// UART0 is already owned by ESP-IDF's console/log VFS layer (ESP_LOGI,
// idf.py monitor): it reads/writes UART0 through esp_vfs_console's own
// polling driver, which never calls uart_driver_install(). The separate
// ring-buffer driver/uart.h API (uart_read_bytes()) requires that install
// call to have happened -- calling it here without one fails instantly on
// every invocation instead of blocking, spinning this task at full speed
// and starving the idle task's watchdog reset (observed as a task_wdt trip
// on IDLE1 with dev_console on the backtrace). Reading through stdin
// instead uses the same already-installed console VFS driver UART0 is
// wired to, so it blocks for real and doesn't fight over ownership of the
// port.
void DevConsoleTask(void* /*arg*/) {
  char line[kLineBufferSize];
  size_t line_len = 0;

  for (;;) {
    const int ch = getchar();
    if (ch == EOF) {
      // No console input driver configured, or a transient read error:
      // don't spin.
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const char byte = static_cast<char>(ch);

    if (byte == '\n' || byte == '\r') {
      if (line_len > 0) {
        line[line_len] = '\0';
        bool matched = false;
        for (const Command& command : kCommands) {
          if (std::strcmp(line, command.line) == 0) {
            command.run();
            matched = true;
            break;
          }
        }
        if (!matched) {
          ESP_LOGW(kTag, "unrecognized dev console line: \"%s\"", line);
        }
        line_len = 0;
      }
      continue;
    }

    if (line_len < kLineBufferSize - 1) {
      line[line_len++] = byte;
    } else {
      // Overlong line: not a recognized command either way; drop it and
      // resync on the next newline rather than growing an unbounded buffer.
      line_len = 0;
    }
  }
}

}  // namespace

void StartDevConsole() {
  TaskHandle_t handle = nullptr;
  xTaskCreate(DevConsoleTask, "dev_console", 4096, nullptr, tskIDLE_PRIORITY + 1, &handle);
  firmware::platform::esp32::capacity::RegisterTask("dev_console", handle);
}

}  // namespace firmware::platform::esp32::dev_console
