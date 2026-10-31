// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace firmware::domain {

// Passive 2.4 GHz observation is scheduled in explicit windows (ADR 0003) and
// this module accounts for what actually happened in them. Requested window time
// is never treated as receive time: only time the driver reports as observed
// counts as observed, so a scheduled-but-unreported interval can never read as
// observed quiet. Pure C++; the ESP32 radio driver and the simulator feed it
// the same reports.

enum class Radio : uint8_t { kWifi, kBle };

// Candidate defaults, from F08's channel-hop and BLE-toggle measurements:
// dwell is two 802.11 beacon periods (2 x 102.4 ms), the US channel range is
// 1-11, and a BLE window matches one WiFi dwell so the two radios share the
// budget equally when both are enabled. Numeric values stay candidates until
// M5 measures real coverage.
inline constexpr uint32_t kDefaultWifiDwellMs = 205;
inline constexpr uint32_t kDefaultBleWindowMs = 205;
inline constexpr uint8_t kDefaultWifiChannelFirst = 1;
inline constexpr uint8_t kDefaultWifiChannelLast = 11;
// 2.4 GHz WiFi channels a plan may name, and the longest window the scheduler
// accepts; anything else is normalised away rather than trusted.
inline constexpr uint8_t kMaxWifiChannel = 14;
inline constexpr uint32_t kMaxWindowMs = 60'000;

struct ObservationConfig {
  bool wifi_enabled = true;
  bool ble_enabled = true;
  std::vector<uint8_t> wifi_channels;  // empty means the default 1-11 plan
  uint32_t wifi_dwell_ms = kDefaultWifiDwellMs;
  uint32_t ble_window_ms = kDefaultBleWindowMs;
};

// The configuration actually in force: channels limited to 1-14, duplicates
// dropped (order kept), the default plan when none remain, and each window
// defaulted when zero and capped at kMaxWindowMs.
ObservationConfig NormalizeConfig(ObservationConfig config);

// One requested Observation Window. `channel` is 0 for BLE.
struct Window {
  Radio radio = Radio::kWifi;
  uint8_t channel = 0;
  uint64_t start_ms = 0;
  uint32_t planned_ms = 0;
};

// Produces the window sequence. Both radios enabled alternate WiFi (next
// channel) and BLE with equal window time; one enabled gets every window;
// none enabled yields no windows (Host-only monitoring). Reconfiguring
// restarts the sequence, so a disabled radio's budget passes to the other at
// the next window.
class ObservationScheduler {
 public:
  explicit ObservationScheduler(ObservationConfig config = {});
  void Configure(ObservationConfig config);
  // The effective (normalised) configuration; wifi_channels is never empty.
  const ObservationConfig& config() const { return config_; }
  // The effective channel plan, the same list as config().wifi_channels.
  const std::vector<uint8_t>& channels() const { return config_.wifi_channels; }
  // The window that starts at `start_ms`; nullopt when both radios are disabled.
  std::optional<Window> NextWindow(uint64_t start_ms);
  // Time for one WiFi channel to come round again under the current config
  // (0 when WiFi is disabled).
  uint32_t WifiRevisitMs() const;

 private:
  ObservationConfig config_;
  size_t next_channel_ = 0;
  Radio next_radio_ = Radio::kWifi;
};

// Why a window ended.
enum class WindowEnd : uint8_t {
  kCompleted,             // ran to its deadline
  kConfigurationChanged,  // ended early because the operator changed the schedule
  kDriverFault,           // the receiver failed to start or stop
};

// What the driver reports when a window ends. Trivially copyable: it crosses
// the radio task to app loop queue by value.
struct WindowReport {
  Window window;
  uint32_t observed_ms = 0;     // receiver actually available; <= planned_ms
  uint32_t dropped_events = 0;  // events lost to overload in this window
  uint32_t window_id = 0;       // monotonic per boot; events carry the same id
  uint32_t config_generation = 0;
  uint64_t completed_ms = 0;  // when the window actually finished, not start + planned
  WindowEnd end = WindowEnd::kCompleted;
};

// Whether a window may advance an observed-quiet recovery clock
// (docs/alert-rules.md 1.1, 1.5): it ran to its deadline (not cut short by a
// configuration change or a driver fault), observed at least half its planned
// time, and lost no events. A window with drops still counts as observed, but
// its event counts are a lower bound, so it is never evidence of quiet.
inline bool EligibleForQuiet(const WindowReport& report) {
  return report.end == WindowEnd::kCompleted &&
         uint64_t{report.observed_ms} * 2 >= report.window.planned_ms && report.dropped_events == 0;
}

// A report for a window that ran to its deadline on schedule, for scripted
// drivers (the simulator and tests); the real driver stamps its own times.
inline WindowReport CompletedReport(const Window& window, uint32_t observed_ms,
                                    uint32_t dropped_events = 0) {
  WindowReport report;
  report.window = window;
  report.observed_ms = observed_ms;
  report.dropped_events = dropped_events;
  report.completed_ms = window.start_ms + window.planned_ms;
  return report;
}

// Fixed-capacity queue that drops the oldest event on overflow and counts the
// drops (docs/extension-interfaces.md, Radio observation).
template <typename T, size_t N>
class BoundedEventQueue {
  static_assert(N > 0, "a zero-capacity queue would divide by zero");

