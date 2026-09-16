// SPDX-License-Identifier: Apache-2.0
//
// Development-only recovery path; TB02 replaces this with the negotiated
// product command. See dev_console.hpp.
#include "dev_console/dev_console.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs/calibration_store.hpp"

namespace firmware::platform::esp32::dev_console {

namespace {

constexpr const char* kTag = "fw.dev_console";
constexpr const char* kClearCalibrationCommand = "DEV:CLEAR_CALIBRATION";
constexpr size_t kLineBufferSize = 128;

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
        if (std::strcmp(line, kClearCalibrationCommand) == 0) {
          ESP_LOGI(kTag, "received %s: clearing calibration record", kClearCalibrationCommand);
          firmware::platform::esp32::nvs::ClearCalibrationRecord();
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
  xTaskCreate(DevConsoleTask, "dev_console", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}

}  // namespace firmware::platform::esp32::dev_console
