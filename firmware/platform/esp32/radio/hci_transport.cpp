// SPDX-License-Identifier: Apache-2.0
#include "hci_transport.hpp"

#include <algorithm>
#include <cstring>

#include "esp_bt.h"
#include "esp_timer.h"

namespace firmware::platform::esp32::radio {

namespace {

using domain::hci::Command;
using domain::hci::IssueResult;
using domain::hci::Outcome;

constexpr uint8_t kH4Event = 0x04;
constexpr uint8_t kLeMetaEvent = 0x3e;
constexpr uint64_t kTickUs = 1'000'000ULL / configTICK_RATE_HZ;

HciTransport* g_transport = nullptr;

uint64_t NowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

// A timeout in ticks that is never shorter than `remaining_us`.
TickType_t TicksFor(uint64_t remaining_us) {
  return static_cast<TickType_t>(std::max<uint64_t>(1, (remaining_us + kTickUs - 1) / kTickUs));
}

struct Frame {
  uint64_t arrival_us;
  uint8_t bytes[HciTransport::kMaxPacketBytes];
};

int OnHostReceive(uint8_t* data, uint16_t length) {
  return g_transport != nullptr ? g_transport->OnReceive(data, length) : ESP_FAIL;
}
void OnHostSendAvailable() {
  if (g_transport != nullptr) g_transport->OnSendAvailable();
}

const esp_vhci_host_callback_t kVhciCallbacks = {
    .notify_host_send_available = OnHostSendAvailable,
    .notify_host_recv = OnHostReceive,
};

struct ParseContext {
  AdvertisingHandler handler;
  void* handler_context;
  uint64_t arrival_us;
};

}  // namespace

class HciTransport::Guard {
 public:
  explicit Guard(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex_); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};

bool HciTransport::Start(AdvertisingHandler handler, void* context) {
  handler_ = handler;
  handler_context_ = context;
  mutex_ = xSemaphoreCreateMutex();
  wake_ = xSemaphoreCreateBinary();
  command_buffer_ = xMessageBufferCreate(kCommandBufferBytes);
  advertising_buffer_ = xMessageBufferCreate(kAdvertisingBufferBytes);
  if (mutex_ == nullptr || wake_ == nullptr || command_buffer_ == nullptr ||
      advertising_buffer_ == nullptr) {
    return false;
  }
  g_transport = this;
  return xTaskCreatePinnedToCore(WorkerMain, "hci_worker", kWorkerStackBytes, this, kWorkerPriority,
                                 &worker_, kWorkerCore) == pdPASS;
}

void HciTransport::Wake() {
  if (wake_ == nullptr) return;
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(wake_, &woken);
    portYIELD_FROM_ISR(woken);
  } else {
    xSemaphoreGive(wake_);
  }
}

void HciTransport::OnSendAvailable() { Wake(); }

// Runs in the controller's context. No parsing, no locks, no logging: copy the
// packet into a bounded buffer, count it if there is no room, wake the worker.
int HciTransport::OnReceive(const uint8_t* data, uint16_t length) {
  if (length < 2 || length > kMaxPacketBytes) {
    malformed_.fetch_add(1, std::memory_order_relaxed);
    return ESP_OK;
  }
  Frame frame;
  frame.arrival_us = NowUs();
  std::memcpy(frame.bytes, data, length);
  // Routing only: LE Meta events (advertising floods) and everything else go to
  // different buffers so a flood cannot evict command completions.
  const bool advertising = data[0] == kH4Event && data[1] == kLeMetaEvent;
  MessageBufferHandle_t buffer = advertising ? advertising_buffer_ : command_buffer_;
  const size_t size = sizeof(frame.arrival_us) + length;
  size_t sent = 0;
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    sent = xMessageBufferSendFromISR(buffer, &frame, size, &woken);
    if (sent != 0) vTaskNotifyGiveFromISR(worker_, &woken);
    portYIELD_FROM_ISR(woken);
  } else {
    sent = xMessageBufferSend(buffer, &frame, size, 0);
    if (sent != 0) xTaskNotifyGive(worker_);
  }
  if (sent == 0) rx_overflow_.fetch_add(1, std::memory_order_relaxed);
  return ESP_OK;
}

void HciTransport::WorkerMain(void* self) { static_cast<HciTransport*>(self)->RunWorker(); }

void HciTransport::RunWorker() {
  Frame frame;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      size_t size = xMessageBufferReceive(command_buffer_, &frame, sizeof(frame), 0);
      if (size == 0) size = xMessageBufferReceive(advertising_buffer_, &frame, sizeof(frame), 0);
      if (size == 0) break;
      if (size <= sizeof(frame.arrival_us)) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ParseContext context{handler_, handler_context_, frame.arrival_us};
      HandlePacket(frame.bytes, size - sizeof(frame.arrival_us), frame.arrival_us, &context);
    }
  }
}

