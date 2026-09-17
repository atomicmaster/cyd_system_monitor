// SPDX-License-Identifier: Apache-2.0
#include "radio/controller_probe.hpp"

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_CONTROLLER_ONLY

#include <algorithm>
#include <array>

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace firmware::platform::esp32::radio {

namespace {

constexpr const char* kTag = "fw.radio_probe";
constexpr uint8_t kWifiMonitorChannel = 1;

// F08 Work item 2: the full US 2.4 GHz WiFi channel range (1-11, FCC Part
// 15) -- not ADR 0026's eventual per-region Channel Plan, which does not
// exist yet, but the first region-sized set this probe covers, after the
// earlier 3-channel (1/6/11) non-overlapping subset. This measures
// whether explicit channel switching is affordable, whether revisit
// timing over a realistic full plan is acceptable, and whether the
// ESP32's single radio visibly interrupts BLE reception while WiFi
// switches -- it is not a scheduling policy.
constexpr uint8_t kChannelPlan[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr size_t kChannelPlanSize = sizeof(kChannelPlan) / sizeof(kChannelPlan[0]);

// Dwell is sized against beacon timing, not an arbitrary round number: the
// 802.11 default beacon period is 100 TU = 102,400 us (dot11BeaconPeriod),
// and the overwhelming majority of consumer APs use exactly that default.
// A dwell shorter than one beacon period can miss every beacon from an AP
// on that channel depending on phase alignment alone, independent of RF
// conditions -- that would be a probe artifact, not a real coverage gap.
// Two beacon periods (~205 ms) is the minimum dwell, matching Kismet's own
// channel-hopping floor for the same reason: one period risks landing
// exactly on the gap between two beacons if phase is unlucky, while two
// periods guarantees a beacon falls inside the window regardless of phase.
// This is deliberately the floor, not a padded-out margin -- see the
// beacon-frame counter below, which exists to check this against reality
// rather than assume a bigger number is automatically safer.
constexpr uint32_t kChannelDwellMs = 205;

constexpr size_t kHciPacketBytes = 260;
// Four complete H4 events are enough to separate the VHCI callback from the
// parser while keeping this feasibility probe's static reservation explicit.
// A full queue is counted as an observation loss rather than extended.
constexpr size_t kHciEventQueueDepth = 4;

struct HciPacket {
  uint16_t length = 0;
  std::array<uint8_t, kHciPacketBytes> bytes{};
};

StaticQueue_t g_event_queue_storage;
std::array<uint8_t, kHciEventQueueDepth * sizeof(HciPacket)> g_event_queue_bytes{};
QueueHandle_t g_event_queue = nullptr;
ControllerProbeStatus g_status;

bool Send(const uint8_t* bytes, uint16_t length) {
  if (!esp_vhci_host_check_send_available()) {
    return false;
  }
  esp_vhci_host_send_packet(const_cast<uint8_t*>(bytes), length);
  ++g_status.commands_sent;
  return true;
}

void WaitAndSend(const uint8_t* bytes, uint16_t length) {
  while (!Send(bytes, length)) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // The next command is only issued after the controller has had a chance to
  // return Command Complete/Status through the same bounded queue.
  vTaskDelay(pdMS_TO_TICKS(30));
}

int OnHciPacket(uint8_t* bytes, uint16_t length) {
  if (length > kHciPacketBytes || g_event_queue == nullptr) {
    ++g_status.dropped_events;
    return ESP_FAIL;
  }
  HciPacket packet;
  packet.length = length;
  std::copy_n(bytes, length, packet.bytes.begin());
  if (xQueueSend(g_event_queue, &packet, 0) != pdTRUE) {
    ++g_status.dropped_events;
  }
  return ESP_OK;
}

void OnControllerSendAvailable() {}

const esp_vhci_host_callback_t kVhciCallbacks = {
    .notify_host_send_available = OnControllerSendAvailable,
    .notify_host_recv = OnHciPacket,
};

void ParseEvent(const HciPacket& packet) {
  if (packet.length < 3 || packet.bytes[0] != 0x04) {  // H4 event packet
    ++g_status.malformed_events;
    return;
  }
  const uint8_t event_code = packet.bytes[1];
  const uint8_t parameter_length = packet.bytes[2];
  if (packet.length != static_cast<uint16_t>(parameter_length) + 3U) {
    ++g_status.malformed_events;
    return;
  }
  if (event_code == 0x0e && packet.length >= 7 && packet.bytes[6] != 0) {
    ++g_status.command_failures;
    ESP_LOGW(kTag, "HCI command failed: opcode=0x%02x%02x status=0x%02x", packet.bytes[5],
             packet.bytes[4], packet.bytes[6]);
    return;
  }
  // LE Meta Event / LE Advertising Report. Do not retain payloads: each
  // report is evidence only and contributes one bounded counter.
  if (event_code == 0x3e && packet.length >= 5 && packet.bytes[3] == 0x02) {
    const uint8_t report_count = packet.bytes[4];
    size_t offset = 5;
    for (uint8_t report = 0; report < report_count; ++report) {
      // event type + address type + 6-byte address + data length + RSSI
      if (offset + 9 > packet.length) {
        ++g_status.malformed_events;
        return;
      }
      const uint8_t data_length = packet.bytes[offset + 8];
      if (offset + 10U + data_length > packet.length) {
        ++g_status.malformed_events;
        return;
      }
      ++g_status.advertising_reports;
      offset += 10U + data_length;
    }
  }
}

void EventTask(void*) {
  HciPacket packet;
  for (;;) {
    if (xQueueReceive(g_event_queue, &packet, portMAX_DELAY) == pdTRUE) {
      ParseEvent(packet);
    }
  }
}

// Distinguishes beacons from the rest of WIFI_PKT_MGMT (probe req/resp,
// (dis)association, deauth, ...) by reading the 802.11 frame control
// field's type/subtype directly out of the first payload byte -- beacon
// is type=0 (management), subtype=8. This exists so the channel-hop
// dwell (kChannelDwellMs) can be checked against real beacon sightings
// instead of assumed correct from the 802.11 default alone.
bool IsBeaconFrame(const wifi_promiscuous_pkt_t& packet) {
  if (packet.rx_ctrl.sig_len < 1) {
    return false;
  }
  constexpr uint8_t kManagementFrameType = 0;
  constexpr uint8_t kBeaconSubtype = 8;
  const uint8_t frame_control_byte0 = packet.payload[0];
  const uint8_t frame_type = (frame_control_byte0 >> 2) & 0x3;
  const uint8_t frame_subtype = (frame_control_byte0 >> 4) & 0xF;
  return frame_type == kManagementFrameType && frame_subtype == kBeaconSubtype;
}

void OnWifiPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) {
    return;
  }
  ++g_status.wifi_management_frames;
  if (IsBeaconFrame(*static_cast<const wifi_promiscuous_pkt_t*>(buf))) {
    ++g_status.beacon_frames;
  }
}

void StartWifi() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  ESP_ERROR_CHECK(esp_wifi_start());
  wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(OnWifiPacket));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_wifi_set_channel(kWifiMonitorChannel, WIFI_SECOND_CHAN_NONE));
  g_status.current_channel = kWifiMonitorChannel;
}

