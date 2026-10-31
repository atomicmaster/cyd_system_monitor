// SPDX-License-Identifier: Apache-2.0
#include "ble_scanner.hpp"

namespace firmware::platform::esp32::radio {

namespace {

using domain::DriverResult;
using domain::FaultOp;

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
  if (!transport_.Recover()) {
    init_result_ = {false, FaultOp::kInit, kCodeControllerReset};
    return false;
  }
  scanning_ = false;  // a reset controller scans nothing
  return true;
}

DriverResult BleScanner::SetScan(bool enable) {
  domain::hci::Options options;
  options.enable = enable;
  const CommandResult result = transport_.Execute(domain::hci::Command::kLeSetScanEnable, options,
                                                  HciTransport::kCommandTimeoutUs);
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
    return Failed(FaultOp::kHciRejected, kCodeHardwareErrorBase | hardware_error);
  }
  // Sent or not, a timed-out enable may have started the scan: Stop must disable it.
  scanning_ = true;
  const DriverResult result = SetScan(true);
  if (!init_result_.ok) return init_result_;  // the controller could not be recovered
  return result;
}

DriverResult BleScanner::Stop() {
  if (!init_result_.ok) return {};  // nothing was ever started
  if (!scanning_) return {};
  const DriverResult result = SetScan(false);
  if (result.ok) scanning_ = false;
  // A controller reset during SetScan left scanning_ false, which also closes the window.
  if (!result.ok && !scanning_) return {};
  return result;
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
