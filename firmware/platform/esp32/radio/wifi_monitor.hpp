// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "firmware/domain/radio_runner.hpp"
#include "observation_gate.hpp"

namespace firmware::platform::esp32::radio {

// Passive 802.11 reception: WIFI_MODE_NULL with promiscuous mode and a
// management-frame filter. It never transmits (ADR 0001): no station or AP
// mode, no connect, no scan API, no esp_wifi_80211_tx. The promiscuous callback
// builds a provisional ObservationEvent (docs/extension-interfaces.md) and
// offers it to the gate; no frame payload is kept.
class WifiMonitor {
 public:
  static constexpr int32_t kCodeChannelMismatchBase = 1000;

  explicit WifiMonitor(ObservationGate& gate) : gate_(gate) {}

  // Brings the WiFi stack up in promiscuous-capable NULL mode, receive off. The
  // result is kept: a failed init is reported by Start() as FaultOp::kInit.
  domain::DriverResult Init();

  // Enables promiscuous mode, selects the channel and reads it back. A channel
  // that differs from the request is a kSetChannel fault (code kCodeChannelMismatchBase +
  // the channel read). Frames are only kept once the gate's window is open, which
  // is after this returns.
  domain::DriverResult Start(uint8_t channel);
  domain::DriverResult Stop();

  // The promiscuous callback body; `type` is a wifi_promiscuous_pkt_type_t.
  static void OnPacket(void* buffer, int type);

 private:
  ObservationGate& gate_;
  domain::DriverResult init_result_{false, domain::FaultOp::kInit, 0};
};

}  // namespace firmware::platform::esp32::radio
