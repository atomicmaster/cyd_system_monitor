// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/radio_messages.hpp"

#include <algorithm>

namespace firmware::domain {

namespace {

const char* OpName(FaultOp op) {
  switch (op) {
    case FaultOp::kInit:
      return "init";
    case FaultOp::kStart:
      return "start";
    case FaultOp::kStop:
      return "stop";
    case FaultOp::kSetChannel:
      return "set channel";
    case FaultOp::kHciTimeout:
      return "hci timeout";
    case FaultOp::kHciRejected:
      return "hci rejected";
    case FaultOp::kReportQueueOverflow:
      return "report overflow";
    case FaultOp::kHardwareError:
      return "hardware error";
  }
  return "fault";
}

}  // namespace

std::string FaultText(FaultOp op, int32_t code) {
  std::string text = OpName(op);
  if (code != 0) text += " (" + std::to_string(code) + ")";
  return text;
}

RadioMessage RadioMessage::WindowStarted(const WindowStartedMessage& started) {
  RadioMessage message;
  message.kind = RadioMessageKind::kWindowStarted;
  message.started = started;
  return message;
}

RadioMessage RadioMessage::WindowReported(const WindowReport& report) {
  RadioMessage message;
  message.kind = RadioMessageKind::kWindowReport;
  message.report = report;
  return message;
}

RadioMessage RadioMessage::Fault(Radio radio, FaultOp op, int32_t code) {
  RadioMessage message;
  message.kind = RadioMessageKind::kFault;
  message.fault = {radio, op, code};
  return message;
}

RadioMessage RadioMessage::FaultCleared(Radio radio) {
  RadioMessage message;
  message.kind = RadioMessageKind::kFaultCleared;
  message.cleared_radio = radio;
  return message;
}

RadioMessage RadioMessage::ConfigApplied(const ConfigAppliedMessage& config) {
  RadioMessage message;
  message.kind = RadioMessageKind::kConfigApplied;
  message.config = config;
  return message;
}

ConfigAppliedMessage ToConfigApplied(uint32_t config_generation, const ObservationConfig& config) {
  const ObservationConfig effective = NormalizeConfig(config);
  ConfigAppliedMessage message;
  message.config_generation = config_generation;
  message.wifi_enabled = effective.wifi_enabled;
  message.ble_enabled = effective.ble_enabled;
  message.wifi_dwell_ms = effective.wifi_dwell_ms;
  message.ble_window_ms = effective.ble_window_ms;
  const size_t count = std::min(effective.wifi_channels.size(), kMaxPlanChannels);
  message.channel_count = static_cast<uint8_t>(count);
  std::copy_n(effective.wifi_channels.begin(), count, message.channels);
  return message;
}

ObservationConfig ToObservationConfig(const ConfigAppliedMessage& message) {
  ObservationConfig config;
  config.wifi_enabled = message.wifi_enabled;
  config.ble_enabled = message.ble_enabled;
  const size_t count = std::min<size_t>(message.channel_count, kMaxPlanChannels);
  config.wifi_channels.assign(message.channels, message.channels + count);
  config.wifi_dwell_ms = message.wifi_dwell_ms;
  config.ble_window_ms = message.ble_window_ms;
  return NormalizeConfig(std::move(config));
}

}  // namespace firmware::domain
