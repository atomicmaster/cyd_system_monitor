// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "firmware/domain/coverage.hpp"
#include "firmware/domain/radio_messages.hpp"

namespace firmware::domain {

// The radio task's scheduling loop, with the hardware behind RadioDriver so it
// runs on the host against a fake clock. The runner owns the
// ObservationScheduler and runs no thread of its own: the platform calls
// Step() in a loop. It reports what the receiver actually did, never what was
// requested: observed time is the measured receive interval clipped to the
// planned window, so preparation, late wakes and overruns are lost time, not
// observed time (ADR 0003).

enum class WakeReason : uint8_t { kDeadline, kControl };

struct DriverResult {
  bool ok = true;
  FaultOp op = FaultOp::kStart;  // the failing step, when !ok
  int32_t code = 0;
};

// Hardware seam. All times are the driver's monotonic microseconds.
//
// Window contract. For each window the runner calls, in order: Start (a failed
// Start is followed by a Stop for the same radio, as it may have enabled
// receive), then, only if Start succeeded, BeginWindow; WaitUntilUs; EndWindow
// at the moment the measured interval closes, BEFORE it issues Stop; Stop;
// DrainEvents; TakeDrops and TakeUnattributed. The platform's receive callback
// must push an event only while a window is open and only if its timestamp lies
// in [start_us, deadline_us] of that window; anything else (no open window, or a
// timestamp outside the interval) is an outside-window arrival: it is counted
// by the platform if it likes and never pushed. In particular, frames that
// arrive after EndWindow, such as while a failed Stop is being retried, are
// outside-window.
class RadioDriver {
 public:
  virtual ~RadioDriver() = default;
  virtual uint64_t NowUs() = 0;
  // The receiver of this window is on. `start_us` is when it started
  // (rx_start) and `deadline_us` the planned end; events are stamped with
  // `window_id` and `config_generation`.
  virtual void BeginWindow(uint32_t window_id, uint32_t config_generation, uint64_t start_us,
                           uint64_t deadline_us) {
    (void)window_id;
    (void)config_generation;
    (void)start_us;
    (void)deadline_us;
  }
  // The measured interval is over; events from now on are outside-window.
  virtual void EndWindow() {}
  // Selects the channel and enables promiscuous receive.
  // A failed Start may have left receive enabled; the runner stops it.
  virtual DriverResult StartWifi(uint8_t channel) = 0;
  virtual DriverResult StopWifi() = 0;
  virtual DriverResult StartBle() = 0;
  virtual DriverResult StopBle() = 0;
  // Blocks until `deadline_us` (returning at once if it has passed), or until
  // the control source has something. UINT64_MAX waits for control only. The
  // runner re-checks NowUs() after kDeadline and waits again if the deadline
  // has not been reached (tick-based timeouts can fire early), and after a
  // kControl wake it polls the control source and keeps waiting if it is empty,
  // so the platform must clear its wake trigger when Poll consumes a request.
  virtual WakeReason WaitUntilUs(uint64_t deadline_us) = 0;
  // Moves EVERY event of the window that just closed out of the ring before it
  // returns, so no event of that window can be evicted after its drops are
  // taken. Called after Stop, and after a successful stop retry.
  virtual void DrainEvents() = 0;
  // Events lost to overload while `window_id` was open (the ring charges each
  // eviction to the pushing window); clears the count.
  virtual uint32_t TakeDrops(uint32_t window_id) = 0;
  // Drops that could not be attributed to a window (folded out of the ring's
  // tally); clears the count. The runner adds them to the report being closed.
  virtual uint32_t TakeUnattributed() = 0;
};

// An operator change delivered to the radio task. Plain data, so it can cross a
// task queue by value. Unset fields keep their current value.
struct ControlRequest {
  std::optional<bool> wifi_enabled;
  std::optional<bool> ble_enabled;
  bool has_channels = false;  // channels[0..channel_count) replace the plan
  uint8_t channel_count = 0;
  uint8_t channels[kMaxPlanChannels] = {};
  std::optional<uint32_t> wifi_dwell_ms;
  std::optional<uint32_t> ble_window_ms;

