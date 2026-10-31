// SPDX-License-Identifier: Apache-2.0
//
// Development-only recovery path; TB02 replaces this with the negotiated
// product command. See dev_console.hpp. F07 additionally uses this console
// to trigger on-demand hardware exercises (LED, audio, peripheral status)
// during physical bring-up, since it is the only channel available before
// TB02's real product protocol exists.
#include "dev_console/dev_console.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "audio/audio.hpp"
#include "battery/battery.hpp"
#include "build_identity.hpp"
#include "button/button.hpp"
#include "capacity/capacity_probe.hpp"
#include "capacity/cpu_load.hpp"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "expansion/expansion.hpp"
#include "firmware/domain/coverage.hpp"
#include "firmware/domain/generated/profile.hpp"
#include "firmware/domain/radio_runner.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led/led.hpp"
#include "microsd/microsd.hpp"
#include "nvs/calibration_store.hpp"
#include "nvs/pairing_store.hpp"
#include "radio/radio_task.hpp"
#include "usb/host_serial.hpp"

namespace firmware::platform::esp32::dev_console {

namespace {

constexpr const char* kTag = "fw.dev_console";
constexpr size_t kLineBufferSize = 128;

// Cycles red, green, blue for 800ms each, then off. F07's acceptance line
// "LEDs ... are visible and reproducible" needs an actual driven sequence
// to observe -- board_init only configures the PWM channels, it never
// sets a color, so without this the LED never lights at all.
void RunLedTest() {
  ESP_LOGI(kTag, "DEV:LED_TEST: red");
  led::SetColor(255, 0, 0);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: green");
  led::SetColor(0, 255, 0);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: blue");
  led::SetColor(0, 0, 255);
  vTaskDelay(pdMS_TO_TICKS(800));
  ESP_LOGI(kTag, "DEV:LED_TEST: off");
  led::SetColor(0, 0, 0);
}

// Deliberately enables the amplifier and plays a short tone, then disables
// it again -- optional sound defaults off per the profile, so this is the
// only way to exercise it. Matches F07's Work item 2 ("exercise optional
// sound deliberately").
void RunAudioTest() {
  ESP_LOGI(kTag, "DEV:AUDIO_TEST: enabling amplifier, playing 1kHz tone for 500ms");
  audio::SetAmplifierEnabled(true);
  audio::PlayTestTone(1000, 500);
  audio::SetAmplifierEnabled(false);
  ESP_LOGI(kTag, "DEV:AUDIO_TEST: amplifier disabled");
}

// Re-probes MicroSD/battery/BOOT-button/expansion live (unlike
// board_init.cpp's once-at-boot probe) so an operator can insert/remove a
// card, press/release BOOT, or jumper the expansion pin between runs and
// see the result change -- the concrete meaning of F07's "visible and
// reproducible" for these.
void RunPeripheralStatus() {
  const auto sd = microsd::Probe();
  const int battery_mv = battery::ReadMillivolts();
  const bool button_pressed = button::IsPressed();
  const bool expansion_high = expansion::IsHigh();
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: microsd_present=%d microsd_detail=\"%s\"", sd.present,
           sd.detail.c_str());
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: battery_mv=%d", battery_mv);
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: boot_button_pressed=%d", button_pressed);
  ESP_LOGI(kTag, "DEV:PERIPHERAL_STATUS: expansion_high=%d", expansion_high);
}

// F08a Work item 1: surfaces LVGL pool peak/available, minimum free heap,
// and every registered task's stack low-water mark over UART, since this
// board has no other channel to report them before TB02's real product
// protocol exists. Re-probes live like RunPeripheralStatus, so an operator
// can request a fresh reading after exercising the capacity slice/radio
// load rather than only ever seeing a one-shot boot value.
void RunCapacityStatus() {
  const auto sample =
      firmware::platform::esp32::capacity::Sample(firmware::platform::esp32::kBuildIdentity);
  ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: build=%s", sample.build_identity.c_str());
  ESP_LOGI(kTag,
           "DEV:CAPACITY_STATUS: lvgl_pool_total_bytes=%lu lvgl_pool_peak_used_bytes=%lu "
           "lvgl_pool_available_bytes=%lu",
           static_cast<unsigned long>(sample.lvgl_pool_total_bytes),
           static_cast<unsigned long>(sample.lvgl_pool_peak_used_bytes),
           static_cast<unsigned long>(sample.lvgl_pool_available_bytes));
  ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: free_heap_bytes=%lu minimum_free_heap_bytes=%lu",
           static_cast<unsigned long>(sample.free_heap_bytes),
           static_cast<unsigned long>(sample.minimum_free_heap_bytes));
  for (const auto& task : sample.task_stacks) {
    ESP_LOGI(kTag, "DEV:CAPACITY_STATUS: task=%s stack_low_water_mark_bytes=%lu",
             task.task_name.c_str(), static_cast<unsigned long>(task.stack_low_water_mark_bytes));
  }
}

