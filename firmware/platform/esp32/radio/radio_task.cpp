// SPDX-License-Identifier: Apache-2.0
#include "radio_task.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <optional>

#include "capacity/capacity_probe.hpp"
#include "esp_log.h"
#include "firmware/domain/radio_messages.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio_driver.hpp"

namespace firmware::platform::esp32::radio {

namespace {

constexpr const char* kTag = "fw.radio";

// About 4 KiB of loop; the extra kilobyte covers the one-off WiFi/BLE bring-up
// on this stack, whose depth is not measured yet (DEV:RADIO_STATUS reports the
// high-water mark).
constexpr uint32_t kRadioTaskStackBytes = 5120;
constexpr UBaseType_t kRadioTaskPriority = 4;  // below the HCI worker (5)
constexpr BaseType_t kRadioTaskCore = 1;

// Stall budget: a window pair makes at most ~10 messages a second (a started and
// a report per ~205 ms window, plus rare faults), so 32 messages ride out an
// app-loop stall of about 3 s (a flash write or screen rebuild is far below
// that). Past it, messages are counted and a report-overflow fault follows.
constexpr size_t kMessageQueueDepth = 32;
constexpr size_t kControlQueueDepth = 8;
// Clean report sends before a report-overflow fault is withdrawn.
constexpr uint32_t kOverflowClearSends = 8;

QueueHandle_t g_messages = nullptr;
QueueHandle_t g_controls = nullptr;
TaskHandle_t g_task = nullptr;
Esp32RadioDriver* g_driver = nullptr;
std::atomic<uint32_t> g_message_overflows{0};
std::atomic<uint32_t> g_control_overflows{0};

portMUX_TYPE g_view_mux = portMUX_INITIALIZER_UNLOCKED;
AppView g_view;

size_t Index(domain::Radio radio) { return radio == domain::Radio::kWifi ? 0 : 1; }

class QueueControlSource final : public domain::ControlSource {
 public:
  std::optional<domain::ControlRequest> Poll() override {
    domain::ControlRequest request;
    if (xQueueReceive(g_controls, &request, 0) != pdTRUE) return std::nullopt;
    // Several requests can share one notification; keep the wake alive while
    // more are queued so the runner polls again instead of sleeping on them.
    if (uxQueueMessagesWaiting(g_controls) > 0) {
      xTaskNotify(g_task, Esp32RadioDriver::kControlNotifyBit, eSetBits);
    }
    return request;
  }
};

// RadioMessages to the app loop. A full queue never blocks the radio task: the
// loss is counted and made up when room returns. What is replayed is state, not
// history: the newest config and per-radio fault state are resent, and a
// report-overflow fault names each radio that lost a window message (withdrawn
// after a run of clean sends). The radio task is its only caller.
class QueueRadioSink final : public domain::RadioSink {
 public:
  void Emit(const domain::RadioMessage& message) override {
    Observe(message);
    Flush();
    if (Send(message)) {
      if (message.kind == domain::RadioMessageKind::kWindowReport) {
        ++clean_sends_[Index(message.report.window.radio)];
      }
    } else {
      Lost(message);
    }
  }

  // Resends what was lost, oldest state first, while the queue has room.
  void Flush() {
    if (config_pending_) {
      if (!Send(config_)) return;
      config_pending_ = false;
    }
    for (size_t i = 0; i < 2; ++i) {
      if (fault_pending_[i]) {
        if (!Send(fault_[i])) return;
        fault_pending_[i] = false;
      }
    }
    for (size_t i = 0; i < 2; ++i) {
      if (lost_[i] == 0) continue;
      const auto radio = i == 0 ? domain::Radio::kWifi : domain::Radio::kBle;
      const auto code =
          static_cast<int32_t>(std::min<uint32_t>(lost_[i], std::numeric_limits<int32_t>::max()));
      if (!Send(domain::RadioMessage::Fault(radio, domain::FaultOp::kReportQueueOverflow, code))) {
        return;
      }
      lost_[i] = 0;
      clean_sends_[i] = 0;
      overflow_fault_[i] = true;
    }
    for (size_t i = 0; i < 2; ++i) {
      if (!overflow_fault_[i] || clean_sends_[i] < kOverflowClearSends) continue;
      // Put back whatever the runner last said about this radio.
      const auto radio = i == 0 ? domain::Radio::kWifi : domain::Radio::kBle;
      const domain::RadioMessage restore =
          runner_fault_active_[i] ? runner_fault_[i] : domain::RadioMessage::FaultCleared(radio);
      if (!Send(restore)) return;
      overflow_fault_[i] = false;
    }
  }

 private:
  static bool Send(const domain::RadioMessage& message) {
    return xQueueSend(g_messages, &message, 0) == pdTRUE;
  }

  // What the runner said, so a resync can restore it; and a fault or clear from
  // the runner replaces any report-overflow text the app loop is showing.
  void Observe(const domain::RadioMessage& message) {
    if (message.kind == domain::RadioMessageKind::kFault) {
      const size_t i = Index(message.fault.radio);
      runner_fault_active_[i] = true;
      runner_fault_[i] = message;
      overflow_fault_[i] = false;
    } else if (message.kind == domain::RadioMessageKind::kFaultCleared) {
      const size_t i = Index(message.cleared_radio);
      runner_fault_active_[i] = false;
      overflow_fault_[i] = false;
    }
  }

