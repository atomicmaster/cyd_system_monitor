// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "firmware/domain/hci/hci.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace firmware::platform::esp32::radio {

// Counters for DEV:RADIO_STATUS. Written by one task each and read by the
// console, so relaxed 32-bit atomics are enough.
struct HciStats {
  uint32_t rx_overflow = 0;  // packets the VHCI callback could not buffer
  uint32_t malformed = 0;    // received packets hci::Parse rejected
  uint32_t commands_sent = 0;
  uint32_t timeouts = 0;
  uint32_t rejections = 0;  // Command Complete/Status with a nonzero status
  uint32_t hardware_errors = 0;
  uint32_t controller_resets = 0;
  uint64_t late = 0;        // late completions (Tracker)
  uint64_t unexpected = 0;  // completions nobody was waiting for (Tracker)
  uint8_t credit = 0;
  uint32_t worker_stack_free_bytes = 0;
};

struct CommandResult {
  enum class Code : uint8_t {
    kCompleted,
    kRejected,     // `detail` is the HCI status
    kTimedOut,     // deadline covered send availability plus completion
    kUnavailable,  // not issued: busy, quarantined, no credit or invalid; `detail` is the
                   // IssueResult
    kNeedsReset,   // the controller no longer returns credit: reset it
  };
  Code code = Code::kUnavailable;
  uint8_t detail = 0;
  bool ok() const { return code == Code::kCompleted; }
};

// Called on the HCI worker for each advertising report; `arrival_us` is when the
// VHCI callback received the packet. `data` is valid only during the call.
using AdvertisingHandler = void (*)(const domain::hci::Report& report, uint64_t arrival_us,
                                    void* context);

// The controller-only BLE path (ADR 0001: it never transmits anything but HCI
// commands to the local controller). The VHCI host callback only copies each
// H4 packet into a bounded message buffer; the worker task parses it.
//
// Two buffers keep an advertising flood from starving command completions:
// the callback reads only the H4 event code (to choose a buffer, no decoding);
// the worker serves the command-event buffer first.
//
// Commands run from one caller at a time (the radio task). The Tracker is not
// thread-safe, so every Tracker call, the send itself and event delivery are
// serialised by one mutex.
class HciTransport {
 public:
  static constexpr size_t kMaxPacketBytes = 260;  // H4 header plus 255 parameter bytes
  static constexpr size_t kCommandBufferBytes = 1024;
  static constexpr size_t kAdvertisingBufferBytes = 4096;
  static constexpr uint32_t kWorkerStackBytes = 3584;
  static constexpr UBaseType_t kWorkerPriority = 5;
  static constexpr BaseType_t kWorkerCore = 1;
  // A command's deadline covers waiting for the controller to accept it plus its completion.
  static constexpr uint64_t kCommandTimeoutUs = 500'000;
  static constexpr uint64_t kResetTimeoutUs = 1'000'000;
  // Bring-up attempts (initial boot plus controller resets) before the radio is faulted.
  static constexpr int kMaxBootAttempts = 3;

  // Allocates the buffers and starts the worker. False when memory or the task is missing.
  bool Start(AdvertisingHandler handler, void* context);

  // Initialises the controller (first call only), then boots it. Bounded:
  // kMaxBootAttempts, resetting the controller between attempts.
  esp_err_t BringUp();
  // Disables and re-enables the controller, clears the Tracker and boots again.
  bool Recover();

  CommandResult Execute(domain::hci::Command command, const domain::hci::Options& options,
                        uint64_t timeout_us);

  // True once per Hardware Error event; `code` receives its HCI status.
  bool TakeHardwareError(uint8_t* code);

  HciStats stats() const;
  TaskHandle_t worker() const { return worker_; }

  // Used by the VHCI callbacks (file scope in the .cpp).
  int OnReceive(const uint8_t* data, uint16_t length);
  void OnSendAvailable();

 private:
  class Guard;
  static void WorkerMain(void* self);
  void RunWorker();
  void HandlePacket(const uint8_t* bytes, size_t length, void* context);
  bool RunBoot();
  bool BootWithRetries(bool cycle_first);
  bool CycleController();
  void Wake();

  domain::hci::Tracker tracker_;
  uint8_t send_packet_[16] = {};  // the controller reads it from here; only Execute writes it
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t wake_ = nullptr;  // binary: send available or an event was processed
  MessageBufferHandle_t command_buffer_ = nullptr;
  MessageBufferHandle_t advertising_buffer_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  AdvertisingHandler handler_ = nullptr;
  void* handler_context_ = nullptr;
  bool controller_initialised_ = false;
  // Guarded by mutex_.
  bool result_ready_ = false;
  domain::hci::Result result_;

  std::atomic<uint32_t> rx_overflow_{0};
  std::atomic<uint32_t> malformed_{0};
  std::atomic<uint32_t> commands_sent_{0};
  std::atomic<uint32_t> timeouts_{0};
  std::atomic<uint32_t> rejections_{0};
  std::atomic<uint32_t> hardware_errors_{0};
  std::atomic<uint32_t> controller_resets_{0};
  std::atomic<bool> hardware_error_pending_{false};
  std::atomic<uint8_t> hardware_error_code_{0};
};

}  // namespace firmware::platform::esp32::radio
