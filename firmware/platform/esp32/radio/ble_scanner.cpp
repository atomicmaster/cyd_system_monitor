// SPDX-License-Identifier: Apache-2.0
#include "ble_scanner.hpp"

namespace firmware::platform::esp32::radio {

namespace {

using domain::DriverResult;
using domain::FaultOp;

// HCI status Command Disallowed.
constexpr uint8_t kHciCommandDisallowed = 0x0c;

}  // namespace

bool BleScanner::StartTransport() {
  transport_started_ = transport_.Start(&BleScanner::OnReport, this);
  return transport_started_;
}

DriverResult BleScanner::Init() {
  if (!transport_started_) {
    init_result_ = {false, FaultOp::kInit, ESP_ERR_NO_MEM};
    return init_result_;
  }
  const esp_err_t err = transport_.BringUp();
  init_result_ = err == ESP_OK ? DriverResult{} : DriverResult{false, FaultOp::kInit, err};
  return init_result_;
}

DriverResult BleScanner::Failed(FaultOp op, int32_t code) { return {false, op, code}; }

DriverResult BleScanner::Map(const CommandResult& result, FaultOp op) {
  switch (result.code) {
    case CommandResult::Code::kCompleted:
      return {};
    case CommandResult::Code::kRejected:
      return Failed(FaultOp::kHciRejected, result.detail);
    case CommandResult::Code::kTimedOut:
      return Failed(FaultOp::kHciTimeout, 0);
    case CommandResult::Code::kUnavailable:
      return Failed(op, kCodeNotIssuedBase + result.detail);
    case CommandResult::Code::kNeedsReset:
      return Failed(op, kCodeControllerReset);
  }
  return Failed(op, -1);
}

bool BleScanner::RecoverController() {
  switch (transport_.Recover()) {
    case HciTransport::Recovery::kRecovered:
      scanning_ = false;
      init_result_ = {};
      return true;
    case HciTransport::Recovery::kCycledNotBooted:
      scanning_ = false;
      init_result_ = {false, FaultOp::kInit, kCodeControllerReset};
      return false;
    case HciTransport::Recovery::kNotCycled:
      init_result_ = {false, FaultOp::kInit, kCodeControllerReset};
      return false;
  }
  return false;
}

DriverResult BleScanner::SetScan(bool enable) {
  domain::hci::Options options;
  options.enable = enable;
  const CommandResult result = transport_.Execute(domain::hci::Command::kLeSetScanEnable, options,
                                                  HciTransport::kCommandTimeoutUs);
  // Whether a scan is running is only known to have changed once the packet was
  // actually sent: a command that never left leaves the state as it was.
  if (enable && result.sent) scanning_ = true;
  if (!enable) {
    if (result.code == CommandResult::Code::kCompleted) {
      scanning_ = false;
    } else if (result.code == CommandResult::Code::kRejected &&
               result.detail == kHciCommandDisallowed) {
      // Disabling a scan that is not running: it is off, which is what was asked.
      stop_already_off_.fetch_add(1, std::memory_order_relaxed);
      scanning_ = false;
      return {};
    }
  }
  if (result.code == CommandResult::Code::kNeedsReset) {
    // The window is lost either way; recovering now leaves the next one clean.
    RecoverController();
  }
  return Map(result, enable ? FaultOp::kStart : FaultOp::kStop);
}

DriverResult BleScanner::Start() {
  if (!init_result_.ok) return init_result_;
  uint8_t hardware_error = 0;
  if (transport_.TakeHardwareError(&hardware_error)) {
    // The controller said it is broken: reset it and lose this window.
    if (!RecoverController()) return init_result_;
    return Failed(FaultOp::kHardwareError, hardware_error);
  }
  const DriverResult result = SetScan(true);
  if (!init_result_.ok) return init_result_;  // the controller could not be recovered
  return result;
}

DriverResult BleScanner::Stop() {
  if (!scanning_) return {};
  // While a scan may be running Stop keeps failing, so the runner stays closed
  // and keeps retrying: first the disable command, then a controller cycle.
  // This holds even when the radio is latched failed.
  const DriverResult disabled = SetScan(false);
  if (!scanning_) return {};
  RecoverController();
  if (!scanning_) return {};
  return disabled.ok ? Failed(FaultOp::kStop, kCodeControllerReset) : disabled;
}

void BleScanner::OnReport(const domain::hci::Report& report, uint64_t arrival_us, void* context) {
  auto* self = static_cast<BleScanner*>(context);
  domain::ObservationEvent event;
  event.protocol = domain::EventProtocol::kBle;
  event.channel = 0;
  event.monotonic_us = arrival_us;
  event.rssi = report.rssi;
  event.metadata_kind = domain::EventMetadataKind::kBle;
  event.ble.adv_type = report.event_type;
  event.ble.addr_type = report.address_type;
  event.ble.addr = report.address;
  event.ble.data_len = report.data_length;
  self->gate_.Offer(event);
}

}  // namespace firmware::platform::esp32::radio