// Cycles kChannelPlan on a fixed kChannelDwellMs dwell, timing each
// esp_wifi_set_channel() call and logging the running BLE/WiFi counters
// alongside it -- an operator diffing consecutive log lines can see
// directly whether a WiFi channel switch visibly stalls BLE advertising
// reception (ADR 0003's "single RF module time-shares WiFi and
// Bluetooth"), not just whether the switch call itself is cheap.
void ChannelHopTask(void*) {
  size_t index = 0;
  uint32_t beacon_frames_at_dwell_start = g_status.beacon_frames;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kChannelDwellMs));

    // Attribute this dwell's beacon sightings to the channel that was just
    // active, before switching away from it -- this is the number
    // kChannelDwellMs's beacon-period margin is meant to keep above zero
    // whenever an AP is actually present on that channel.
    const uint8_t dwelled_channel = g_status.current_channel;
    const uint32_t beacons_this_dwell = g_status.beacon_frames - beacon_frames_at_dwell_start;

    const uint8_t next_channel = kChannelPlan[index];
    index = (index + 1) % kChannelPlanSize;

    const int64_t start_us = esp_timer_get_time();
    const esp_err_t err = esp_wifi_set_channel(next_channel, WIFI_SECOND_CHAN_NONE);
    const auto duration_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "channel switch failed: channel=%u err=0x%x", next_channel, err);
      continue;
    }

    g_status.current_channel = next_channel;
    ++g_status.channel_switch_count;
    g_status.last_switch_duration_us = duration_us;
    g_status.max_switch_duration_us = std::max(g_status.max_switch_duration_us, duration_us);
    g_status.total_switch_duration_us += duration_us;
    beacon_frames_at_dwell_start = g_status.beacon_frames;
    ESP_LOGI(kTag,
             "channel switch: dwelled_channel=%u beacons_this_dwell=%lu next_channel=%u "
             "duration_us=%lu advertising_reports=%lu wifi_management_frames=%lu",
             dwelled_channel, static_cast<unsigned long>(beacons_this_dwell), next_channel,
             static_cast<unsigned long>(duration_us),
             static_cast<unsigned long>(g_status.advertising_reports),
             static_cast<unsigned long>(g_status.wifi_management_frames));
  }
}

