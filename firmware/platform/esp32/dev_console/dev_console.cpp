// SPDX-License-Identifier: Apache-2.0
//
// Development-only recovery path; TB02 replaces this with the negotiated
// product command. See dev_console.hpp. F07 additionally uses this console
// to trigger on-demand hardware exercises (LED, audio, peripheral status)
// during physical bring-up, since it is the only channel available before
// TB02's real product protocol exists.
#include "dev_console/dev_console.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "audio/audio.hpp"
#include "battery/battery.hpp"
#include "build_identity.hpp"
#include "button/button.hpp"
#include "capacity/capacity_probe.hpp"
#include "capacity/cpu_load.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "expansion/expansion.hpp"
#include "firmware/domain/generated/profile.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led/led.hpp"
#include "microsd/microsd.hpp"
#include "nvs/calibration_store.hpp"
#include "radio/controller_probe.hpp"

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

void RunControllerProbeStatus() {
  const auto status = firmware::platform::esp32::radio::ReadControllerProbeStatus();
  ESP_LOGI(kTag,
           "DEV:HCI_STATUS: enabled=%d scan_enabled=%d commands_sent=%lu command_failures=%lu "
           "advertising_reports=%lu wifi_management_frames=%lu malformed_events=%lu "
           "dropped_events=%lu",
           status.enabled, status.scan_enabled, static_cast<unsigned long>(status.commands_sent),
           static_cast<unsigned long>(status.command_failures),
           static_cast<unsigned long>(status.advertising_reports),
           static_cast<unsigned long>(status.wifi_management_frames),
           static_cast<unsigned long>(status.malformed_events),
           static_cast<unsigned long>(status.dropped_events));
}

// F08 Work item 2: on-demand summary of the WiFi channel-hop probe
// (ChannelHopTask in controller_probe.cpp) -- switch count and
// last/max/average switch duration, alongside the current channel. The
// per-switch trace (channel, duration, and running BLE/WiFi counters at
// that instant) is logged directly by the hop task itself as it happens;
// this command is only the cumulative summary, not a replacement for that
// trace.
void RunChannelStatus() {
  const auto status = firmware::platform::esp32::radio::ReadControllerProbeStatus();
  const uint64_t average_us = status.channel_switch_count > 0
                                  ? status.total_switch_duration_us / status.channel_switch_count
                                  : 0;
  ESP_LOGI(kTag,
           "DEV:CHANNEL_STATUS: current_channel=%u switch_count=%lu last_switch_duration_us=%lu "
           "max_switch_duration_us=%lu average_switch_duration_us=%llu beacon_frames_total=%lu",
           status.current_channel, static_cast<unsigned long>(status.channel_switch_count),
           static_cast<unsigned long>(status.last_switch_duration_us),
           static_cast<unsigned long>(status.max_switch_duration_us),
           static_cast<unsigned long long>(average_us),
           static_cast<unsigned long>(status.beacon_frames));
}

// F08 Work item 2: on-demand summary of the BLE-side scan toggle probe
// (BleScanToggleTask in controller_probe.cpp) -- the BLE counterpart to
// RunChannelStatus above. BLE has no per-channel switch, so this reports
// scan disable/enable round-trip timing and the cumulative count of
// advertising reports seen while the scan was deliberately off (expected
// to stay at zero).
void RunBleToggleStatus() {
  const auto status = firmware::platform::esp32::radio::ReadControllerProbeStatus();
  const uint64_t average_us =
      status.ble_scan_toggle_count > 0
          ? status.total_scan_toggle_duration_us / (2ULL * status.ble_scan_toggle_count)
          : 0;
  ESP_LOGI(kTag,
           "DEV:BLE_TOGGLE_STATUS: toggle_count=%lu last_disable_duration_us=%lu "
           "last_enable_duration_us=%lu max_toggle_duration_us=%lu "
           "average_toggle_duration_us=%llu advertising_reports_while_disabled=%lu",
           static_cast<unsigned long>(status.ble_scan_toggle_count),
           static_cast<unsigned long>(status.last_scan_disable_duration_us),
           static_cast<unsigned long>(status.last_scan_enable_duration_us),
           static_cast<unsigned long>(status.max_scan_toggle_duration_us),
           static_cast<unsigned long long>(average_us),
           static_cast<unsigned long>(status.advertising_reports_while_disabled));
}

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

struct Command {
  const char* line;
  void (*run)();
};

void RunClearCalibration() {
  ESP_LOGI(kTag, "received DEV:CLEAR_CALIBRATION: clearing calibration record");
  firmware::platform::esp32::nvs::ClearCalibrationRecord();
}

constexpr Command kCommands[] = {
    {"DEV:CLEAR_CALIBRATION", RunClearCalibration},
    {"DEV:LED_TEST", RunLedTest},
    {"DEV:AUDIO_TEST", RunAudioTest},
    {"DEV:PERIPHERAL_STATUS", RunPeripheralStatus},
    {"DEV:CAPACITY_STATUS", RunCapacityStatus},
    {"DEV:HCI_STATUS", RunControllerProbeStatus},
    {"DEV:CHANNEL_STATUS", RunChannelStatus},
    {"DEV:BLE_TOGGLE_STATUS", RunBleToggleStatus},
    {"DEV:CPU_STATUS", RunCpuStatus},
    {"DEV:NVS_WRITE_TEST", RunNvsWriteTest},
    {"DEV:SERIAL_RX_TEST", RunSerialRxTest},
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

void StartDevConsole() {
  TaskHandle_t handle = nullptr;
  xTaskCreate(DevConsoleTask, "dev_console", 4096, nullptr, tskIDLE_PRIORITY + 1, &handle);
  firmware::platform::esp32::capacity::RegisterTask("dev_console", handle);
}

}  // namespace firmware::platform::esp32::dev_console
