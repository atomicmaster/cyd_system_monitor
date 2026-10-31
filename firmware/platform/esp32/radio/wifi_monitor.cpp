// SPDX-License-Identifier: Apache-2.0
#include "wifi_monitor.hpp"

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace firmware::platform::esp32::radio {

namespace {

using domain::DriverResult;
using domain::FaultOp;

WifiMonitor* g_monitor = nullptr;

constexpr size_t kAddr1Offset = 4;
constexpr size_t kAddr2Offset = 10;
constexpr size_t kAddr3Offset = 16;
constexpr size_t kMacBytes = 6;
constexpr uint8_t kRetryBit = 0x08;  // frame control byte 1

// The promiscuous callback has no context argument, so it reaches the monitor
// through a file-scope pointer set by Init.
void PacketCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  WifiMonitor::OnPacket(buffer, static_cast<int>(type));
}

void CopyMac(const uint8_t* payload, size_t length, size_t offset, domain::MacAddress& out) {
  if (offset + kMacBytes <= length) std::memcpy(out.data(), payload + offset, kMacBytes);
}

}  // namespace

void WifiMonitor::OnPacket(void* buffer, int type) {
  if (g_monitor == nullptr || type != WIFI_PKT_MGMT || buffer == nullptr) return;
  const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
  const size_t length = packet->rx_ctrl.sig_len;

  domain::ObservationEvent event;
  event.protocol = domain::EventProtocol::kWifi;
  event.channel = static_cast<uint8_t>(packet->rx_ctrl.channel);
  event.monotonic_us = static_cast<uint64_t>(esp_timer_get_time());
  event.rssi = static_cast<int8_t>(packet->rx_ctrl.rssi);
  event.metadata_kind = domain::EventMetadataKind::kWifi;
  domain::WifiEventMetadata& wifi = event.wifi;
  wifi.length = static_cast<uint16_t>(length);
  // The 802.11 header is in the payload; reads are bounded by the frame length.
  if (length >= 2) {
    const uint8_t* payload = packet->payload;
    wifi.frame_type = (payload[0] >> 2) & 0x3;
    wifi.frame_subtype = payload[0] >> 4;
    if (payload[1] & kRetryBit) wifi.flags |= domain::kWifiFlagRetry;
    CopyMac(payload, length, kAddr2Offset, wifi.transmitter);
    CopyMac(payload, length, kAddr3Offset, wifi.bssid);
  }
  if (packet->rx_ctrl.rx_state == 0) wifi.flags |= domain::kWifiFlagFcsOk;
  g_monitor->gate_.Offer(event);
}

DriverResult WifiMonitor::Init() {
  g_monitor = this;
  // The driver logs two INFO lines per window (ic_enable_sniffer and
  // ic_disable_sniffer), about five lines a second. Our own fw.* tags are untouched.
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_err_t err = esp_netif_init();
  if (err == ESP_OK) {
    err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;  // someone else already made it
  }
  if (err == ESP_OK) {
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
  }
  if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_NULL);
  if (err == ESP_OK) err = esp_wifi_start();
  if (err == ESP_OK) {
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    err = esp_wifi_set_promiscuous_filter(&filter);
  }
  if (err == ESP_OK) {
    err = esp_wifi_set_promiscuous_rx_cb(PacketCallback);
  }
  init_result_ = err == ESP_OK ? DriverResult{} : DriverResult{false, FaultOp::kInit, err};
  return init_result_;
}

DriverResult WifiMonitor::Start(uint8_t channel) {
  if (!init_result_.ok) return init_result_;
  // Receive is gated by the observation window, which opens after this returns,
  // so frames heard while the channel settles are counted outside-window.
  esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) return {false, FaultOp::kStart, err};
  err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) return {false, FaultOp::kSetChannel, err};
  uint8_t primary = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  err = esp_wifi_get_channel(&primary, &secondary);
  if (err != ESP_OK) return {false, FaultOp::kSetChannel, err};
  if (primary != channel) return {false, FaultOp::kSetChannel, kCodeChannelMismatchBase + primary};
  return {};
}

DriverResult WifiMonitor::Stop() {
  if (!init_result_.ok) return {};
  const esp_err_t err = esp_wifi_set_promiscuous(false);
  if (err != ESP_OK) return {false, FaultOp::kStop, err};
  return {};
}

}  // namespace firmware::platform::esp32::radio
