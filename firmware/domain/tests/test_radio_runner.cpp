// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "firmware/domain/coverage.hpp"
#include "firmware/domain/event_ring.hpp"
#include "firmware/domain/radio_messages.hpp"
#include "firmware/domain/radio_runner.hpp"

namespace {

using namespace firmware::domain;

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

constexpr uint64_t kForever = std::numeric_limits<uint64_t>::max();

class FakeControl : public ControlSource {
 public:
  std::optional<ControlRequest> Poll() override {
    if (pending.empty()) return std::nullopt;
    ControlRequest request = pending.front();
    pending.pop_front();
    return request;
  }
  std::deque<ControlRequest> pending;
};

class FakeSink : public RadioSink {
 public:
  void Emit(const RadioMessage& message) override { messages.push_back(message); }

  std::vector<WindowReport> Reports() const {
    std::vector<WindowReport> reports;
    for (const auto& m : messages) {
      if (m.kind == RadioMessageKind::kWindowReport) reports.push_back(m.report);
    }
    return reports;
  }
  size_t Count(RadioMessageKind kind) const {
    return static_cast<size_t>(std::count_if(
        messages.begin(), messages.end(), [&](const RadioMessage& m) { return m.kind == kind; }));
  }
  // Index of the first message of `kind` at or after `from`, or messages.size().
  size_t IndexOf(RadioMessageKind kind, size_t from = 0) const {
    for (size_t i = from; i < messages.size(); ++i) {
      if (messages[i].kind == kind) return i;
    }
    return messages.size();
  }
  std::vector<RadioMessage> messages;
};

// A scripted Start: the result, and whether a failure still left receive on.
struct ScriptedStart {
  DriverResult result;
  bool leaves_on = false;
};

constexpr ScriptedStart Ok() { return {}; }
constexpr ScriptedStart Fail(FaultOp op, int32_t code, bool leaves_on = false) {
  return {{false, op, code}, leaves_on};
}

// A driver on a fake clock: each operation costs a configured amount of time.
class FakeDriver : public RadioDriver {
 public:
  struct Begin {
    uint32_t window_id;
    uint32_t generation;
    uint64_t start_us;
    uint64_t deadline_us;
    uint64_t called_at_us;
  };

  explicit FakeDriver(FakeControl& control) : control_(control) {}

  uint64_t NowUs() override { return now_us; }
  void BeginWindow(uint32_t window_id, uint32_t generation, uint64_t start_us,
                   uint64_t deadline_us) override {
    begun.push_back({window_id, generation, start_us, deadline_us, now_us});
    log.push_back("begin");
  }
  void EndWindow() override {
    ++ends;
    log.push_back("end");
  }
  DriverResult StartWifi(uint8_t channel) override {
    wifi_channels.push_back(channel);
    return Start(0, start_wifi_cost_us);
  }
  DriverResult StopWifi() override { return Stop(0); }
  DriverResult StartBle() override { return Start(1, start_ble_cost_us); }
  DriverResult StopBle() override { return Stop(1); }

