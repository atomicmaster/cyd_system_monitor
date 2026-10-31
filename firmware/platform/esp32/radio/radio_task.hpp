// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "firmware/domain/coverage.hpp"
#include "firmware/domain/radio_messages.hpp"
#include "firmware/domain/radio_runner.hpp"
#include "hci_transport.hpp"

namespace firmware::platform::esp32::radio {

// Starts the radio subsystem: the radio task (core 1) that runs
// domain::RadioRunner::Step() in a loop, and the HCI worker. Returns once the
// tasks exist; WiFi and BLE bring-up happen on the radio task, so boot never
// waits for them and nothing here depends on calibration or Host state. A
// radio that fails to initialise is reported as a fault and the other keeps
// working. Returns false only when the task or its queues could not be created.
bool StartRadio();

// Sends an operator change to the radio task. Thread-safe (a queue plus a task
// notification); never blocks. False when the radio is not running or the
// queue is full.
bool RequestControl(const domain::ControlRequest& request);

// App loop side: the next message from the radio task, without blocking.
bool ReceiveMessage(domain::RadioMessage& out);

// Counters from the radio side, for DEV:RADIO_STATUS.
struct RadioStatus {
  bool started = false;
  bool wifi_init_ok = false;
  bool ble_init_ok = false;
  int32_t wifi_init_code = 0;
  int32_t ble_init_code = 0;
  uint32_t events_drained[2] = {};     // indexed by domain::EventProtocol
  uint32_t outside_window[2] = {};     // arrivals with no matching open window
  uint32_t wrong_radio[2] = {};        // subset: a window of the other radio was open
  uint32_t scan_stop_already_off = 0;  // scan disables answered 0x0C (already off)
  uint64_t ring_drops = 0;             // events evicted from the rings, attributed or not
  uint32_t unattributed_drops = 0;     // of those, drops that no window could claim
  uint32_t message_overflows = 0;      // RadioMessages the app loop had no room for
  uint32_t control_overflows = 0;      // ControlRequests refused for lack of room
  HciStats hci;
  uint32_t radio_stack_free_bytes = 0;
};
RadioStatus ReadStatus();

// The app loop's copy of what the radio task reported, published so the dev
// console (another task) can print it without touching app-loop state.
struct AppView {
  domain::ConfigAppliedMessage config;  // generation 0 until the first kConfigApplied
  bool has_active = false;
  domain::ActiveWindow active;
  bool has_last = false;
  domain::WindowReport last;
  domain::RadioTotals totals[2];  // indexed by domain::Radio
  char fault[2][48] = {};         // CoverageLedger fault reasons; empty when none
};
void PublishAppView(const AppView& view);
AppView ReadAppView();

}  // namespace firmware::platform::esp32::radio
