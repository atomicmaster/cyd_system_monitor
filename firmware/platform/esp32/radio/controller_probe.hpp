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
  // Subset of wifi_management_frames that are specifically 802.11 beacons
  // (frame control type=0/subtype=8), not probe/assoc/deauth/etc. Exists
  // to check the channel-hop dwell (see ChannelHopTask) against real
  // beacon sightings, since the dwell is sized from the 802.11 beacon
  // period, not from generic management-frame traffic.
  uint32_t beacon_frames = 0;
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

  // F08 Work item 2: the BLE-side analog of the WiFi channel-hop probe
  // above (see BleScanToggleTask in controller_probe.cpp). BLE has no
  // per-channel select command comparable to esp_wifi_set_channel() --
  // the controller itself cycles the three primary advertising channels
  // (37/38/39) while a scan is enabled -- so the switching-gap question
  // on this side is the cost of stopping and restarting the scan itself,
  // as an explicit BLE Observation Window would need to (ADR 0003).
  uint32_t ble_scan_toggle_count = 0;
  uint32_t last_scan_disable_duration_us = 0;
  uint32_t last_scan_enable_duration_us = 0;
  uint32_t max_scan_toggle_duration_us = 0;
  uint64_t total_scan_toggle_duration_us = 0;
  // Advertising reports counted while the scan was deliberately disabled.
  // Expected to stay at zero -- any nonzero value means either a stray
  // report the controller queued before actually stopping, or a real
  // parser/ordering bug, and is worth flagging either way.
  uint32_t advertising_reports_while_disabled = 0;
};

ControllerProbeStatus ReadControllerProbeStatus();

}  // namespace firmware::platform::esp32::radio