 public:
  void Push(T event) {
    if (size_ == N) {
      head_ = (head_ + 1) % N;
      --size_;
      ++dropped_;
    }
    items_[(head_ + size_) % N] = std::move(event);
    ++size_;
  }
  std::optional<T> Pop() {
    if (size_ == 0) return std::nullopt;
    T event = std::move(items_[head_]);
    head_ = (head_ + 1) % N;
    --size_;
    return event;
  }
  size_t size() const { return size_; }
  uint64_t dropped() const { return dropped_; }

 private:
  std::array<T, N> items_{};
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t dropped_ = 0;
};

struct CoverageGap {
  Radio radio = Radio::kWifi;
  uint64_t start_ms = 0;
  uint32_t duration_ms = 0;
};

struct RadioTotals {
  uint64_t planned_ms = 0;
  uint64_t observed_ms = 0;
  uint64_t dropped_events = 0;
  uint32_t windows = 0;
  uint8_t last_channel = 0;
  std::optional<uint64_t> last_report_ms;  // completion time of the most recent reported window
  uint32_t last_planned_ms = 0;
  uint32_t last_observed_ms = 0;
};

// Bounded coverage accounting. Keeps per-radio totals and the most recent
// gaps; older gaps roll off, but their time stays in the totals. A Coverage
// Gap is a window that observed less than half its planned time (exactly half
// is not a gap); its partial observed time stays in the totals, and its
// `duration_ms` is the unavailable time, planned minus observed. Smaller
// losses only reduce the observed share and add no gap.
class CoverageLedger {
 public:
  static constexpr size_t kMaxGaps = 16;
  void Record(const WindowReport& report);
  // A radio's driver failed to initialise or run; surfaced for later
  // Operational Alert integration. Cleared by ClearFault.
  void SetFault(Radio radio, std::string reason);
  void ClearFault(Radio radio);
  const RadioTotals& totals(Radio radio) const { return totals_[Index(radio)]; }
  const std::string& fault(Radio radio) const { return faults_[Index(radio)]; }
  const std::deque<CoverageGap>& gaps() const { return gaps_; }
  // Total observed time across every reported window, gap windows' partial
  // time included. Display only: observed-quiet recovery must consume eligible
  // per-window reports (per WiFi channel, zero drops), never these radio-wide
  // totals (docs/alert-rules.md 1.1); see EligibleForQuiet. Scheduled time
  // without a report never counts.
  uint64_t observed_ms(Radio radio) const { return totals(radio).observed_ms; }

 private:
  static size_t Index(Radio radio) { return radio == Radio::kWifi ? 0 : 1; }
  std::array<RadioTotals, 2> totals_;
  std::array<std::string, 2> faults_;
  std::deque<CoverageGap> gaps_;
};

enum class RadioState : uint8_t {
  kOperatorDisabled,  // explicit state, not zero activity and not a gap
  kFault,             // initialisation or driver failure
  kNoData,            // enabled but nothing reported yet, or reports gone stale
  kObserving,         // recent windows reported observed time
  kGap,               // the latest window observed less than half its planned time
};

struct RadioCoverageView {
  RadioState state = RadioState::kNoData;
  std::string note;                // fault reason, or empty
  uint32_t observed_permille = 0;  // observed / planned over the totals
  uint64_t dropped_events = 0;
  uint8_t last_channel = 0;
};

struct CoverageView {
  RadioCoverageView wifi;
  RadioCoverageView ble;
  // The radio whose Observation Window is current or most recently reported
  // (fresh reports only); nullopt when neither radio has one.
  std::optional<Radio> current;
  // Both radios disabled: only Host metrics are monitored.
  bool host_only = false;
  uint32_t wifi_revisit_ms = 0;
};

// Reports older than this many expected revisit periods make a radio's state
// kNoData: its coverage is unknown, not quiet.
inline constexpr uint32_t kStaleRevisitPeriods = 3;

// A window whose receiver has started but whose report has not arrived yet.
struct ActiveWindow {
  Radio radio = Radio::kWifi;
  uint8_t channel = 0;  // 0 for BLE
  uint32_t window_id = 0;
  uint64_t rx_start_ms = 0;
  uint32_t planned_ms = 0;
};

// `config` is the effective configuration (the app loop keeps a copy of the
// radio task's kConfigApplied, not a second scheduler). With an `active`
// window, `current` names it and a normally observing radio shows its channel;
// otherwise `current` is the radio with the latest fresh report.
CoverageView MakeCoverageView(const ObservationConfig& config, const CoverageLedger& ledger,
                              uint64_t now_ms, std::optional<ActiveWindow> active = std::nullopt);

CoverageView MakeCoverageView(const ObservationScheduler& scheduler, const CoverageLedger& ledger,
                              uint64_t now_ms);

// "wifi=<state>[(note)] <permille>/1000 drop=<n> ch=<c> | ble=... | host_only=<0|1>"
// for replay checkpoints, e.g. "wifi=fault(init failed) 928/1000 drop=2 ch=3".
std::string FormatCoverage(const CoverageView& view);

// Compact persistent-status text: the current Observation Window plus each
// radio's problem state, e.g. "WiFi ch6", "BLE scan", "WiFi ch6 BLE gap",
// "WiFi ? BLE off", "Host only". A radio observing normally is named only while
// its window is the current one; `gap` (a lost window), `?` (unknown, never
// quiet), `off` and `err` (a faulted radio; "fault" does not fit the bar) are
// always shown. No percentage: an observed share
// of window time is not capture coverage of any channel.
std::string CoverageStatusText(const CoverageView& view);

}  // namespace firmware::domain
