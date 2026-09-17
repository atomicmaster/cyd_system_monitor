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

  // F08 Work item 2: bounded WiFi channel-hop feasibility measurement (see
  // ChannelHopTask in controller_probe.cpp), not the product's eventual
  // WiFi Channel Plan scheduler (ADR 0026) -- this always cycles the same
  // fixed 3-channel probe set on a fixed dwell, with no BLE/WiFi
  // arbitration beyond what ESP-IDF's coexistence layer already does.
  uint8_t current_channel = 0;
  uint32_t channel_switch_count = 0;
  uint32_t last_switch_duration_us = 0;
  uint32_t max_switch_duration_us = 0;
  uint64_t total_switch_duration_us = 0;
};

ControllerProbeStatus ReadControllerProbeStatus();

}  // namespace firmware::platform::esp32::radio