  WakeReason WaitUntilUs(uint64_t deadline_us) override {
    waits.push_back(deadline_us);
    if (waits.size() > 100'000) throw std::runtime_error("runaway wait");
    if (immediate_returns > 0) {  // returns at once, the clock untouched
      --immediate_returns;
      return WakeReason::kDeadline;
    }
    if (spurious_control_wakes > 0) {  // a control wake with nothing to poll
      --spurious_control_wakes;
      return WakeReason::kControl;
    }
    if (!control_.pending.empty()) return WakeReason::kControl;
    if (scheduled && scheduled->first <= deadline_us) {
      now_us = std::max(now_us, scheduled->first);
      control_.pending.push_back(scheduled->second);
      scheduled.reset();
      return WakeReason::kControl;
    }
    if (deadline_us == kForever) throw std::runtime_error("waited forever with no control due");
    if (now_us < deadline_us) {
      if (max_wait_advance_us != 0 && now_us + max_wait_advance_us < deadline_us) {
        now_us += max_wait_advance_us;  // a tick timeout that fires early
        return WakeReason::kDeadline;
      }
      now_us = deadline_us + wake_latency_us;
    }
    return WakeReason::kDeadline;
  }
  void DrainEvents() override {
    ++drains;
    log.push_back("drain");
  }
  uint32_t TakeDrops(uint32_t window_id) override {
    const auto it = drops.find(window_id);
    if (it == drops.end()) return 0;
    const uint32_t count = it->second;
    drops.erase(it);
    return count;
  }
  uint32_t TakeUnattributed() override { return std::exchange(unattributed, 0); }

  void ScheduleControl(uint64_t at_us, ControlRequest request) {
    scheduled = std::make_pair(at_us, request);
  }
  int StopCalls() const { return stops[0] + stops[1]; }

  uint64_t now_us = 0;
  uint64_t start_wifi_cost_us = 800;  // F08: a WiFi channel switch is under 1 ms
  uint64_t start_ble_cost_us = 3000;  // F08: a BLE scan enable is 1.4-5.4 ms
  uint64_t stop_cost_us = 200;
  uint64_t wake_latency_us = 0;
  uint64_t max_wait_advance_us = 0;  // 0: a wait reaches its deadline
  int immediate_returns = 0;
  int spurious_control_wakes = 0;
  std::deque<ScriptedStart> start_results[2];
  std::deque<DriverResult> stop_results[2];
  std::map<uint32_t, uint32_t> drops;
  uint32_t unattributed = 0;
  std::optional<std::pair<uint64_t, ControlRequest>> scheduled;

  std::vector<uint64_t> waits;
  std::vector<uint8_t> wifi_channels;
  std::vector<Begin> begun;
  std::vector<uint64_t> stop_times_us;
  std::vector<std::string> log;
  int drains = 0;
  int ends = 0;
  int starts[2] = {0, 0};
  int stops[2] = {0, 0};
  int overlaps = 0;       // a receiver started while the other was still on
  int double_starts = 0;  // a receiver started while it was already on

 private:
  DriverResult Start(int radio, uint64_t cost_us) {
    ++starts[radio];
    log.push_back(radio == 0 ? "start wifi" : "start ble");
    if (on[1 - radio]) ++overlaps;
    if (on[radio]) ++double_starts;
    now_us += cost_us;
    ScriptedStart scripted;
    if (!start_results[radio].empty()) {
      scripted = start_results[radio].front();
      start_results[radio].pop_front();
    }
    if (!scripted.result.ok) {
      if (scripted.leaves_on) on[radio] = true;
      return scripted.result;
    }
    on[radio] = true;
    return {};
  }
  DriverResult Stop(int radio) {
    ++stops[radio];
    stop_times_us.push_back(now_us);
    log.push_back(radio == 0 ? "stop wifi" : "stop ble");
    now_us += stop_cost_us;
    if (!stop_results[radio].empty()) {
      const DriverResult result = stop_results[radio].front();
      stop_results[radio].pop_front();
      if (!result.ok) return result;
    }
    on[radio] = false;
    return {};
  }

  FakeControl& control_;
  bool on[2] = {false, false};
};

struct Rig {
  explicit Rig(ObservationConfig config = {})
      : driver(control), runner(driver, control, sink, std::move(config)) {}
  void Steps(int count) {
    for (int i = 0; i < count; ++i) runner.Step();
  }
  FakeControl control;
  FakeDriver driver;
  FakeSink sink;
  RadioRunner runner;
};

// A driver whose receive callback pushes into a real EventRing while a window
// is open, as the platform does, and whose drain empties the ring.
class RingDriver : public FakeDriver {
 public:
  struct Ev {
    uint32_t window_id;
    uint64_t seq;
  };
  static constexpr size_t kRingSize = 8;

  RingDriver(FakeControl& control, bool drain_on_close)
      : FakeDriver(control), drain_on_close_(drain_on_close) {}

  // Events the receiver delivers in window `id`.
  static size_t Burst(uint32_t id) { return (id * 7) % 23; }

  void BeginWindow(uint32_t id, uint32_t generation, uint64_t start_us,
                   uint64_t deadline_us) override {
    FakeDriver::BeginWindow(id, generation, start_us, deadline_us);
    open_ = id;
    delivered_ = false;
  }
  void EndWindow() override {
    FakeDriver::EndWindow();
    open_ = 0;
  }
  WakeReason WaitUntilUs(uint64_t deadline_us) override {
    if (open_ != 0 && !delivered_) {
      delivered_ = true;
      for (size_t i = 0; i < Burst(open_); ++i) ring.Push({open_, ++seq_});
    }
    return FakeDriver::WaitUntilUs(deadline_us);
  }
  void DrainEvents() override {
    FakeDriver::DrainEvents();
    if (!drain_on_close_) return;
    Ev batch[kRingSize];
    while (const size_t n = ring.PopBatch(batch, kRingSize)) consumed += n;
  }
  uint32_t TakeDrops(uint32_t id) override { return ring.TakeDrops(id); }
  uint32_t TakeUnattributed() override { return ring.TakeUnattributed(); }

  EventRing<Ev, kRingSize> ring;
  uint64_t consumed = 0;

