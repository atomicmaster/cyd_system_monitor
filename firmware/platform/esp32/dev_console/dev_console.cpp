// SPDX-License-Identifier: Apache-2.0
//
// Development-only recovery path; TB02 replaces this with the negotiated
// product command. See dev_console.hpp.
#include "dev_console/dev_console.hpp"

#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs/calibration_store.hpp"

namespace firmware::platform::esp32::dev_console {

namespace {

constexpr const char* kTag = "fw.dev_console";
constexpr uart_port_t kUartPort =
    UART_NUM_0;  // shared with ESP_LOGI output over the USB-serial bridge
constexpr const char* kClearCalibrationCommand = "DEV:CLEAR_CALIBRATION";
constexpr size_t kLineBufferSize = 128;

void DevConsoleTask(void* /*arg*/) {
  char line[kLineBufferSize];
  size_t line_len = 0;

  for (;;) {
    uint8_t byte = 0;
    const int read = uart_read_bytes(kUartPort, &byte, 1, pdMS_TO_TICKS(1000));
    if (read <= 0) continue;

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
      line[line_len++] = static_cast<char>(byte);
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
