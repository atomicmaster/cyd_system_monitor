// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

#include "firmware/domain/coverage.hpp"

namespace firmware::domain {

// What the radio task tells the app loop. Every message is fixed-size and
// trivially copyable, so it crosses a task queue by value with no allocation.
// The app loop feeds kWindowReport to CoverageLedger::Record, kFault and
// kFaultCleared to SetFault/ClearFault (text from FaultText), and keeps the
// kConfigApplied configuration for MakeCoverageView.

// The driver step that failed.
enum class FaultOp : uint8_t {
  kInit,
  kStart,
  kStop,
  kSetChannel,
  kHciTimeout,
  kHciRejected,
  kReportQueueOverflow,
};

// Short reason for CoverageLedger::SetFault, e.g. "start (-258)"; the code is
// omitted when zero.
std::string FaultText(FaultOp op, int32_t code);

enum class RadioMessageKind : uint8_t {
  kWindowStarted,  // the receiver is on; the report follows when the window ends
  kWindowReport,
  kFault,
  kFaultCleared,
  kConfigApplied,
};

// The channel plan is at most one entry per channel 1-14.
inline constexpr size_t kMaxPlanChannels = kMaxWifiChannel;

struct WindowStartedMessage {
  uint32_t window_id = 0;
  uint32_t config_generation = 0;
  Radio radio = Radio::kWifi;
  uint8_t channel = 0;  // 0 for BLE
  uint64_t rx_start_ms = 0;
  uint32_t planned_ms = 0;
};

struct FaultMessage {
  Radio radio = Radio::kWifi;
  FaultOp op = FaultOp::kStart;
  int32_t code = 0;  // driver-specific error code
};

// The effective configuration the radio task applied, as plain data.
struct ConfigAppliedMessage {
  uint32_t config_generation = 0;
  bool wifi_enabled = true;
  bool ble_enabled = true;
  uint8_t channel_count = 0;
  uint8_t channels[kMaxPlanChannels] = {};
  uint32_t wifi_dwell_ms = kDefaultWifiDwellMs;
  uint32_t ble_window_ms = kDefaultBleWindowMs;
};

// Tagged by `kind`; only the member for that kind is meaningful. A plain
// struct rather than a union so it stays trivially copyable and zero-initialised.
struct RadioMessage {
  RadioMessageKind kind = RadioMessageKind::kFaultCleared;
  Radio cleared_radio = Radio::kWifi;  // kFaultCleared
  WindowStartedMessage started;        // kWindowStarted
  WindowReport report;                 // kWindowReport
  FaultMessage fault;                  // kFault
  ConfigAppliedMessage config;         // kConfigApplied

  static RadioMessage WindowStarted(const WindowStartedMessage& started);
  static RadioMessage WindowReported(const WindowReport& report);
  static RadioMessage Fault(Radio radio, FaultOp op, int32_t code);
  static RadioMessage FaultCleared(Radio radio);
  static RadioMessage ConfigApplied(const ConfigAppliedMessage& config);
};

static_assert(std::is_trivially_copyable_v<RadioMessage>, "messages cross a task queue by value");

// kConfigApplied <-> ObservationConfig. ToConfigApplied normalises first, so
// the message always carries an effective plan of at most 14 channels.
ConfigAppliedMessage ToConfigApplied(uint32_t config_generation, const ObservationConfig& config);
ObservationConfig ToObservationConfig(const ConfigAppliedMessage& message);

}  // namespace firmware::domain