// F08 Work item 2: CPU load under whatever is running concurrently right
// now (radio probe, LVGL, touch polling). Blocks the dev-console task for
// one second while it samples, which is fine here since this is the only
// thing that task is doing when a command is being typed.
void RunCpuStatus() {
  constexpr uint32_t kSampleWindowMs = 1000;
  const auto samples = firmware::platform::esp32::capacity::SampleCpuLoad(kSampleWindowMs);
  if (samples.empty()) {
    ESP_LOGW(kTag, "DEV:CPU_STATUS: no samples (window too short or stats disabled)");
    return;
  }
  ESP_LOGI(kTag, "DEV:CPU_STATUS: window_ms=%lu", static_cast<unsigned long>(kSampleWindowMs));
  for (const auto& sample : samples) {
    ESP_LOGI(kTag, "DEV:CPU_STATUS: task=%s core=%d percent_x100=%lu", sample.task_name.c_str(),
             sample.core_id, static_cast<unsigned long>(sample.percent_x100));
  }
}

// F08 Work item 2/4: measures real NVS blob-write latency under whatever
// load is currently running, for the provisional write/endurance budget.
// Re-saves the board's own already-valid calibration record, unchanged,
// kWriteCount times -- never a synthetic record -- so this
// cannot desynchronize the stored checksum from a real calibration, and
// never runs at all if no valid record is present rather than writing
// placeholder coordinates a real calibration flow would then have to
// silently overwrite.
void RunNvsWriteTest() {
  // "landscape" duplicates main.cpp's kOrientation: this dev-only probe
  // reads the same board record main.cpp writes, and there is no shared
  // orientation constant to include instead (see main.cpp's own comment
  // on kOrientation).
  constexpr const char* kOrientation = "landscape";
  constexpr int kWriteCount = 20;

  firmware::domain::calibration::CalibrationRecord record;
  if (!firmware::platform::esp32::nvs::LoadCalibrationRecord(
          record, firmware::domain::profile::kProfileId, kOrientation,
          firmware::domain::calibration::kCalibrationSchemaVersion)) {
    ESP_LOGW(kTag,
             "DEV:NVS_WRITE_TEST: no valid stored calibration record -- skipping rather than "
             "writing a synthetic one");
    return;
  }

  int64_t min_us = INT64_MAX;
  int64_t max_us = 0;
  int64_t total_us = 0;
  int failures = 0;
  for (int i = 0; i < kWriteCount; ++i) {
    const int64_t start_us = esp_timer_get_time();
    const bool ok = firmware::platform::esp32::nvs::SaveCalibrationRecord(record);
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (!ok) {
      ++failures;
      continue;
    }
    min_us = std::min(min_us, elapsed_us);
    max_us = std::max(max_us, elapsed_us);
    total_us += elapsed_us;
  }
  const int successes = kWriteCount - failures;
  ESP_LOGI(kTag, "DEV:NVS_WRITE_TEST: writes=%d failures=%d min_us=%lld max_us=%lld avg_us=%lld",
           kWriteCount, failures, successes > 0 ? static_cast<long long>(min_us) : 0LL,
           static_cast<long long>(max_us),
           successes > 0 ? static_cast<long long>(total_us / successes) : 0LL);
}

