// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "firmware/domain/radio_runner.hpp"
#include "hci_transport.hpp"
#include "observation_gate.hpp"

namespace firmware::platform::esp32::radio {

// Passive BLE scanning for Observation Windows: LE Set Scan Enable, issued
// through the HCI transport and waited for with a checked, bounded completion.
// Scan parameters (interval == window == 0x50, passive) are set once by the
// boot sequence; duplicate filtering is off. It never sends anything but HCI
// commands to the local controller.
class BleScanner {
 public:
  // Fault codes for FaultOp::kStart / kStop / kHciTimeout when no HCI status applies.
  static constexpr int32_t kCodeNotIssuedBase = 100;  // + hci::IssueResult
  static constexpr int32_t kCodeControllerReset = 200;
  // A Hardware Error event: 0x100 | its HCI status, reported as kHciRejected.
  static constexpr int32_t kCodeHardwareErrorBase = 0x100;

  BleScanner(HciTransport& transport, ObservationGate& gate) : transport_(transport), gate_(gate) {}

  // Allocates the HCI buffers and starts the worker task; cheap, safe from the
  // boot task. False when memory or the task is missing (Init then fails).
  bool StartTransport();
  // Initialises and boots the controller. The result is kept: a failed init is
  // reported by Start() as FaultOp::kInit and never retried.
  domain::DriverResult Init();

  domain::DriverResult Start();
  domain::DriverResult Stop();

  // Worker-side advertising handler (HciTransport::Start).
  static void OnReport(const domain::hci::Report& report, uint64_t arrival_us, void* context);

 private:
  domain::DriverResult Failed(domain::FaultOp op, int32_t code);
  domain::DriverResult Map(const CommandResult& result, domain::FaultOp op);
  // A controller that stopped answering or raised a Hardware Error: reset it.
  // Returns whether it is usable again; if not, the radio stays faulted.
  bool RecoverController();
  domain::DriverResult SetScan(bool enable);

  HciTransport& transport_;
  ObservationGate& gate_;
  domain::DriverResult init_result_{false, domain::FaultOp::kInit, 0};
  bool transport_started_ = false;
  bool scanning_ = false;  // a scan enable was sent and no disable has completed since
};

}  // namespace firmware::platform::esp32::radio
