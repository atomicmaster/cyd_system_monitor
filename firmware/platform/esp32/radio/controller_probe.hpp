// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace firmware::platform::esp32::radio {

// F08-only experiment: starts Wi-Fi plus a controller-only, passive BLE
// scanner. It deliberately exposes HCI-level counters, not product radio
// semantics. In normal diagnostic builds this is a harmless disabled stub.
void StartControllerOnlyProbe();

struct ControllerProbeStatus {
  bool enabled = false;
  bool scan_enabled = false;
  uint32_t commands_sent = 0;
  uint32_t command_failures = 0;
  uint32_t advertising_reports = 0;
  uint32_t wifi_management_frames = 0;
  uint32_t malformed_events = 0;
  uint32_t dropped_events = 0;
};

ControllerProbeStatus ReadControllerProbeStatus();

}  // namespace firmware::platform::esp32::radio
