// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace firmware::domain::hci {
enum class Command : uint16_t {
  kReset = 0x0c03,
  kSetEventMask = 0x0c01,
  kLeSetEventMask = 0x2001,
  kLeSetScanParameters = 0x200b,
  kLeSetScanEnable = 0x200c
};
struct Options {
  std::array<uint8_t, 8> mask{};
  uint16_t interval = 0x50;
  uint16_t window = 0x50;
  bool enable = false;
};
size_t Build(Command command, const Options& options, uint8_t* output, size_t capacity);
struct Report {
  uint8_t event_type, address_type;
  std::array<uint8_t, 6> address;
  uint8_t data_length;
  // Points into the packet; valid only during the report callback. Do not retain.
  const uint8_t* data;
  int8_t rssi;
};
enum class Kind { kMalformed, kIgnored, kComplete, kStatus, kAdvertising, kHardwareError };
struct Event {
  Kind kind = Kind::kMalformed;
  uint8_t code = 0, status = 0, credit = 0;
  uint16_t opcode = 0;
};
using ReportCallback = void (*)(const Report&, void*);
Event Parse(const uint8_t* packet, size_t length, ReportCallback callback = nullptr,
            void* context = nullptr);
// A command event (Complete or Status) that arrived before the controller was
// last reset answers a command the reset abandoned: it must not reach the
// Tracker, or it could satisfy the next command of the same opcode. Times are
// the transport's monotonic microseconds; reset_us 0 means no reset yet.
inline bool IsStaleAfterReset(const Event& event, uint64_t arrival_us, uint64_t reset_us) {
  return (event.kind == Kind::kComplete || event.kind == Kind::kStatus) && arrival_us < reset_us;
}
enum class Outcome { kNone, kCompleted, kRejected, kTimedOut, kInvalid };
struct Result {
  Outcome outcome = Outcome::kNone;
  uint8_t status = 0;
};
enum class IssueResult { kIssued, kBusy, kNoCredit, kNeedsControllerReset, kQuarantined, kInvalid };
// Issue starts the deadline before waiting for VHCI availability. Call
// TryBeginSend immediately before transmission; transmit only if it returns true.
// Serialize tracker operations with sending and event delivery. Poll while waiting.
// A sent command that times out quarantines its opcode for twice its timeout
// after the deadline, or until its late event arrives. HCI has no transaction
// ID: after this bounded quarantine an exceptionally late event is ambiguous;
// reset/reinitialize the controller if that risk cannot be accepted.
class Tracker {
 public:
  IssueResult Issue(Command command, uint64_t now_us, uint64_t timeout_us);
  bool TryBeginSend(uint64_t now_us);
  void ResetController();
  Result OnEvent(const Event& event, uint64_t now_us);
  Result Poll(uint64_t now_us);
  uint8_t credit() const { return credit_; }
  uint64_t unexpected() const { return unexpected_; }
  uint64_t late() const { return late_; }

 private:
  bool pending_ = false, sent_ = false;
  uint16_t opcode_ = 0;
  uint64_t deadline_ = 0, timeout_ = 0;
  uint8_t credit_ = 1;
  std::array<uint64_t, 5> quarantine_until_{};
  std::array<bool, 5> quarantined_{};
  uint64_t unexpected_ = 0, late_ = 0;
};
struct BootStep {
  Command command;
  Options options;
};
std::array<BootStep, 4> BootSequence(uint16_t interval = 0x50, uint16_t window = 0x50);
class BootSequencer {
 public:
  explicit BootSequencer(uint16_t interval = 0x50, uint16_t window = 0x50);
  const BootStep* current() const;
  void OnResult(Result result);
  bool complete() const;
  bool failed() const;
  size_t step_index() const { return index_; }
  Result failure() const { return failure_; }

 private:
  std::array<BootStep, 4> steps_;
  size_t index_ = 0;
  Result failure_{};
};
}  // namespace firmware::domain::hci
