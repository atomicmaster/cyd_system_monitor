// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace firmware::domain {

// PROVISIONAL envelope for one observed frame or advertisement. TB10 (WiFi)
// and TB11 (BLE) own the final protocol metadata; this fixes only what the
// radio path and the event ring need now: which window an event belongs to, so
// drops can be attributed, and enough header fields to move through a queue.
// No payload is retained (docs/radio-model.md); addresses are raw over-the-air
// values. Fixed-size and trivially copyable.

enum class EventProtocol : uint8_t { kWifi, kBle };

// Which metadata member is valid; kNone means the decoder produced none.
enum class EventMetadataKind : uint8_t { kNone, kWifi, kBle };

using MacAddress = std::array<uint8_t, 6>;

inline constexpr uint8_t kWifiFlagRetry = 1u << 0;
inline constexpr uint8_t kWifiFlagFcsOk = 1u << 1;

struct WifiEventMetadata {
  uint8_t frame_type = 0;     // 802.11 frame control type
  uint8_t frame_subtype = 0;  // 802.11 frame control subtype
  MacAddress transmitter{};
  MacAddress bssid{};
  uint16_t length = 0;  // over-the-air frame length
  uint8_t flags = 0;    // kWifiFlag*
};

struct BleEventMetadata {
  uint8_t adv_type = 0;
  uint8_t addr_type = 0;
  MacAddress addr{};
  uint8_t data_len = 0;  // advertising data length; the data itself is not kept
};

struct ObservationEvent {
  EventProtocol protocol = EventProtocol::kWifi;
  uint8_t channel = 0;  // 0 for BLE
  uint32_t window_id = 0;
  uint32_t config_generation = 0;
  uint64_t monotonic_us = 0;
  int8_t rssi = 0;
  EventMetadataKind metadata_kind = EventMetadataKind::kNone;
  WifiEventMetadata wifi;  // valid when metadata_kind == kWifi
  BleEventMetadata ble;    // valid when metadata_kind == kBle
};

static_assert(std::is_trivially_copyable_v<ObservationEvent>,
              "events move through a ring by value");

}  // namespace firmware::domain