// F08 Work item 4: raw esp_partition_write()/erase_range() timing against
// the "state" partition ADR 0017's future Alert/Baseline journal will
// live in -- distinct from RunNvsWriteTest above, which measures the
// ESP-IDF NVS library's own blob API against the separate "nvs"
// partition. Nothing reads "state" yet (the journal itself is M6 work),
// so this is free to erase and overwrite it; the sector is left erased
// (0xFF) afterward so no stale test pattern is mistaken for real data by
// whatever reads this partition first. kPartitionTestRecordBytes matches
// snapshot-size-estimate.md sibling work's provisional 128-byte Alert
// record size, not a coincidence -- this grounds that budget's per-write
// latency assumption in a measurement instead of an estimate.
void RunPartitionWriteTest() {
  constexpr size_t kRecordBytes = 128;
  constexpr int kWriteCount = 20;        // matches RunNvsWriteTest, for comparability
  constexpr size_t kSectorBytes = 4096;  // ESP32 SPI NOR flash erase-sector size
  static_assert(kRecordBytes * kWriteCount <= kSectorBytes,
                "test writes must fit the single erased sector this probe uses");

  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "state");
  if (partition == nullptr) {
    ESP_LOGW(kTag,
             "DEV:PARTITION_WRITE_TEST: \"state\" partition not found -- check partitions.csv");
    return;
  }

  const int64_t erase_start_us = esp_timer_get_time();
  const esp_err_t erase_err = esp_partition_erase_range(partition, 0, kSectorBytes);
  const int64_t erase_duration_us = esp_timer_get_time() - erase_start_us;
  if (erase_err != ESP_OK) {
    ESP_LOGW(kTag, "DEV:PARTITION_WRITE_TEST: sector erase failed: 0x%x", erase_err);
    return;
  }

  uint8_t record[kRecordBytes];
  std::memset(record, 0xA5, sizeof(record));

  int64_t min_us = INT64_MAX;
  int64_t max_us = 0;
  int64_t total_us = 0;
  int failures = 0;
  for (int i = 0; i < kWriteCount; ++i) {
    const size_t offset = static_cast<size_t>(i) * kRecordBytes;
    const int64_t start_us = esp_timer_get_time();
    const esp_err_t write_err = esp_partition_write(partition, offset, record, sizeof(record));
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (write_err != ESP_OK) {
      ++failures;
      continue;
    }
    min_us = std::min(min_us, elapsed_us);
    max_us = std::max(max_us, elapsed_us);
    total_us += elapsed_us;
  }
  const int successes = kWriteCount - failures;

  ESP_LOGI(kTag,
           "DEV:PARTITION_WRITE_TEST: sector_erase_us=%lld record_bytes=%zu writes=%d "
           "failures=%d min_write_us=%lld max_write_us=%lld avg_write_us=%lld",
           static_cast<long long>(erase_duration_us), kRecordBytes, kWriteCount, failures,
           successes > 0 ? static_cast<long long>(min_us) : 0LL, static_cast<long long>(max_us),
           successes > 0 ? static_cast<long long>(total_us / successes) : 0LL);

  esp_partition_erase_range(partition, 0, kSectorBytes);
}

const char* WindowEndName(firmware::domain::WindowEnd end) {
  switch (end) {
    case firmware::domain::WindowEnd::kCompleted:
      return "completed";
    case firmware::domain::WindowEnd::kConfigurationChanged:
      return "config-changed";
    case firmware::domain::WindowEnd::kDriverFault:
      return "driver-fault";
  }
  return "?";
}