  // Sets the replacement plan, truncated to kMaxPlanChannels entries.
  void SetChannels(const std::vector<uint8_t>& plan) {
    has_channels = true;
    channel_count = static_cast<uint8_t>(std::min(plan.size(), kMaxPlanChannels));
    std::copy_n(plan.begin(), channel_count, channels);
  }
};
static_assert(std::is_trivially_copyable_v<ControlRequest>, "controls cross a task queue by value");

class ControlSource {
 public:
  virtual ~ControlSource() = default;
  // The next pending request, or nullopt. Never blocks.
  virtual std::optional<ControlRequest> Poll() = 0;
};

class RadioSink {
 public:
  virtual ~RadioSink() = default;
  virtual void Emit(const RadioMessage& message) = 0;
};

class RadioRunner {
 public:
  // After a failed stop the runner retries this often, starting no window until it works.
  static constexpr uint64_t kStopRetryBackoffUs = 1'000'000;

  RadioRunner(RadioDriver& driver, ControlSource& control, RadioSink& sink,
              ObservationConfig config = {});

  // One scheduling step: one Observation Window, or one blocked wait (both
  // radios disabled, or a stop being retried). The first step announces the
  // initial configuration as generation 1.
  void Step();

  const ObservationConfig& config() const { return scheduler_.config(); }
  uint32_t config_generation() const { return config_generation_; }
  uint32_t last_window_id() const { return window_id_; }

 private:
  static size_t Index(Radio radio) { return radio == Radio::kWifi ? 0 : 1; }
  static Radio Other(Radio radio) { return radio == Radio::kWifi ? Radio::kBle : Radio::kWifi; }

  // Applies a request unless it changes nothing once merged and normalised;
  // returns whether it did. A change bumps the generation, forgets the planned
  // start (it may lie in the past) and clears the faults of disabled radios.
  bool ApplyControl(const ControlRequest& request);
  void EmitConfig();
  // Waits until NowUs() >= deadline_us, or returns the first control that
  // would change the configuration (not yet applied). Early kDeadline wakes
  // and empty or no-op control wakes keep waiting.
  std::optional<ControlRequest> WaitUntil(uint64_t deadline_us);
  void RunWindow();
  void RetryStop();
  DriverResult StopReceiver(Radio radio);
  // Records a fault, and tells the app loop when it first appears or its
  // (op, code) changes.
  void NoteFault(Radio radio, const DriverResult& result);
  // A receiver that would not stop: fail closed until it does.
  void NoteStopFailure(Radio radio, const DriverResult& result);
  // Ends a fault the app loop was told about.
  void ClearFault(Radio radio);

  RadioDriver& driver_;
  ControlSource& control_;
  RadioSink& sink_;
  ObservationScheduler scheduler_;
  bool announced_ = false;
  uint32_t config_generation_ = 0;
  uint32_t window_id_ = 0;
  // Planned start of the next window; unset means "now" (first window, or
  // after a block, when an old deadline would be meaningless).
  std::optional<uint64_t> next_start_us_;
  std::array<bool, 2> running_{};
  std::array<bool, 2> fault_active_{};
  std::array<FaultOp, 2> fault_op_{};
  std::array<int32_t, 2> fault_code_{};
  // Consecutive clean windows since the radio's last fault; WiFi's fault clears
  // after one clean pass over the channel plan, BLE's after one window.
  std::array<uint32_t, 2> clean_windows_{};
  // Drops a stop retry took after the window that caused them was reported.
  uint32_t carried_drops_ = 0;
  std::optional<Radio> stuck_;  // a receiver whose stop failed
  uint64_t stop_retry_at_us_ = 0;
};

}  // namespace firmware::domain