  void Lost(const domain::RadioMessage& message) {
    g_message_overflows.fetch_add(1, std::memory_order_relaxed);
    switch (message.kind) {
      case domain::RadioMessageKind::kConfigApplied:
        config_ = message;  // only the newest matters
        config_pending_ = true;
        break;
      case domain::RadioMessageKind::kFault:
      case domain::RadioMessageKind::kFaultCleared: {
        const auto radio = message.kind == domain::RadioMessageKind::kFault ? message.fault.radio
                                                                            : message.cleared_radio;
        fault_[Index(radio)] = message;
        fault_pending_[Index(radio)] = true;
        break;
      }
      case domain::RadioMessageKind::kWindowStarted:
        ++lost_[Index(message.started.radio)];
        clean_sends_[Index(message.started.radio)] = 0;
        break;
      case domain::RadioMessageKind::kWindowReport:
        ++lost_[Index(message.report.window.radio)];
        clean_sends_[Index(message.report.window.radio)] = 0;
        break;
    }
  }

  bool config_pending_ = false;
  domain::RadioMessage config_;
  bool fault_pending_[2] = {};
  domain::RadioMessage fault_[2];
  uint32_t lost_[2] = {};
  uint32_t clean_sends_[2] = {};
  bool overflow_fault_[2] = {};
  bool runner_fault_active_[2] = {};
  domain::RadioMessage runner_fault_[2];
};

void RadioTaskMain(void* /*arg*/) {
  // WiFi first, then BLE: their PHY bring-up must not overlap. A failure is
  // kept by the driver and surfaces as that radio's kInit fault at its first
  // window; it does not stop the other radio.
  g_driver->InitReceivers();
  const domain::DriverResult wifi = g_driver->wifi_init();
  const domain::DriverResult ble = g_driver->ble_init();
  ESP_LOGI(kTag, "init: wifi=%s (%ld) ble=%s (%ld)", wifi.ok ? "ok" : "FAILED",
           static_cast<long>(wifi.code), ble.ok ? "ok" : "FAILED", static_cast<long>(ble.code));

  static QueueControlSource control;
  static QueueRadioSink sink;
  // Both radios on with the default plan. Not persisted (TB16).
  domain::RadioRunner runner(*g_driver, control, sink, domain::ObservationConfig{});
  for (;;) {
    runner.Step();
    sink.Flush();
  }
}

}  // namespace

bool StartRadio() {
  if (g_task != nullptr) return true;
  g_messages = xQueueCreate(kMessageQueueDepth, sizeof(domain::RadioMessage));
  g_controls = xQueueCreate(kControlQueueDepth, sizeof(domain::ControlRequest));
  g_driver = new (std::nothrow) Esp32RadioDriver();
  if (g_messages == nullptr || g_controls == nullptr || g_driver == nullptr) {
    ESP_LOGE(kTag, "radio not started: out of memory");
    return false;
  }
  if (!g_driver->StartWorkers()) {
    // Not fatal for WiFi: BLE then reports an init fault, WiFi still runs.
    ESP_LOGE(kTag, "hci worker not started: BLE will be faulted");
  } else {
    capacity::RegisterTask("hci_worker", g_driver->hci_worker());
  }
  if (xTaskCreatePinnedToCore(RadioTaskMain, "radio", kRadioTaskStackBytes, nullptr,
                              kRadioTaskPriority, &g_task, kRadioTaskCore) != pdPASS) {
    g_task = nullptr;
    ESP_LOGE(kTag, "radio task not started");
    return false;
  }
  capacity::RegisterTask("radio", g_task);
  return true;
}

bool RequestControl(const domain::ControlRequest& request) {
  if (g_task == nullptr || g_controls == nullptr) return false;
  if (xQueueSend(g_controls, &request, 0) != pdTRUE) {
    g_control_overflows.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  xTaskNotify(g_task, Esp32RadioDriver::kControlNotifyBit, eSetBits);
  return true;
}

bool ReceiveMessage(domain::RadioMessage& out) {
  return g_messages != nullptr && xQueueReceive(g_messages, &out, 0) == pdTRUE;
}

RadioStatus ReadStatus() {
  RadioStatus status;
  status.message_overflows = g_message_overflows.load(std::memory_order_relaxed);
  status.control_overflows = g_control_overflows.load(std::memory_order_relaxed);
  if (g_driver == nullptr) return status;
  status.started = g_task != nullptr;
  const auto counts = g_driver->gate().counts();
  for (size_t i = 0; i < 2; ++i) {
    status.events_drained[i] = counts.drained[i];
    status.outside_window[i] = counts.outside_window[i];
    status.wrong_radio[i] = counts.wrong_radio[i];
  }
  status.ring_drops = g_driver->gate().ring_drops_total();
  status.unattributed_drops = counts.unattributed_drops;
  status.hci = g_driver->hci().stats();
  status.wifi_init_ok = g_driver->wifi_init().ok;
  status.wifi_init_code = g_driver->wifi_init().code;
  status.ble_init_ok = g_driver->ble_init().ok;
  status.ble_init_code = g_driver->ble_init().code;
  if (g_task != nullptr) {
    status.radio_stack_free_bytes = uxTaskGetStackHighWaterMark(g_task) * sizeof(StackType_t);
  }
  return status;
}

void PublishAppView(const AppView& view) {
  portENTER_CRITICAL(&g_view_mux);
  g_view = view;
  portEXIT_CRITICAL(&g_view_mux);
}

AppView ReadAppView() {
  AppView view;
  portENTER_CRITICAL(&g_view_mux);
  view = g_view;
  portEXIT_CRITICAL(&g_view_mux);
  return view;
}

}  // namespace firmware::platform::esp32::radio