// TB09: the radio subsystem's state in one place. The console task never
// touches app-loop state: the effective config, windows and ledger totals come
// from the copy the app loop publishes (radio::PublishAppView), and the counters
// from the radio side's own thread-safe reads.
void RunRadioStatus() {
  namespace domain = firmware::domain;
  const auto view = radio::ReadAppView();
  const auto status = radio::ReadStatus();

  ESP_LOGI(kTag, "DEV:RADIO_STATUS: started=%d wifi_init=%s(%ld) ble_init=%s(%ld)", status.started,
           status.wifi_init_ok ? "ok" : "FAILED", static_cast<long>(status.wifi_init_code),
           status.ble_init_ok ? "ok" : "FAILED", static_cast<long>(status.ble_init_code));

  char channels[domain::kMaxPlanChannels * 3 + 1] = "";
  size_t used = 0;
  for (uint8_t i = 0; i < view.config.channel_count && used + 4 < sizeof(channels); ++i) {
    used += static_cast<size_t>(std::snprintf(channels + used, sizeof(channels) - used, "%s%u",
                                              i == 0 ? "" : ",",
                                              static_cast<unsigned>(view.config.channels[i])));
  }
  ESP_LOGI(kTag,
           "DEV:RADIO_STATUS: config generation=%lu wifi=%s ble=%s wifi_dwell_ms=%lu "
           "ble_window_ms=%lu channels=[%s]",
           static_cast<unsigned long>(view.config.config_generation),
           view.config.wifi_enabled ? "on" : "off", view.config.ble_enabled ? "on" : "off",
           static_cast<unsigned long>(view.config.wifi_dwell_ms),
           static_cast<unsigned long>(view.config.ble_window_ms), channels);

  if (view.has_active) {
    ESP_LOGI(kTag,
             "DEV:RADIO_STATUS: active window id=%lu radio=%s channel=%u rx_start_ms=%llu "
             "planned_ms=%lu",
             static_cast<unsigned long>(view.active.window_id),
             view.active.radio == domain::Radio::kWifi ? "wifi" : "ble",
             static_cast<unsigned>(view.active.channel),
             static_cast<unsigned long long>(view.active.rx_start_ms),
             static_cast<unsigned long>(view.active.planned_ms));
  } else {
    ESP_LOGI(kTag, "DEV:RADIO_STATUS: active window none");
  }
  if (view.has_last) {
    ESP_LOGI(kTag,
             "DEV:RADIO_STATUS: last window id=%lu radio=%s channel=%u planned_ms=%lu "
             "observed_ms=%lu end=%s drops=%lu completed_ms=%llu generation=%lu",
             static_cast<unsigned long>(view.last.window_id),
             view.last.window.radio == domain::Radio::kWifi ? "wifi" : "ble",
             static_cast<unsigned>(view.last.window.channel),
             static_cast<unsigned long>(view.last.window.planned_ms),
             static_cast<unsigned long>(view.last.observed_ms), WindowEndName(view.last.end),
             static_cast<unsigned long>(view.last.dropped_events),
             static_cast<unsigned long long>(view.last.completed_ms),
             static_cast<unsigned long>(view.last.config_generation));
  } else {
    ESP_LOGI(kTag, "DEV:RADIO_STATUS: last window none");
  }

  const char* const names[2] = {"wifi", "ble"};
  for (size_t i = 0; i < 2; ++i) {
    const auto& totals = view.totals[i];
    ESP_LOGI(kTag,
             "DEV:RADIO_STATUS: %s totals windows=%lu planned_ms=%llu observed_ms=%llu "
             "dropped_events=%llu last_channel=%u fault=\"%s\"",
             names[i], static_cast<unsigned long>(totals.windows),
             static_cast<unsigned long long>(totals.planned_ms),
             static_cast<unsigned long long>(totals.observed_ms),
             static_cast<unsigned long long>(totals.dropped_events),
             static_cast<unsigned>(totals.last_channel), view.fault[i]);
  }

  ESP_LOGI(kTag,
           "DEV:RADIO_STATUS: events wifi=%lu ble=%lu outside_window wifi=%lu ble=%lu "
           "wrong_radio wifi=%lu ble=%lu",
           static_cast<unsigned long>(status.events_drained[0]),
           static_cast<unsigned long>(status.events_drained[1]),
           static_cast<unsigned long>(status.outside_window[0]),
           static_cast<unsigned long>(status.outside_window[1]),
           static_cast<unsigned long>(status.wrong_radio[0]),
           static_cast<unsigned long>(status.wrong_radio[1]));
  ESP_LOGI(kTag,
           "DEV:RADIO_STATUS: ring_drops=%llu unattributed_drops=%lu message_overflows=%lu "
           "control_overflows=%lu",
           static_cast<unsigned long long>(status.ring_drops),
           static_cast<unsigned long>(status.unattributed_drops),
           static_cast<unsigned long>(status.message_overflows),
           static_cast<unsigned long>(status.control_overflows));
  const auto& hci = status.hci;
  ESP_LOGI(kTag,
           "DEV:RADIO_STATUS: hci vhci_rx_overflow=%lu malformed=%lu commands_sent=%lu "
           "timeouts=%lu rejections=%lu late=%llu unexpected=%llu credit=%u hardware_errors=%lu "
           "controller_resets=%lu",
           static_cast<unsigned long>(hci.rx_overflow), static_cast<unsigned long>(hci.malformed),
           static_cast<unsigned long>(hci.commands_sent), static_cast<unsigned long>(hci.timeouts),
           static_cast<unsigned long>(hci.rejections), static_cast<unsigned long long>(hci.late),
           static_cast<unsigned long long>(hci.unexpected), static_cast<unsigned>(hci.credit),
           static_cast<unsigned long>(hci.hardware_errors),
           static_cast<unsigned long>(hci.controller_resets));
  ESP_LOGI(kTag, "DEV:RADIO_STATUS: stack_free_bytes radio=%lu hci_worker=%lu",
           static_cast<unsigned long>(status.radio_stack_free_bytes),
           static_cast<unsigned long>(hci.worker_stack_free_bytes));
}

