// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/radio_runner.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace firmware::domain {

namespace {

constexpr uint64_t kUsPerMs = 1000;
constexpr uint64_t kForeverUs = std::numeric_limits<uint64_t>::max();
// A bound on one drain of the control source, so a flooding source cannot
// hold a step forever.
constexpr int kMaxControlsPerStep = 16;
// Consecutive wakes that leave the clock where it was before a wait gives up
// and lets the step return (so the next step polls the controls afresh).
constexpr int kMaxStalledWakes = 64;

uint64_t SaturatingAdd(uint64_t a, uint64_t b) { return b > kForeverUs - a ? kForeverUs : a + b; }

uint32_t SaturatingAdd32(uint32_t a, uint32_t b) { return b > UINT32_MAX - a ? UINT32_MAX : a + b; }

bool SameConfig(const ObservationConfig& a, const ObservationConfig& b) {
  return a.wifi_enabled == b.wifi_enabled && a.ble_enabled == b.ble_enabled &&
         a.wifi_channels == b.wifi_channels && a.wifi_dwell_ms == b.wifi_dwell_ms &&
         a.ble_window_ms == b.ble_window_ms;
}

// The configuration `request` would produce from `config`, normalised.
ObservationConfig Merged(ObservationConfig config, const ControlRequest& request) {
  if (request.wifi_enabled) config.wifi_enabled = *request.wifi_enabled;
  if (request.ble_enabled) config.ble_enabled = *request.ble_enabled;
  if (request.has_channels) {
    const size_t count = std::min<size_t>(request.channel_count, kMaxPlanChannels);
    config.wifi_channels.assign(request.channels, request.channels + count);
  }
  if (request.wifi_dwell_ms) config.wifi_dwell_ms = *request.wifi_dwell_ms;
  if (request.ble_window_ms) config.ble_window_ms = *request.ble_window_ms;
  return NormalizeConfig(std::move(config));
}

}  // namespace

RadioRunner::RadioRunner(RadioDriver& driver, ControlSource& control, RadioSink& sink,
                         ObservationConfig config)
    : driver_(driver), control_(control), sink_(sink), scheduler_(std::move(config)) {}

void RadioRunner::EmitConfig() {
  sink_.Emit(RadioMessage::ConfigApplied(ToConfigApplied(config_generation_, scheduler_.config())));
}

bool RadioRunner::ApplyControl(const ControlRequest& request) {
  ObservationConfig merged = Merged(scheduler_.config(), request);
  if (SameConfig(merged, scheduler_.config())) return false;
  const ObservationConfig& before = scheduler_.config();
  // A new plan or an enable change restarts the radio's clean pass.
  if (merged.wifi_enabled != before.wifi_enabled || merged.wifi_channels != before.wifi_channels) {
    clean_windows_[Index(Radio::kWifi)] = 0;
  }
  if (merged.ble_enabled != before.ble_enabled) clean_windows_[Index(Radio::kBle)] = 0;
  scheduler_.Configure(std::move(merged));
  ++config_generation_;
  next_start_us_.reset();
  EmitConfig();
  // A disabled radio's fault clears, unless its receiver is still stuck on: that
  // fault stands until the stop retry succeeds.
  if (!scheduler_.config().wifi_enabled && stuck_ != Radio::kWifi) ClearFault(Radio::kWifi);
  if (!scheduler_.config().ble_enabled && stuck_ != Radio::kBle) ClearFault(Radio::kBle);
  return true;
}

std::optional<ControlRequest> RadioRunner::WaitUntil(uint64_t deadline_us) {
  uint64_t last_now_us = driver_.NowUs();
  int stalled_wakes = 0;
  while (last_now_us < deadline_us) {
    if (driver_.WaitUntilUs(deadline_us) == WakeReason::kControl) {
      const std::optional<ControlRequest> request = control_.Poll();
      if (request && !SameConfig(Merged(scheduler_.config(), *request), scheduler_.config())) {
        return request;
      }
    }
    const uint64_t now_us = driver_.NowUs();
    if (now_us > last_now_us) {
      last_now_us = now_us;
      stalled_wakes = 0;
    } else if (++stalled_wakes > kMaxStalledWakes) {
      return std::nullopt;  // the clock is not moving: return, the caller re-checks it
    }
  }
  return std::nullopt;
}

DriverResult RadioRunner::StopReceiver(Radio radio) {
  const DriverResult result = radio == Radio::kWifi ? driver_.StopWifi() : driver_.StopBle();
  if (result.ok) running_[Index(radio)] = false;
  return result;
}

void RadioRunner::NoteFault(Radio radio, const DriverResult& result) {
  const size_t i = Index(radio);
  clean_windows_[i] = 0;
  if (fault_active_[i] && fault_op_[i] == result.op && fault_code_[i] == result.code) return;
  fault_active_[i] = true;
  fault_op_[i] = result.op;
  fault_code_[i] = result.code;
  sink_.Emit(RadioMessage::Fault(radio, result.op, result.code));
}

void RadioRunner::NoteStopFailure(Radio radio, const DriverResult& result) {
  stuck_ = radio;
  stop_retry_at_us_ = SaturatingAdd(driver_.NowUs(), kStopRetryBackoffUs);
  next_start_us_.reset();
  NoteFault(radio, result);
}

void RadioRunner::ClearFault(Radio radio) {
  const size_t i = Index(radio);
  clean_windows_[i] = 0;
  if (!fault_active_[i]) return;
  fault_active_[i] = false;
  sink_.Emit(RadioMessage::FaultCleared(radio));
}