 private:
  bool drain_on_close_;
  uint32_t open_ = 0;
  bool delivered_ = false;
  uint64_t seq_ = 0;
};

struct RingRig {
  explicit RingRig(bool drain_on_close)
      : driver(control, drain_on_close), runner(driver, control, sink) {}
  FakeControl control;
  RingDriver driver;
  FakeSink sink;
  RadioRunner runner;
};

// What the app loop does with each message.
void Apply(CoverageLedger& ledger, const RadioMessage& message) {
  switch (message.kind) {
    case RadioMessageKind::kWindowReport:
      ledger.Record(message.report);
      break;
    case RadioMessageKind::kFault:
      ledger.SetFault(message.fault.radio, FaultText(message.fault.op, message.fault.code));
      break;
    case RadioMessageKind::kFaultCleared:
      ledger.ClearFault(message.cleared_radio);
      break;
    default:
      break;
  }
}

}  // namespace

int main() {
  // Messages: the fault text and the configuration round trip.
  {
    Expect(FaultText(FaultOp::kStart, -258) == "start (-258)" &&
               FaultText(FaultOp::kHciTimeout, 0) == "hci timeout" &&
               FaultText(FaultOp::kHardwareError, 0) == "hardware error" &&
               FaultText(FaultOp::kHardwareError, 7) == "hardware error (7)",
           "fault text names the step and the code when there is one");
    ObservationConfig config{.ble_enabled = false,
                             .wifi_channels = {6, 99, 1, 6},
                             .wifi_dwell_ms = 150,
                             .ble_window_ms = 70'000};
    const ConfigAppliedMessage message = ToConfigApplied(7, config);
    Expect(message.config_generation == 7 && message.wifi_enabled && !message.ble_enabled &&
               message.channel_count == 2 && message.channels[0] == 6 && message.channels[1] == 1 &&
               message.wifi_dwell_ms == 150 && message.ble_window_ms == kMaxWindowMs,
           "kConfigApplied carries the effective configuration");
    const ObservationConfig back = ToObservationConfig(message);
    Expect(back.wifi_channels == std::vector<uint8_t>({6, 1}) && back.wifi_dwell_ms == 150 &&
               !back.ble_enabled && back.ble_window_ms == kMaxWindowMs,
           "kConfigApplied converts back to the same configuration");
    Expect(
        ToConfigApplied(1, {}).channel_count == 11 &&
            ToConfigApplied(1, {.wifi_channels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}})
                    .channel_count == 14,
        "the default plan and the widest plan both fit the message");
  }

  // Both radios alternate; each window loses only its switching time.
  {
    Rig rig;
    rig.Steps(4);
    const auto& m = rig.sink.messages;
    Expect(m[0].kind == RadioMessageKind::kConfigApplied && m[0].config.config_generation == 1 &&
               m[0].config.channel_count == 11,
           "the first step announces the initial configuration as generation 1");
    const auto reports = rig.sink.Reports();
    Expect(reports.size() == 4, "one report per window");
    Expect(reports[0].window.radio == Radio::kWifi && reports[0].window.channel == 1 &&
               reports[1].window.radio == Radio::kBle && reports[2].window.channel == 2 &&
               reports[3].window.radio == Radio::kBle,
           "WiFi walks its channels with a BLE window between");
    Expect(
        reports[0].window_id == 1 && reports[3].window_id == 4 && reports[0].config_generation == 1,
        "windows carry a monotonic id and the configuration generation");
    Expect(reports[0].window.start_ms == 0 && reports[0].window.planned_ms == 205 &&
               reports[0].observed_ms == 204,
           "WiFi observes the window minus the 0.8 ms channel switch, rounded down");
    Expect(reports[1].window.start_ms == 205 && reports[1].observed_ms == 202,
           "BLE starts after the WiFi stop completes and loses its 3 ms scan enable");
    Expect(reports[0].completed_ms == 205 && reports[0].end == WindowEnd::kCompleted,
           "the report carries the actual completion time");
    Expect(rig.driver.overlaps == 0, "the two receivers never run at once");
    Expect(EligibleForQuiet(reports[0]) && EligibleForQuiet(reports[1]),
           "fully observed drop-free windows are eligible for quiet");
    Expect(rig.driver.begun.size() == 4 && rig.driver.begun[2].window_id == 3 &&
               rig.driver.begun[2].generation == 1 && rig.driver.drains == 4,
           "the platform learns each window's id and drains its events before the report");
    Expect(rig.sink.Count(RadioMessageKind::kWindowStarted) == 4 &&
               rig.sink.messages[1].kind == RadioMessageKind::kWindowStarted &&
               rig.sink.messages[1].started.rx_start_ms == 0 &&
               rig.sink.messages[1].started.planned_ms == 205,
           "each window announces its receiver start before its report");
  }

  // Preparation that takes the whole window leaves nothing observed, and the
  // overrun does not push into the next window's time.
  {
    Rig rig;
    rig.driver.start_wifi_cost_us = 300'000;
    rig.Steps(3);
    const auto reports = rig.sink.Reports();
    Expect(reports[0].observed_ms == 0 && reports[0].end == WindowEnd::kCompleted,
           "preparation that outlasts the window observes nothing");
    Expect(reports[1].window.start_ms == 300,
           "the next window starts when the late one finished, not at the old deadline");
    Expect(reports[1].observed_ms == 202 && reports[2].observed_ms == 0,
           "the overrun costs only its own window");
    CoverageLedger ledger;
    for (const auto& message : rig.sink.messages) Apply(ledger, message);
    Expect(ledger.gaps().size() == 2 && ledger.gaps()[0].duration_ms == 205,
           "a fully lost window is a Coverage Gap");
  }

  // A late wake or overrun is never observed time and does not accumulate.
  {
    Rig rig;
    rig.driver.wake_latency_us = 50'000;
    rig.Steps(3);
    const auto reports = rig.sink.Reports();
    Expect(reports[0].observed_ms == 204,
           "time past the deadline is not counted: the receive interval is clipped to the window");
    Expect(reports[0].completed_ms == 255 && reports[1].window.start_ms == 255 &&
               reports[2].window.start_ms == 510,
           "each window starts when the previous one actually ended; lateness does not accumulate");
    Expect(reports[1].observed_ms == 202 && reports[2].observed_ms == 204,
           "later windows are measured against their own deadlines");
  }

  // A slow stop delays the next window but does not reduce observed time.
  {
    Rig rig;
    rig.driver.stop_cost_us = 20'000;
    rig.Steps(2);
    const auto reports = rig.sink.Reports();
    Expect(reports[0].observed_ms == 204 && reports[0].completed_ms == 225,
           "the receive interval ends before the stop is issued");
    Expect(reports[1].window.start_ms == 225, "the next window waits for the slow stop");
  }

  // A stop that fails fails closed: the other receiver does not start until it works.
  {
    Rig rig;
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -3}, {false, FaultOp::kStop, -3}};
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 1, "a failed stop raises a fault");
    const size_t fault = rig.sink.IndexOf(RadioMessageKind::kFault);
    Expect(rig.sink.messages[fault].fault.radio == Radio::kWifi &&
               rig.sink.messages[fault].fault.op == FaultOp::kStop &&
               rig.sink.messages[fault].fault.code == -3,
           "the fault names the radio, step and code");
    const auto first = rig.sink.Reports().at(0);
    Expect(first.end == WindowEnd::kDriverFault && first.observed_ms == 204,
           "the window reports the fault but keeps the time it really received");

    rig.Steps(1);  // waits the backoff, retries, still fails
    Expect(rig.driver.starts[1] == 0 && rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "BLE does not start while WiFi cannot be stopped");
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 1, "retries do not repeat the fault");
    Expect(rig.driver.waits.back() - rig.driver.waits[rig.driver.waits.size() - 2] <=
               2 * RadioRunner::kStopRetryBackoffUs,
           "the retry is spaced by the bounded backoff");

    rig.Steps(1);  // the stop works now
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1 && rig.driver.starts[1] == 0,
           "a successful retry clears the fault before anything else runs");
    rig.Steps(1);
    Expect(rig.driver.starts[1] == 1 && rig.driver.overlaps == 0 &&
               rig.sink.IndexOf(RadioMessageKind::kFaultCleared) <
                   rig.sink.IndexOf(RadioMessageKind::kWindowStarted, fault),
           "scheduling resumes after the fault clears, with no overlap");
    CoverageLedger ledger;
    for (size_t i = 0; i <= rig.sink.IndexOf(RadioMessageKind::kFaultCleared); ++i) {
      Apply(ledger, rig.sink.messages[i]);
    }
    Expect(ledger.fault(Radio::kWifi).empty(), "the ledger fault is cleared too");
  }

  // A failed start is a lost, faulted window; the next good window of that radio clears it.
  {
    Rig rig({.ble_enabled = false, .wifi_channels = {1, 2}});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5)};
    CoverageLedger ledger;
    rig.Steps(1);
    for (const auto& message : rig.sink.messages) Apply(ledger, message);
    const auto reports = rig.sink.Reports();
    Expect(reports.at(0).observed_ms == 0 && reports[0].end == WindowEnd::kDriverFault &&
               reports[0].window.planned_ms == 205 && reports[0].completed_ms >= 205,
           "a failed start reports zero observed time and holds the window's time");
    Expect(rig.sink.Count(RadioMessageKind::kWindowStarted) == 0,
           "no window start is announced for a receiver that did not start");
    Expect(ledger.fault(Radio::kWifi) == "start (-5)",
           "the fault reaches the ledger with its reason");
    rig.Steps(1);
    Expect(rig.sink.Reports().size() == 2 && rig.sink.Reports()[1].window.channel == 2 &&
               rig.sink.Reports()[1].end == WindowEnd::kCompleted,
           "scheduling continues to the next channel");
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "one good WiFi window does not clear the fault: the whole plan must run clean");
    rig.Steps(1);
    const size_t cleared = rig.sink.IndexOf(RadioMessageKind::kFaultCleared);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1 &&
               rig.sink.messages[cleared].cleared_radio == Radio::kWifi,
           "a full plan of successful WiFi windows clears the fault");
    for (const auto& message : rig.sink.messages) Apply(ledger, message);
    Expect(ledger.fault(Radio::kWifi).empty(), "the ledger fault is cleared");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5), Fail(FaultOp::kStart, -5)};
    rig.Steps(3);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 1,
           "a persistent start failure raises one fault, not one per window");
  }

  // A control request ends the current window early and keeps its original plan.
  {
    Rig rig;
    ControlRequest request{.wifi_enabled = true, .ble_enabled = false, .wifi_dwell_ms = 100};
    request.SetChannels({6});
    rig.driver.ScheduleControl(100'000, request);
    rig.Steps(3);
    const auto reports = rig.sink.Reports();
    Expect(reports[0].end == WindowEnd::kConfigurationChanged &&
               reports[0].window.planned_ms == 205 && reports[0].observed_ms == 99 &&
               reports[0].config_generation == 1,
           "the early-ended window keeps its original planned time and its generation");
    CoverageLedger ledger;
    Apply(ledger, rig.sink.messages[rig.sink.IndexOf(RadioMessageKind::kWindowReport)]);
    Expect(ledger.gaps().size() == 1 && ledger.gaps()[0].duration_ms == 106,
           "the lost remainder shows as a Coverage Gap");
    size_t applied = rig.sink.IndexOf(RadioMessageKind::kConfigApplied, 1);
    Expect(applied < rig.sink.messages.size() &&
               rig.sink.messages[applied].config.config_generation == 2 &&
               !rig.sink.messages[applied].config.ble_enabled &&
               rig.sink.messages[applied].config.channel_count == 1 &&
               rig.sink.messages[applied].config.channels[0] == 6 &&
               rig.sink.messages[applied].config.wifi_dwell_ms == 100,
           "the applied configuration is announced with a bumped generation");
    Expect(applied > rig.sink.IndexOf(RadioMessageKind::kWindowReport),
           "the old window is reported before the new configuration takes over");
    Expect(reports[1].window.radio == Radio::kWifi && reports[1].window.channel == 6 &&
               reports[1].window.planned_ms == 100 && reports[1].config_generation == 2 &&
               reports[1].window.start_ms == 100 && reports[2].window.channel == 6,
           "the new configuration starts at once and BLE gives its budget to WiFi");
    Expect(rig.driver.overlaps == 0, "no overlap across the change");
  }

  // One radio disabled gives every window to the other.
  {
    Rig rig({.wifi_enabled = false});
    rig.Steps(3);
    bool all_ble = true;
    for (const auto& report : rig.sink.Reports())
      all_ble = all_ble && report.window.radio == Radio::kBle;
    Expect(all_ble && rig.sink.Reports().size() == 3 && rig.driver.starts[0] == 0,
           "BLE gets every window when WiFi is disabled");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.Steps(3);
    const auto reports = rig.sink.Reports();
    Expect(rig.driver.starts[1] == 0 && reports[0].window.channel == 1 &&
               reports[1].window.channel == 2 && reports[2].window.channel == 3 &&
               reports[1].window.start_ms == 205,
           "WiFi gets every window back to back when BLE is disabled");
  }

  // Both radios disabled: no windows; the runner sleeps until a control arrives.
  {
    Rig rig({.wifi_enabled = false, .ble_enabled = false});
    rig.driver.ScheduleControl(5'000'000, {.wifi_enabled = true, .ble_enabled = false});
    rig.Steps(1);
    Expect(rig.driver.waits.size() == 1 && rig.driver.waits[0] == kForever &&
               rig.sink.Count(RadioMessageKind::kWindowStarted) == 0 &&
               rig.driver.starts[0] + rig.driver.starts[1] == 0,
           "with both radios off the runner blocks and starts nothing");
    Expect(rig.sink.Count(RadioMessageKind::kConfigApplied) == 2,
           "the control that ends the wait is applied in the same step");
    rig.Steps(1);
    Expect(rig.sink.messages.back().kind == RadioMessageKind::kWindowReport, "observation resumes");
    Expect(rig.sink.Reports().at(0).window.start_ms == 5000 &&
               rig.sink.Reports()[0].observed_ms == 204,
           "the first window after a block starts now, not at a stale deadline");
  }
  {
    Rig rig({.wifi_enabled = false, .ble_enabled = false});
    rig.driver.spurious_control_wakes = 2;
    rig.driver.ScheduleControl(1'000'000, {.wifi_enabled = true});
    rig.Steps(1);
    Expect(rig.driver.waits.size() == 3 && rig.driver.waits[2] == kForever &&
               rig.sink.Count(RadioMessageKind::kConfigApplied) == 2,
           "empty control wakes do not end the wait; only a real control does");
  }

  // Dropped events are charged to the window they belong to.
  {
    Rig rig;
    rig.driver.drops = {{1, 3}, {3, 7}};
    rig.driver.unattributed = 4;  // folded out of the platform's tally
    rig.Steps(3);
    const auto reports = rig.sink.Reports();
    Expect(reports[0].dropped_events == 7 && reports[1].dropped_events == 0 &&
               reports[2].dropped_events == 7,
           "each report carries the drops of its own window");
  }

  // Exactly half observed is eligible; one millisecond less is a gap.
  {
    Rig rig({.ble_enabled = false, .wifi_dwell_ms = 200});
    rig.driver.start_wifi_cost_us = 100'000;
    rig.Steps(1);
    rig.driver.start_wifi_cost_us = 100'001;
    rig.Steps(1);
    const auto reports = rig.sink.Reports();
    CoverageLedger ledger;
    for (const auto& message : rig.sink.messages) Apply(ledger, message);
    Expect(reports[0].observed_ms == 100 && reports[1].observed_ms == 99,
           "half and just under half of a 200 ms window");
    Expect(ledger.gaps().size() == 1 && ledger.gaps()[0].start_ms == reports[1].window.start_ms,
           "only the window under half is a gap");
  }

  // The receive interval is clipped to the planned window, and µs convert to ms once.
  {
    Rig rig({.ble_enabled = false});
    rig.driver.wake_latency_us = 999;
    rig.driver.start_wifi_cost_us = 1999;  // 204.001 ms of a 205 ms window
    rig.Steps(1);
    Expect(rig.sink.Reports().at(0).observed_ms == 203,
           "203.001 ms observed rounds down once, and the 999 us late wake adds nothing");
  }

  // Item 1: drops are charged to the window that experienced the overload, and
  // across many windows every eviction reaches exactly one report.
  for (const bool drain : {true, false}) {
    RingRig rig(drain);
    for (int i = 0; i < 30; ++i) rig.runner.Step();
    uint64_t reported = 0;
    bool per_window = true;
    for (const auto& report : rig.sink.Reports()) {
      reported += report.dropped_events;
      const size_t burst = RingDriver::Burst(report.window_id);
      if (drain) {
        const size_t expected = burst > RingDriver::kRingSize ? burst - RingDriver::kRingSize : 0;
        per_window = per_window && report.dropped_events == expected;
      }
    }
    Expect(rig.driver.ring.dropped() > 0 && reported == rig.driver.ring.dropped(),
           drain ? "with a draining consumer the reports' drops sum to the ring's drops"
                 : "with a consumer that never drains the reports' drops still sum to the ring's");
    if (drain) {
      Expect(per_window, "each window reports exactly the overflow of its own burst");
    }
  }

  // Item 2: the platform is told when a window's receiver is on and when its
  // measured interval ends, in the order that keeps late frames outside-window.
  {
    Rig rig({.ble_enabled = false});
    rig.Steps(1);
    Expect(rig.driver.log ==
               std::vector<std::string>({"start wifi", "begin", "end", "stop wifi", "drain"}),
           "start, then begin; end before stop; drain after stop");
    const auto& begin = rig.driver.begun.at(0);
    Expect(begin.window_id == 1 && begin.start_us == 800 && begin.deadline_us == 205'000 &&
               begin.called_at_us == 800,
           "the window opens at the measured receiver start and closes at the planned deadline");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5)};
    rig.Steps(1);
    Expect(rig.driver.begun.empty() && rig.driver.ends == 0,
           "no window is opened for a receiver that did not start");
    Expect(rig.driver.log == std::vector<std::string>({"start wifi", "stop wifi"}),
           "a failed start is followed by a stop");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -3}, {false, FaultOp::kStop, -3}};
    rig.Steps(1);
    const auto it = std::find(rig.driver.log.begin(), rig.driver.log.end(), "stop wifi");
    Expect(rig.driver.ends == 1 && std::find(rig.driver.log.begin(), it, "end") != it,
           "the window is ended before a stop that then fails: late frames are outside-window");
    rig.Steps(1);
    Expect(rig.driver.begun.size() == 1 && rig.driver.drains == 1,
           "no window opens, and nothing drains, while the stop keeps failing");
    rig.Steps(1);
    Expect(rig.driver.drains == 2, "a successful stop retry drains the receiver's events");
    // The retry waited at least the backoff after the failed stop.
    Expect(rig.driver.stop_times_us.size() == 3 &&
               rig.driver.stop_times_us[1] - rig.driver.stop_times_us[0] >=
                   RadioRunner::kStopRetryBackoffUs &&
               rig.driver.stop_times_us[2] - rig.driver.stop_times_us[1] >=
                   RadioRunner::kStopRetryBackoffUs,
           "stop retries are at least the backoff apart");
  }

  // Item 3 in the runner: a window cut short is not eligible for quiet.
  {
    Rig rig;
    rig.driver.ScheduleControl(150'000,
                               {.wifi_enabled = true, .ble_enabled = true, .wifi_dwell_ms = 100});
    rig.Steps(1);
    const auto reports = rig.sink.Reports();
    Expect(reports.at(0).observed_ms * 2 >= reports[0].window.planned_ms &&
               !EligibleForQuiet(reports[0]),
           "an early-ended window is not eligible even when it observed over half");
  }

  // Item 4: a controls-only channel change leaves BLE as it was.
  {
    Rig rig({.ble_enabled = false});
    ControlRequest request;
    request.SetChannels({4, 5});
    rig.control.pending.push_back(request);
    rig.Steps(2);
    const auto reports = rig.sink.Reports();
    Expect(!rig.runner.config().ble_enabled && rig.driver.starts[1] == 0 &&
               reports.at(0).window.channel == 4 && reports.at(1).window.channel == 5,
           "a channels-only control keeps BLE disabled");
    ControlRequest enable;
    enable.ble_enabled = true;
    rig.control.pending.push_back(enable);
    rig.Steps(1);
    Expect(rig.runner.config().ble_enabled && rig.runner.config().wifi_channels.size() == 2,
           "an enable-only control keeps the channel plan");
  }

  // Item 5: a failed start may have enabled receive, so it is stopped; if it will
  // not stop, nothing else starts.
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kSetChannel, -4, /*leaves_on=*/true)};
    rig.Steps(3);
    Expect(rig.driver.stops[0] >= 1 && rig.driver.double_starts == 0,
           "a failed start that left receive on is stopped before the next start");
  }
  {
    Rig rig;
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5, /*leaves_on=*/true)};
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -9}};
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 2 && rig.driver.starts[1] == 0,
           "a failed start and then a failed stop raise both faults and start nothing");
    rig.Steps(1);  // the retry works
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1 && rig.driver.starts[1] == 0,
           "the stuck stop clears first");
    rig.Steps(1);
    Expect(rig.driver.starts[1] == 1 && rig.driver.overlaps == 0 && rig.driver.double_starts == 0,
           "scheduling resumes with no overlap");
  }

  // Item 6: waiting.
  {
    Rig rig({.ble_enabled = false});
    rig.driver.max_wait_advance_us = 60'000;
    rig.Steps(1);
    const auto report = rig.sink.Reports().at(0);
    Expect(rig.driver.waits.size() >= 4 && report.observed_ms == 204 && report.completed_ms >= 205,
           "a wait that returns kDeadline early is repeated until the deadline");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.spurious_control_wakes = 3;
    rig.Steps(1);
    const auto report = rig.sink.Reports().at(0);
    Expect(report.end == WindowEnd::kCompleted && report.observed_ms == 204 &&
               rig.sink.Count(RadioMessageKind::kConfigApplied) == 1,
           "a control wake with nothing to poll keeps waiting");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.Steps(1);
    rig.driver.now_us -= 3000;  // a clock that steps back, or a start planned ahead
    rig.Steps(1);
    Expect(rig.driver.begun.at(1).start_us >= 205'200 + 800,
           "a window never starts before its planned start");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -3}};
    rig.Steps(1);
    rig.driver.immediate_returns = 5;  // waits that return without waiting
    rig.Steps(1);
    Expect(
        rig.driver.StopCalls() == 2 && rig.driver.stop_times_us[1] - rig.driver.stop_times_us[0] >=
                                           RadioRunner::kStopRetryBackoffUs,
        "the stop is not retried before the backoff has elapsed");
  }

  // Item 7: fault reporting.
  {
    Rig rig({.ble_enabled = false, .wifi_channels = {1, 13}});
    rig.driver.start_results[0] = {Ok(), Fail(FaultOp::kSetChannel, -7),
                                   Ok(), Fail(FaultOp::kSetChannel, -7),
                                   Ok(), Fail(FaultOp::kSetChannel, -7)};
    rig.Steps(6);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 1 &&
               rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "one bad channel keeps WiFi faulted: no fault or clear flapping");
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "one good window of a two-channel plan does not clear");
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "the fault clears after one clean pass over the plan");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kSetChannel, -1), Fail(FaultOp::kStart, -1),
                                   Fail(FaultOp::kSetChannel, -1)};
    rig.Steps(3);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 3,
           "each change of failing step is reported");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5), Fail(FaultOp::kStart, -5),
                                   Fail(FaultOp::kStart, -6)};
    rig.Steps(3);
    const size_t last =
        rig.sink.IndexOf(RadioMessageKind::kFault, rig.sink.IndexOf(RadioMessageKind::kFault) + 1);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 2 && last < rig.sink.messages.size() &&
               rig.sink.messages[last].fault.code == -6,
           "a change of code while the fault is active is reported");
  }
  {
    Rig rig({.wifi_enabled = false});
    rig.driver.start_results[1] = {Fail(FaultOp::kStart, -5)};
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFault) == 1 &&
               rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "a BLE start failure raises a fault");
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "one successful BLE window clears it");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5)};
    rig.Steps(1);
    rig.control.pending.push_back({.wifi_enabled = false, .ble_enabled = true});
    rig.Steps(1);
    const size_t applied = rig.sink.IndexOf(RadioMessageKind::kConfigApplied, 1);
    const size_t cleared = rig.sink.IndexOf(RadioMessageKind::kFaultCleared);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1 && cleared > applied &&
               rig.sink.messages[cleared].cleared_radio == Radio::kWifi,
           "disabling a faulted radio clears its fault");
    rig.control.pending.push_back({.wifi_enabled = true});
    rig.Steps(2);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "re-enabling it does not clear a second time");
  }
  {
    Rig rig({.ble_enabled = false});
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -3}, {false, FaultOp::kStop, -3}};
    rig.Steps(1);
    rig.control.pending.push_back({.wifi_enabled = false, .ble_enabled = true});
    rig.Steps(1);  // the control applies; the retried stop fails again
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 0 && rig.driver.starts[1] == 0,
           "disabling a radio whose receiver is stuck on keeps its fault");
    rig.Steps(1);  // the retry works
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "the stuck radio's fault clears once its stop succeeds");
  }

  // A step cannot spin forever on wakes that do not move the clock.
  {
    // Frozen clock, and a wait that returns at once with nothing to report.
    class FrozenDriver : public FakeDriver {
     public:
      using FakeDriver::FakeDriver;
      WakeReason WaitUntilUs(uint64_t) override {
        if (++wakes > 100'000) throw std::runtime_error("runaway wait");
        return WakeReason::kDeadline;
      }
      int wakes = 0;
    };
    FakeControl control;
    FrozenDriver driver(control);
    driver.start_wifi_cost_us = 0;
    driver.stop_cost_us = 0;
    FakeSink sink;
    RadioRunner runner(driver, control, sink, ObservationConfig{.ble_enabled = false});
    runner.Step();
    Expect(driver.wakes > 0 && driver.wakes <= 200,
           "a frozen clock with immediate waits ends the step after a bounded number of wakes");
  }
  {
    // Every wake is a control wake, and every request equals the current config.
    class ConstantControl : public FakeControl {
     public:
      std::optional<ControlRequest> Poll() override { return ControlRequest{.wifi_enabled = true}; }
    };
    class ControlWakeDriver : public FakeDriver {
     public:
      using FakeDriver::FakeDriver;
      WakeReason WaitUntilUs(uint64_t) override {
        if (++wakes > 100'000) throw std::runtime_error("runaway wait");
        return WakeReason::kControl;
      }
      int wakes = 0;
    };
    ConstantControl control;
    ControlWakeDriver driver(control);
    driver.start_wifi_cost_us = 0;
    driver.stop_cost_us = 0;
    FakeSink sink;
    RadioRunner runner(driver, control, sink, ObservationConfig{.ble_enabled = false});
    runner.Step();
    Expect(driver.wakes > 0 && driver.wakes <= 200 && runner.config_generation() == 1,
           "constant equal-config control wakes cannot hold a step forever");
  }
  {
    // A wake that moves the clock resets the count: 5 x 60 stalled wakes is fine.
    Rig rig({.ble_enabled = false});
    rig.driver.immediate_returns = 60;
    rig.Steps(1);
    Expect(rig.sink.Reports().at(0).end == WindowEnd::kCompleted &&
               rig.sink.Reports()[0].observed_ms == 204,
           "a few stalled wakes before the clock moves do not end the window early");
  }

  // WiFi fault clearing counts only completed windows that observed something.
  {
    Rig rig({.ble_enabled = false});
    rig.driver.start_results[0] = {Fail(FaultOp::kSetChannel, -7)};
    rig.Steps(1);
    rig.Steps(10);  // 10 clean windows of an 11-channel plan
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "10 clean windows do not clear an 11-channel plan");
    ControlRequest plan;
    plan.SetChannels({1, 2, 3});
    rig.control.pending.push_back(plan);
    rig.Steps(2);  // the first of these applies the new plan and runs its first window
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "a plan change restarts the clean pass");
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "three clean completed windows of the new plan clear the fault");
  }
  {
    Rig rig({.ble_enabled = false, .wifi_channels = {1, 2, 3}});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5)};
    rig.Steps(1);
    rig.Steps(2);  // two clean windows
    rig.driver.ScheduleControl(rig.driver.now_us + 100'000, {.wifi_dwell_ms = 150});
    rig.Steps(1);  // cut short by a configuration change
    Expect(rig.sink.Reports().back().end == WindowEnd::kConfigurationChanged &&
               rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "a window cut short by a configuration change does not count as clean");
    rig.Steps(1);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "the next completed window finishes the clean pass");
  }
  {
    Rig rig({.ble_enabled = false, .wifi_channels = {1, 2, 3}});
    rig.driver.start_results[0] = {Fail(FaultOp::kStart, -5)};
    rig.Steps(1);
    rig.driver.start_wifi_cost_us = 210'000;  // receive starts after the window is over
    rig.Steps(3);
    Expect(rig.sink.Reports().back().observed_ms == 0 &&
               rig.sink.Count(RadioMessageKind::kFaultCleared) == 0,
           "windows that observed nothing do not clear a fault");
    rig.driver.start_wifi_cost_us = 800;
    rig.Steps(3);
    Expect(rig.sink.Count(RadioMessageKind::kFaultCleared) == 1,
           "three windows that observed something do");
  }

  // Drops taken by a successful stop retry reach the next report.
  {
    Rig rig({.ble_enabled = false});
    rig.driver.stop_results[0] = {{false, FaultOp::kStop, -3}};
    rig.Steps(1);
    rig.driver.unattributed = 3;  // the platform pushed after EndWindow
    rig.Steps(1);                 // the retry works and drains
    Expect(rig.driver.unattributed == 0, "the stop retry takes the unattributed drops");
    rig.Steps(1);
    Expect(rig.sink.Reports().back().dropped_events == 3,
           "they are carried into the next window's dropped_events");
    rig.Steps(1);
    Expect(rig.sink.Reports().back().dropped_events == 0, "and only once");
  }

  // Item 8: controls.
  {
    Rig rig({.ble_enabled = false});
    rig.Steps(1);
    rig.driver.now_us += 10'000'000;  // the radio task was idle, so the old plan is stale
    rig.control.pending.push_back({.wifi_dwell_ms = 100});
    rig.Steps(1);
    const auto reports = rig.sink.Reports();
    Expect(reports.at(1).window.start_ms >= 10'000 && reports[1].observed_ms == 99,
           "a control never plans the next window in the past");
  }
  {
    Rig rig;
    rig.driver.ScheduleControl(100'000, {.wifi_enabled = true, .ble_enabled = true});
    rig.Steps(1);
    const auto report = rig.sink.Reports().at(0);
    Expect(report.end == WindowEnd::kCompleted && report.observed_ms == 204 &&
               rig.runner.config_generation() == 1 &&
               rig.sink.Count(RadioMessageKind::kConfigApplied) == 1,
           "a control that changes nothing leaves the window and the generation alone");
    ControlRequest same;
    same.SetChannels({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 99});  // normalises to the default plan
    same.wifi_dwell_ms = 205;
    rig.control.pending.push_back(same);
    rig.Steps(1);
    Expect(rig.runner.config_generation() == 1 &&
               rig.sink.Count(RadioMessageKind::kConfigApplied) == 1,
           "a request equal to the current configuration once normalised is ignored");
  }
  {
    Rig rig;
    rig.Steps(2);  // WiFi ch1, BLE
    rig.control.pending.push_back({.ble_enabled = false});
    rig.Steps(1);
    Expect(rig.sink.Reports().at(2).window.channel == 2,
           "disabling BLE keeps the WiFi walk where it was");
  }

  if (failures == 0) {
    std::printf("firmware_domain_radio_runner_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_radio_runner_tests: %d failure(s)\n", failures);
  return 1;
}