// Each toggle is a ControlRequest to the radio task through its thread-safe
// queue, so the console never touches app-loop or scheduler state. The new
// budget shows up in DEV:RADIO_STATUS (and the status bar) with the next
// kConfigApplied.
void SendRadioToggle(const char* name, bool wifi, bool on) {
  firmware::domain::ControlRequest request;
  if (wifi) {
    request.wifi_enabled = on;
  } else {
    request.ble_enabled = on;
  }
  const bool queued = radio::RequestControl(request);
  ESP_LOGI(kTag, "%s: %s", name, queued ? "requested" : "NOT queued (radio stopped or queue full)");
}

void RunRadioWifiOn() { SendRadioToggle("DEV:RADIO_WIFI_ON", true, true); }
void RunRadioWifiOff() { SendRadioToggle("DEV:RADIO_WIFI_OFF", true, false); }
void RunRadioBleOn() { SendRadioToggle("DEV:RADIO_BLE_ON", false, true); }
void RunRadioBleOff() { SendRadioToggle("DEV:RADIO_BLE_OFF", false, false); }

// F08: measures raw UART throughput and frame loss against a host-side
// simulator sending representative-sized Host Metric snapshot frames
// (see hardware/profiles/lcdwiki-esp32-32e-2.8/development/
// serial_snapshot_probe.py and snapshot-size-estimate.md), ahead of C01's
// real protocol. Each frame is a newline-terminated "SEQ:<n>:<filler>"
// line; this counts total bytes, detects sequence gaps, and times
// inter-frame arrival -- not a CBOR decoder, since no wire format exists
// yet. Held in a static buffer, not the stack, because DevConsoleTask's
// stack is sized for short command handlers, not a snapshot-sized line.
constexpr size_t kSerialRxTestFrameCount = 60;
constexpr size_t kSerialRxTestMaxFrameBytes = 4096;
char g_serial_rx_test_frame[kSerialRxTestMaxFrameBytes];