void RadioRunner::Step() {
  if (!announced_) {
    announced_ = true;
    ++config_generation_;
    EmitConfig();
  }
  for (int i = 0; i < kMaxControlsPerStep; ++i) {
    const std::optional<ControlRequest> request = control_.Poll();
    if (!request) break;
    ApplyControl(*request);
  }
  if (stuck_) {
    RetryStop();
    return;
  }
  const ObservationConfig& config = scheduler_.config();
  if (!config.wifi_enabled && !config.ble_enabled) {
    // Host-only: no window to run. Sleep until the operator changes something.
    next_start_us_.reset();
    if (const auto request = WaitUntil(kForeverUs)) ApplyControl(*request);
    return;
  }
  RunWindow();
}

void RadioRunner::RetryStop() {
  const Radio radio = *stuck_;
  if (const auto request = WaitUntil(stop_retry_at_us_)) {
    ApplyControl(*request);  // the radio stays stuck; the next step comes back here
    return;
  }
  if (driver_.NowUs() < stop_retry_at_us_) return;  // the wait gave up early: not yet
  if (!StopReceiver(radio).ok) {
    stop_retry_at_us_ = SaturatingAdd(driver_.NowUs(), kStopRetryBackoffUs);
    return;
  }
  stuck_.reset();
  driver_.DrainEvents();
  // Anything pushed after EndWindow was charged to no report: carry it forward.
  carried_drops_ = SaturatingAdd32(carried_drops_, driver_.TakeUnattributed());
  next_start_us_.reset();
  ClearFault(radio);
}

void RadioRunner::RunWindow() {
  // The plan is fixed before any preparation, so setup time is spent inside
  // the window and shows up as lost observed time.
  const uint64_t start_us = next_start_us_.value_or(driver_.NowUs());
  if (const auto request = WaitUntil(start_us)) {
    ApplyControl(*request);  // the plan changed under us; replan next step
    return;
  }
  if (driver_.NowUs() < start_us) return;  // the wait gave up early: try again next step
  const Window window = *scheduler_.NextWindow(start_us / kUsPerMs);
  const uint64_t deadline_us = SaturatingAdd(start_us, uint64_t{window.planned_ms} * kUsPerMs);
  const size_t self = Index(window.radio);

  WindowReport report;
  report.window = window;
  report.window_id = ++window_id_;
  report.config_generation = config_generation_;

  bool faulted = false;
  bool started = false;
  uint64_t rx_start_us = 0;
  std::optional<ControlRequest> pending;

  // The other receiver must be off before this one starts.
  const Radio other = Other(window.radio);
  if (running_[Index(other)]) {
    const DriverResult stopped = StopReceiver(other);
    if (!stopped.ok) {
      NoteStopFailure(other, stopped);
      faulted = true;
    }
  }

  if (!faulted) {
    const DriverResult result =
        window.radio == Radio::kWifi ? driver_.StartWifi(window.channel) : driver_.StartBle();
    if (result.ok) {
      started = true;
      running_[self] = true;
      rx_start_us = driver_.NowUs();
      driver_.BeginWindow(report.window_id, report.config_generation, rx_start_us, deadline_us);
      sink_.Emit(
          RadioMessage::WindowStarted({report.window_id, report.config_generation, window.radio,
                                       window.channel, rx_start_us / kUsPerMs, window.planned_ms}));
    } else {
      NoteFault(window.radio, result);
      faulted = true;
      // A failed start may still have enabled receive: make sure it is off.
      running_[self] = true;
      const DriverResult stopped = StopReceiver(window.radio);
      if (!stopped.ok) NoteStopFailure(window.radio, stopped);
    }
  }

  // Hold the window for its full time even when it failed, so a persistent
  // fault cannot spin; a control request ends it early. A receiver that will
  // not stop gets no wait: the runner goes straight to retrying the stop.
  if (!stuck_) pending = WaitUntil(deadline_us);
  // The measured interval closes before the stop is issued.
  const uint64_t rx_end_us = driver_.NowUs();

  if (started) {
    driver_.EndWindow();
    const DriverResult stopped = StopReceiver(window.radio);
    driver_.DrainEvents();
    if (!stopped.ok) {
      NoteStopFailure(window.radio, stopped);
      faulted = true;
    }
  }
  report.dropped_events = SaturatingAdd32(
      SaturatingAdd32(driver_.TakeDrops(report.window_id), driver_.TakeUnattributed()),
      std::exchange(carried_drops_, 0));

  if (started) {
    const uint64_t begin = std::max(rx_start_us, start_us);
    const uint64_t end = std::min(rx_end_us, deadline_us);
    if (end > begin) {
      report.observed_ms = static_cast<uint32_t>((end - begin) / kUsPerMs);
    }
  }
  const uint64_t done_us = driver_.NowUs();
  report.completed_ms = done_us / kUsPerMs;
  report.end = faulted               ? WindowEnd::kDriverFault
               : pending.has_value() ? WindowEnd::kConfigurationChanged
                                     : WindowEnd::kCompleted;

  // A clean window counts toward clearing a fault: WiFi needs a clean pass
  // over its whole channel plan, so one bad channel keeps it faulted.
  if (faulted) {
    clean_windows_[self] = 0;
  } else if (report.end == WindowEnd::kCompleted && report.observed_ms > 0) {
    ++clean_windows_[self];
    const size_t needed = window.radio == Radio::kWifi ? scheduler_.channels().size() : 1;
    if (fault_active_[self] && clean_windows_[self] >= needed) ClearFault(window.radio);
  }
  sink_.Emit(RadioMessage::WindowReported(report));

  if (stuck_) {
    next_start_us_.reset();
  } else {
    next_start_us_ = std::max(deadline_us, done_us);
  }
  if (pending) ApplyControl(*pending);
}

}  // namespace firmware::domain
