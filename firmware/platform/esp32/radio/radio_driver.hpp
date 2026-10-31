// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include "ble_scanner.hpp"
#include "firmware/domain/radio_runner.hpp"
#include "hci_transport.hpp"
#include "observation_gate.hpp"
#include "wifi_monitor.hpp"

namespace firmware::platform::esp32::radio {

// The ESP32 implementation of the domain RadioDriver seam. It owns the receive
// paths (WiFi promiscuous, BLE over VHCI) and the observation gate. All
// RadioDriver methods are called from the radio task only.
class Esp32RadioDriver final : public domain::RadioDriver {
 public:
  // Notification bit that ControlSource enqueueing sets on the radio task.
  static constexpr uint32_t kControlNotifyBit = 1u << 0;

  Esp32RadioDriver() : wifi_(gate_), ble_(hci_, gate_) {}

  // Allocates the HCI buffers and starts the HCI worker; returns at once.
  bool StartWorkers() { return ble_.StartTransport(); }
  TaskHandle_t hci_worker() const { return hci_.worker(); }

  // Brings both receivers up, one after the other (their PHY bring-up must not
  // overlap). A failure faults only that radio, at its first window.
  void InitReceivers();

  uint64_t NowUs() override;
  void BeginWindow(uint32_t window_id, uint32_t config_generation, uint64_t start_us,
                   uint64_t deadline_us) override;
  void EndWindow() override;
  domain::DriverResult StartWifi(uint8_t channel) override;
  domain::DriverResult StopWifi() override;
  domain::DriverResult StartBle() override;
  domain::DriverResult StopBle() override;
  domain::WakeReason WaitUntilUs(uint64_t deadline_us) override;
  void Yield() override;
  void DrainEvents() override;
  uint32_t TakeDrops(uint32_t window_id) override;
  uint32_t TakeUnattributed() override;

  const ObservationGate& gate() const { return gate_; }
  const HciTransport& hci() const { return hci_; }
  // The bring-up results, readable from other tasks (the console).
  domain::DriverResult wifi_init() const { return Load(wifi_init_ok_, wifi_init_code_); }
  domain::DriverResult ble_init() const { return Load(ble_init_ok_, ble_init_code_); }
  uint32_t scan_stop_already_off() const { return ble_.stop_already_off(); }

 private:
  ObservationGate gate_;
  HciTransport hci_;
  WifiMonitor wifi_;
  BleScanner ble_;
  domain::Radio window_radio_ = domain::Radio::kWifi;  // the receiver Start* last turned on
  static domain::DriverResult Load(const std::atomic<bool>& ok, const std::atomic<int32_t>& code) {
    return ok.load(std::memory_order_acquire)
               ? domain::DriverResult{}
               : domain::DriverResult{false, domain::FaultOp::kInit,
                                      code.load(std::memory_order_relaxed)};
  }
  static void Store(std::atomic<bool>& ok, std::atomic<int32_t>& code,
                    const domain::DriverResult& result) {
    code.store(result.code, std::memory_order_relaxed);
    ok.store(result.ok, std::memory_order_release);
  }

  std::atomic<bool> wifi_init_ok_{false};
  std::atomic<int32_t> wifi_init_code_{0};
  std::atomic<bool> ble_init_ok_{false};
  std::atomic<int32_t> ble_init_code_{0};
};

}  // namespace firmware::platform::esp32::radio