void RunSerialRxTest() {
  ESP_LOGI(kTag, "DEV:SERIAL_RX_TEST: waiting for %zu frames (\"SEQ:<n>:<filler>\\n\")",
           kSerialRxTestFrameCount);

  uint32_t expected_seq = 0;
  bool have_expected = false;
  uint32_t sequence_gaps = 0;
  uint64_t bytes_received = 0;
  size_t frames_received = 0;
  size_t frame_len = 0;
  const int64_t start_us = esp_timer_get_time();
  int64_t last_frame_us = start_us;
  int64_t min_interval_us = INT64_MAX;
  int64_t max_interval_us = 0;
  int64_t total_interval_us = 0;

  while (frames_received < kSerialRxTestFrameCount) {
    const int ch = getchar();
    if (ch == EOF) {
      // At the default 100 Hz tick rate, pdMS_TO_TICKS(5) truncates to 0
      // ticks and never actually blocks, which starves IDLE1 and trips the
      // task watchdog under continuous traffic (observed live: reproduced
      // the watchdog reset every ~5 s for this task's whole duration). The
      // smallest real (1-tick, 10 ms) delay is deliberately used instead of
      // matching DevConsoleTask's 50 ms EOF delay: at 115200 baud this
      // console UART's polling VFS driver can receive several hundred bytes
      // in 50 ms, and blocking that long between polls dropped bytes and
      // corrupted frame boundaries in the first fix attempt (observed:
      // sequence_gaps, spurious near-0ms frame intervals, and leftover
      // stream bytes misparsed as console commands afterward).
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    ++bytes_received;
    const char byte = static_cast<char>(ch);

    if (byte != '\n' && byte != '\r') {
      if (frame_len < kSerialRxTestMaxFrameBytes - 1) {
        g_serial_rx_test_frame[frame_len++] = byte;
      } else {
        // Overlong frame: drop it and resync on the next newline, counting
        // it as a loss rather than growing an unbounded buffer.
        frame_len = 0;
        ++sequence_gaps;
      }
      continue;
    }
    if (frame_len == 0) {
      continue;  // stray CR/LF between frames
    }

    const int64_t now_us = esp_timer_get_time();
    if (frames_received > 0) {
      const int64_t interval_us = now_us - last_frame_us;
      min_interval_us = std::min(min_interval_us, interval_us);
      max_interval_us = std::max(max_interval_us, interval_us);
      total_interval_us += interval_us;
    }
    last_frame_us = now_us;

    // Parse the "SEQ:<digits>:" prefix; anything else can't be checked for
    // loss but still counts toward bytes/frames received.
    if (frame_len > 4 && std::strncmp(g_serial_rx_test_frame, "SEQ:", 4) == 0) {
      size_t i = 4;
      uint32_t seq = 0;
      bool any_digit = false;
      while (i < frame_len && g_serial_rx_test_frame[i] >= '0' &&
             g_serial_rx_test_frame[i] <= '9') {
        seq = seq * 10 + static_cast<uint32_t>(g_serial_rx_test_frame[i] - '0');
        ++i;
        any_digit = true;
      }
      if (any_digit && i < frame_len && g_serial_rx_test_frame[i] == ':') {
        if (have_expected && seq != expected_seq) {
          ++sequence_gaps;
        }
        expected_seq = seq + 1;
        have_expected = true;
      }
    }
    ++frames_received;
    frame_len = 0;
  }

  const int64_t total_duration_us = esp_timer_get_time() - start_us;
  const int64_t avg_interval_us =
      frames_received > 1 ? total_interval_us / static_cast<int64_t>(frames_received - 1) : 0;
  ESP_LOGI(kTag,
           "DEV:SERIAL_RX_STATUS: frames_received=%zu bytes_received=%llu sequence_gaps=%lu "
           "duration_ms=%lld avg_frame_interval_ms=%lld min_frame_interval_ms=%lld "
           "max_frame_interval_ms=%lld",
           frames_received, static_cast<unsigned long long>(bytes_received),
           static_cast<unsigned long>(sequence_gaps),
           static_cast<long long>(total_duration_us / 1000),
           static_cast<long long>(avg_interval_us / 1000),
           static_cast<long long>((min_interval_us == INT64_MAX ? 0 : min_interval_us) / 1000),
           static_cast<long long>(max_interval_us / 1000));
}

std::atomic<bool> g_cpu_replay_requested{false};

void RunCpuReplayRequest() {
  ESP_LOGI(kTag, "DEV:CPU_REPLAY: requested");
  g_cpu_replay_requested.store(true);
}

struct Command {
  const char* line;
  void (*run)();
};

void RunClearCalibration() {
  ESP_LOGI(kTag, "received DEV:CLEAR_CALIBRATION: clearing calibration record");
  firmware::platform::esp32::nvs::ClearCalibrationRecord();
}

// Recovery for a lost or broken paired Mac: forgets the stored Host so any
// Mac can pair again. The negotiated Unpair command is the normal path.
void RunUnpair() {
  ESP_LOGI(kTag, "received DEV:UNPAIR: forgetting the paired Host");
  firmware::platform::esp32::nvs::NvsPairingStore().ClearPairedHost();
}

constexpr Command kCommands[] = {
    {"DEV:UNPAIR", RunUnpair},
    {"DEV:CLEAR_CALIBRATION", RunClearCalibration},
    {"DEV:LED_TEST", RunLedTest},
    {"DEV:AUDIO_TEST", RunAudioTest},
    {"DEV:PERIPHERAL_STATUS", RunPeripheralStatus},
    {"DEV:CAPACITY_STATUS", RunCapacityStatus},
    {"DEV:RADIO_STATUS", RunRadioStatus},
    {"DEV:RADIO_WIFI_ON", RunRadioWifiOn},
    {"DEV:RADIO_WIFI_OFF", RunRadioWifiOff},
    {"DEV:RADIO_BLE_ON", RunRadioBleOn},
    {"DEV:RADIO_BLE_OFF", RunRadioBleOff},
    {"DEV:CPU_STATUS", RunCpuStatus},
    {"DEV:NVS_WRITE_TEST", RunNvsWriteTest},
    {"DEV:PARTITION_WRITE_TEST", RunPartitionWriteTest},
    {"DEV:SERIAL_RX_TEST", RunSerialRxTest},
    {"DEV:CPU_REPLAY", RunCpuReplayRequest},
    {"CYD:LINK", usb::EnterLinkMode},
};

// UART0 is already owned by ESP-IDF's console/log VFS layer (ESP_LOGI,
// idf.py monitor): it reads/writes UART0 through esp_vfs_console's own
// polling driver, which never calls uart_driver_install(). The separate
// ring-buffer driver/uart.h API (uart_read_bytes()) requires that install
// call to have happened -- calling it here without one fails instantly on
// every invocation instead of blocking, spinning this task at full speed
// and starving the idle task's watchdog reset (observed as a task_wdt trip
// on IDLE1 with dev_console on the backtrace). Reading through stdin
// instead uses the same already-installed console VFS driver UART0 is
// wired to, so it blocks for real and doesn't fight over ownership of the
// port.
void DevConsoleTask(void* /*arg*/) {
  char line[kLineBufferSize];
  size_t line_len = 0;

  for (;;) {
    const int ch = getchar();
    if (usb::LinkModeActive()) {
      // DEV: commands are NOT parsed once CYD:LINK has activated (until the next
      // reset): from then on the UART carries only protocol bytes, so
      // DEV:RADIO_STATUS and the other DEV: lines are unavailable on a linked
      // board. Negotiated link: every byte is protocol data. Poll at the smallest
      // real delay so the UART FIFO never overruns (see RunSerialRxTest).
      if (ch == EOF) {
        vTaskDelay(pdMS_TO_TICKS(10));
      } else {
        usb::OnRxByte(static_cast<uint8_t>(ch));
      }
      continue;
    }
    if (ch == EOF) {
      // No console input driver configured, or a transient read error:
      // don't spin.
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const char byte = static_cast<char>(ch);

    if (byte == '\n' || byte == '\r') {
      if (line_len > 0) {
        line[line_len] = '\0';
        bool matched = false;
        for (const Command& command : kCommands) {
          if (std::strcmp(line, command.line) == 0) {
            command.run();
            matched = true;
            break;
          }
        }
        if (!matched) {
          ESP_LOGW(kTag, "unrecognized dev console line: \"%s\"", line);
        }
        line_len = 0;
      }
      continue;
    }

    if (line_len < kLineBufferSize - 1) {
      line[line_len++] = byte;
    } else {
      // Overlong line: not a recognized command either way; drop it and
      // resync on the next newline rather than growing an unbounded buffer.
      line_len = 0;
    }
  }
}

}  // namespace

bool ConsumeCpuReplayRequest() { return g_cpu_replay_requested.exchange(false); }

void StartDevConsole() {
  TaskHandle_t handle = nullptr;
  xTaskCreate(DevConsoleTask, "dev_console", 4096, nullptr, tskIDLE_PRIORITY + 1, &handle);
  firmware::platform::esp32::capacity::RegisterTask("dev_console", handle);
}

}  // namespace firmware::platform::esp32::dev_console
