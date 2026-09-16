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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace firmware::platform::esp32::radio {

namespace {

constexpr const char* kTag = "fw.radio_probe";
constexpr uint8_t kWifiMonitorChannel = 1;
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

void OnWifiPacket(void*, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MGMT) {
    ++g_status.wifi_management_frames;
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
