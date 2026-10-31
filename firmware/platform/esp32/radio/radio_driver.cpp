// SPDX-License-Identifier: Apache-2.0
#include "radio_driver.hpp"

#include <algorithm>
#include <limits>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace firmware::platform::esp32::radio {

namespace {

constexpr uint64_t kTickUs = 1'000'000ULL / configTICK_RATE_HZ;

}  // namespace

void Esp32RadioDriver::InitReceivers() {
  wifi_init_ = wifi_.Init();
  ble_init_ = ble_.Init();
}

uint64_t Esp32RadioDriver::NowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

void Esp32RadioDriver::BeginWindow(uint32_t window_id, uint32_t config_generation,
                                   uint64_t start_us, uint64_t deadline_us) {
  gate_.BeginWindow(window_radio_, window_id, config_generation, start_us, deadline_us);
}

void Esp32RadioDriver::EndWindow() { gate_.EndWindow(); }

domain::DriverResult Esp32RadioDriver::StartWifi(uint8_t channel) {
  window_radio_ = domain::Radio::kWifi;
  return wifi_.Start(channel);
}

domain::DriverResult Esp32RadioDriver::StopWifi() { return wifi_.Stop(); }

domain::DriverResult Esp32RadioDriver::StartBle() {
  window_radio_ = domain::Radio::kBle;
  return ble_.Start();
}

domain::DriverResult Esp32RadioDriver::StopBle() { return ble_.Stop(); }

domain::WakeReason Esp32RadioDriver::WaitUntilUs(uint64_t deadline_us) {
  TickType_t ticks = portMAX_DELAY;
  if (deadline_us != std::numeric_limits<uint64_t>::max()) {
    const uint64_t now_us = NowUs();
    if (now_us >= deadline_us) return domain::WakeReason::kDeadline;
    // Rounded up, so the wait is never shorter than asked (the runner re-checks the clock).
    ticks = static_cast<TickType_t>(std::min<uint64_t>(
        (deadline_us - now_us + kTickUs - 1) / kTickUs, std::numeric_limits<uint32_t>::max() - 1));
  }
  uint32_t bits = 0;
  // The control bit is cleared as the wait returns, so one wake is one notification.
  if (xTaskNotifyWait(0, kControlNotifyBit, &bits, ticks) == pdTRUE &&
      (bits & kControlNotifyBit) != 0) {
    return domain::WakeReason::kControl;
  }
  return domain::WakeReason::kDeadline;
}

void Esp32RadioDriver::DrainEvents() { gate_.Drain(); }

uint32_t Esp32RadioDriver::TakeDrops(uint32_t window_id) { return gate_.TakeDrops(window_id); }

uint32_t Esp32RadioDriver::TakeUnattributed() { return gate_.TakeUnattributed(); }

}  // namespace firmware::platform::esp32::radio