void HciTransport::HandlePacket(const uint8_t* bytes, size_t length, uint64_t arrival_us,
                                void* context) {
  const domain::hci::Event event = domain::hci::Parse(
      bytes, length,
      [](const domain::hci::Report& report, void* raw) {
        const auto* parse = static_cast<const ParseContext*>(raw);
        if (parse->handler != nullptr)
          parse->handler(report, parse->arrival_us, parse->handler_context);
      },
      context);
  switch (event.kind) {
    case domain::hci::Kind::kMalformed:
      malformed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case domain::hci::Kind::kComplete:
    case domain::hci::Kind::kStatus: {
      {
        Guard guard(mutex_);
        if (domain::hci::IsStaleAfterReset(event, arrival_us, reset_us_)) {
          // Queued before the last controller reset: it answers an abandoned command.
          stale_dropped_.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        const domain::hci::Result result = tracker_.OnEvent(event, NowUs());
        if (result.outcome != Outcome::kNone) {
          result_ = result;
          result_ready_ = true;
        }
      }
      Wake();  // a completion, or new credit, may unblock the waiting command
      break;
    }
    case domain::hci::Kind::kHardwareError:
      hardware_errors_.fetch_add(1, std::memory_order_relaxed);
      hardware_error_code_.store(event.status, std::memory_order_relaxed);
      hardware_error_pending_.store(true, std::memory_order_release);
      break;
    case domain::hci::Kind::kAdvertising:  // already delivered report by report
    case domain::hci::Kind::kIgnored:
      break;
  }
}

CommandResult HciTransport::Execute(Command command, const domain::hci::Options& options,
                                    uint64_t timeout_us) {
  using Code = CommandResult::Code;
  size_t length = 0;
  uint64_t deadline_us = 0;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  length = domain::hci::Build(command, options, send_packet_, sizeof(send_packet_));
  if (length == 0) {
    xSemaphoreGive(mutex_);
    return {Code::kUnavailable, static_cast<uint8_t>(IssueResult::kInvalid)};
  }

  // The deadline starts here and covers send availability as well as completion.
  const uint64_t issued_us = NowUs();
  const IssueResult issue = tracker_.Issue(command, issued_us, timeout_us);
  if (issue != IssueResult::kIssued) {
    xSemaphoreGive(mutex_);
    if (issue == IssueResult::kNeedsControllerReset) return {Code::kNeedsReset, 0};
    return {Code::kUnavailable, static_cast<uint8_t>(issue)};
  }

  result_ready_ = false;
  xSemaphoreTake(wake_, 0);  // a wake from before this command carries no news
  deadline_us = issued_us + timeout_us;
  bool sent = false;
  // The mutex is held at the top of each pass and never across the send or a
  // wait: esp_vhci_host_send_packet may block until the controller wakes, and
  // the worker needs the mutex to record completions.
  for (;;) {
    const uint64_t now_us = NowUs();
    CommandResult done;
    done.sent = sent;
    bool finished = true;
    if (result_ready_) {
      result_ready_ = false;
      switch (result_.outcome) {
        case Outcome::kCompleted:
          done.code = Code::kCompleted;
          break;
        case Outcome::kRejected:
          rejections_.fetch_add(1, std::memory_order_relaxed);
          done.code = Code::kRejected;
          done.detail = result_.status;
          break;
        case Outcome::kTimedOut:
          timeouts_.fetch_add(1, std::memory_order_relaxed);
          done.code = Code::kTimedOut;
          break;
        default:
          done.code = Code::kUnavailable;
          done.detail = static_cast<uint8_t>(IssueResult::kInvalid);
          break;
      }
    } else if (tracker_.Poll(now_us).outcome == Outcome::kTimedOut) {
      timeouts_.fetch_add(1, std::memory_order_relaxed);
      done.code = Code::kTimedOut;
    } else {
      finished = false;
    }
    if (finished) {
      xSemaphoreGive(mutex_);
      return done;
    }

    const bool send_now =
        !sent && esp_vhci_host_check_send_available() && tracker_.TryBeginSend(now_us);
    xSemaphoreGive(mutex_);
    if (send_now) {
      esp_vhci_host_send_packet(send_packet_, static_cast<uint16_t>(length));
      commands_sent_.fetch_add(1, std::memory_order_relaxed);
      sent = true;
    } else {
      // Poll fires only strictly after the deadline, hence the +1.
      const uint64_t remaining_us = deadline_us >= now_us ? deadline_us - now_us + 1 : 1;
      xSemaphoreTake(wake_, TicksFor(remaining_us));
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
}

bool HciTransport::TakeHardwareError(uint8_t* code) {
  if (!hardware_error_pending_.exchange(false, std::memory_order_acquire)) return false;
  *code = hardware_error_code_.load(std::memory_order_relaxed);
  return true;
}

bool HciTransport::RunBoot() {
  domain::hci::BootSequencer sequencer;  // interval == window == 0x50: continuous within a window
  while (const domain::hci::BootStep* step = sequencer.current()) {
    const uint64_t timeout_us =
        step->command == Command::kReset ? kResetTimeoutUs : kCommandTimeoutUs;
    const CommandResult result = Execute(step->command, step->options, timeout_us);
    switch (result.code) {
      case CommandResult::Code::kCompleted:
        sequencer.OnResult({Outcome::kCompleted, 0});
        break;
      case CommandResult::Code::kRejected:
        sequencer.OnResult({Outcome::kRejected, result.detail});
        break;
      case CommandResult::Code::kTimedOut:
        sequencer.OnResult({Outcome::kTimedOut, 0});
        break;
      default:
        return false;  // not issued, or the controller needs a reset
    }
  }
  return sequencer.complete();
}

bool HciTransport::CycleController() {
  const esp_err_t disabled = esp_bt_controller_disable();
  {
    // Command events already queued by the old controller answer abandoned
    // commands: HandlePacket drops anything that arrived before this instant.
    Guard guard(mutex_);
    reset_us_ = NowUs();
  }
  if (disabled != ESP_OK && disabled != ESP_ERR_INVALID_STATE) return false;
  if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return false;
  if (esp_vhci_host_register_callback(&kVhciCallbacks) != ESP_OK) return false;
  {
    Guard guard(mutex_);
    tracker_.ResetController();
    result_ready_ = false;
  }
  controller_resets_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool HciTransport::BootWithRetries(bool cycle_first) {
  for (int attempt = 0; attempt < kMaxBootAttempts; ++attempt) {
    if ((attempt > 0 || cycle_first) && !CycleController()) continue;
    if (RunBoot()) return true;
  }
  return false;
}

esp_err_t HciTransport::BringUp() {
  if (!controller_initialised_) {
    // CAUTION: in ESP-IDF v5.3.2 this call can abort() inside the library (it
    // wraps its work in ESP_ERROR_CHECK) instead of returning an error, e.g. if
    // the controller memory was already released or is not idle. It cannot be
    // made recoverable from here; it runs once, first, before any other
    // Bluetooth call (see the TB09 physical check list).
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) return err;
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&config);
    if (err != ESP_OK) return err;
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) return err;
    err = esp_vhci_host_register_callback(&kVhciCallbacks);
    if (err != ESP_OK) return err;
    controller_initialised_ = true;
  }
  return BootWithRetries(false) ? ESP_OK : ESP_FAIL;
}

HciTransport::Recovery HciTransport::Recover() {
  bool cycled = false;
  for (int attempt = 0; attempt < kMaxBootAttempts; ++attempt) {
    if (!CycleController()) continue;
    cycled = true;
    if (RunBoot()) return Recovery::kRecovered;
  }
  return cycled ? Recovery::kCycledNotBooted : Recovery::kNotCycled;
}

HciStats HciTransport::stats() const {
  HciStats stats;
  stats.rx_overflow = rx_overflow_.load(std::memory_order_relaxed);
  stats.malformed = malformed_.load(std::memory_order_relaxed);
  stats.stale_dropped = stale_dropped_.load(std::memory_order_relaxed);
  stats.commands_sent = commands_sent_.load(std::memory_order_relaxed);
  stats.timeouts = timeouts_.load(std::memory_order_relaxed);
  stats.rejections = rejections_.load(std::memory_order_relaxed);
  stats.hardware_errors = hardware_errors_.load(std::memory_order_relaxed);
  stats.controller_resets = controller_resets_.load(std::memory_order_relaxed);
  if (mutex_ != nullptr && xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    stats.late = tracker_.late();
    stats.unexpected = tracker_.unexpected();
    stats.credit = tracker_.credit();
    xSemaphoreGive(mutex_);
  }
  if (worker_ != nullptr) {
    stats.worker_stack_free_bytes = uxTaskGetStackHighWaterMark(worker_) * sizeof(StackType_t);
  }
  return stats;
}

}  // namespace firmware::platform::esp32::radio
