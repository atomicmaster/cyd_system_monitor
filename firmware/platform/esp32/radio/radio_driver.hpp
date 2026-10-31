// SPDX-License-Identifier: Apache-2.0
#pragma once

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
  void DrainEvents() override;
  uint32_t TakeDrops(uint32_t window_id) override;
  uint32_t TakeUnattributed() override;

  const ObservationGate& gate() const { return gate_; }
  const HciTransport& hci() const { return hci_; }
  domain::DriverResult wifi_init() const { return wifi_init_; }
  domain::DriverResult ble_init() const { return ble_init_; }

 private:
  ObservationGate gate_;
  HciTransport hci_;
  WifiMonitor wifi_;
  BleScanner ble_;
  domain::Radio window_radio_ = domain::Radio::kWifi;  // the receiver Start* last turned on
  domain::DriverResult wifi_init_{false, domain::FaultOp::kInit, 0};
  domain::DriverResult ble_init_{false, domain::FaultOp::kInit, 0};
};

}  // namespace firmware::platform::esp32::radio