void StartBle() {
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
  esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_init(&config));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
  g_event_queue = xQueueCreateStatic(kHciEventQueueDepth, sizeof(HciPacket),
                                     g_event_queue_bytes.data(), &g_event_queue_storage);
  ESP_ERROR_CHECK(g_event_queue == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
  ESP_ERROR_CHECK(esp_vhci_host_register_callback(&kVhciCallbacks));
  xTaskCreatePinnedToCore(EventTask, "hci_parser", 2048, nullptr, 5, nullptr, 1);

  constexpr uint8_t kReset[] = {0x01, 0x03, 0x0c, 0x00};
  constexpr uint8_t kEventMask[] = {0x01, 0x01, 0x0c, 0x08, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x20};
  constexpr uint8_t kLeEventMask[] = {0x01, 0x01, 0x20, 0x08, 0x02, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // LE Set Scan Parameters: passive, 50ms interval, 30ms window, public address, accept all.
  constexpr uint8_t kPassiveScanParameters[] = {0x01, 0x0b, 0x20, 0x07, 0x00, 0x50,
                                                0x00, 0x30, 0x00, 0x00, 0x00};
  constexpr uint8_t kScanEnable[] = {0x01, 0x0c, 0x20, 0x02, 0x01, 0x00};
  WaitAndSend(kReset, sizeof(kReset));
  WaitAndSend(kEventMask, sizeof(kEventMask));
  WaitAndSend(kLeEventMask, sizeof(kLeEventMask));
  WaitAndSend(kPassiveScanParameters, sizeof(kPassiveScanParameters));
  WaitAndSend(kScanEnable, sizeof(kScanEnable));
  g_status.scan_enabled = true;
}

}  // namespace

void StartControllerOnlyProbe() {
  if (g_status.enabled) {
    return;
  }
  StartWifi();
  StartBle();
  // Pinned to core 1 alongside the wifi and hci_parser tasks: a channel
  // switch and BLE event parsing contending for the same core is exactly
  // the interference this probe wants to observe, not something to
  // engineer away by isolating it on core 0 with the UI.
  xTaskCreatePinnedToCore(ChannelHopTask, "channel_hop", 2048, nullptr, 4, nullptr, 1);
  g_status.enabled = true;
  ESP_LOGI(kTag, "controller-only passive BLE scan and Wi-Fi monitor enabled");
}

ControllerProbeStatus ReadControllerProbeStatus() { return g_status; }

}  // namespace firmware::platform::esp32::radio

#else

namespace firmware::platform::esp32::radio {

void StartControllerOnlyProbe() {}

ControllerProbeStatus ReadControllerProbeStatus() { return {}; }

}  // namespace firmware::platform::esp32::radio

#endif
