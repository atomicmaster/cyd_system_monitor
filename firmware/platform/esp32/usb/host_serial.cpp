// SPDX-License-Identifier: Apache-2.0
#include "usb/host_serial.hpp"

#include <atomic>
#include <cstdio>

#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

namespace firmware::platform::esp32::usb {
namespace {

constexpr size_t kRxBufferBytes = 4096;

std::atomic<bool> g_link_mode{false};
StreamBufferHandle_t g_rx = nullptr;

}  // namespace

void EnterLinkMode() {
  if (g_link_mode.load()) return;
  if (g_rx == nullptr) g_rx = xStreamBufferCreate(kRxBufferBytes, 1);
  // Binary frames must pass untouched in both directions, and log text in
  // the middle of a frame would corrupt it.
  uart_vfs_dev_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_LF);
  uart_vfs_dev_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_LF);
  esp_log_level_set("*", ESP_LOG_NONE);
  g_link_mode.store(true);
}

bool LinkModeActive() { return g_link_mode.load(); }

void OnRxByte(uint8_t byte) {
  if (g_rx != nullptr) xStreamBufferSend(g_rx, &byte, 1, 0);
}

size_t Read(uint8_t* out, size_t capacity) {
  if (g_rx == nullptr) return 0;
  return xStreamBufferReceive(g_rx, out, capacity, 0);
}

void Write(std::span<const uint8_t> bytes) {
  std::fwrite(bytes.data(), 1, bytes.size(), stdout);
  std::fflush(stdout);
}

}  // namespace firmware::platform::esp32::usb
