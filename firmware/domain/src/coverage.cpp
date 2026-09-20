// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/coverage.hpp"

#include <algorithm>

namespace firmware::domain {

namespace {

std::vector<uint8_t> DefaultChannels() {
  std::vector<uint8_t> channels;
  for (uint8_t c = kDefaultWifiChannelFirst; c <= kDefaultWifiChannelLast; ++c) {
    channels.push_back(c);
  }
  return channels;
}

const char* StateName(RadioState state) {
  switch (state) {
    case RadioState::kOperatorDisabled:
      return "disabled";
    case RadioState::kFault:
      return "fault";
    case RadioState::kNoData:
      return "no-data";
    case RadioState::kObserving:
      return "observing";
    case RadioState::kGap:
      return "gap";
  }
  return "?";
}

}  // namespace

ObservationScheduler::ObservationScheduler(ObservationConfig config) {
  Configure(std::move(config));
}

void ObservationScheduler::Configure(ObservationConfig config) {
  config_ = std::move(config);
  // A zero window would spin the driver; fall back to the candidate default.
  if (config_.wifi_dwell_ms == 0) config_.wifi_dwell_ms = kDefaultWifiDwellMs;
  if (config_.ble_window_ms == 0) config_.ble_window_ms = kDefaultBleWindowMs;
  channels_ = config_.wifi_channels.empty() ? DefaultChannels() : config_.wifi_channels;
  next_channel_ = 0;
  next_radio_ = config_.wifi_enabled ? Radio::kWifi : Radio::kBle;
}

std::optional<Window> ObservationScheduler::NextWindow(uint64_t start_ms) {
  if (!config_.wifi_enabled && !config_.ble_enabled) return std::nullopt;
  Window window;
  window.start_ms = start_ms;
  if (next_radio_ == Radio::kWifi) {
    window.radio = Radio::kWifi;
    window.channel = channels_[next_channel_];
    window.planned_ms = config_.wifi_dwell_ms;
    next_channel_ = (next_channel_ + 1) % channels_.size();
    if (config_.ble_enabled) next_radio_ = Radio::kBle;
  } else {
    window.radio = Radio::kBle;
    window.channel = 0;
    window.planned_ms = config_.ble_window_ms;
    if (config_.wifi_enabled) next_radio_ = Radio::kWifi;
  }
  return window;
}

uint32_t ObservationScheduler::WifiRevisitMs() const {
  if (!config_.wifi_enabled) return 0;
  const uint32_t per_channel =
      config_.wifi_dwell_ms + (config_.ble_enabled ? config_.ble_window_ms : 0);
  return per_channel * static_cast<uint32_t>(channels_.size());
}

void CoverageLedger::Record(const WindowReport& report) {
  RadioTotals& totals = totals_[Index(report.window.radio)];
  const uint32_t observed = std::min(report.observed_ms, report.window.planned_ms);
  totals.planned_ms += report.window.planned_ms;
  totals.observed_ms += observed;
  totals.dropped_events += report.dropped_events;
  ++totals.windows;
  totals.last_channel = report.window.channel;
  totals.last_report_ms = report.window.start_ms + report.window.planned_ms;
  totals.last_planned_ms = report.window.planned_ms;
  totals.last_observed_ms = observed;
  if (observed < report.window.planned_ms) {
    // The unobserved remainder is a Coverage Gap, kept visible rather than
    // folded into quiet time.
    gaps_.push_back({report.window.radio, report.window.start_ms + observed,
                     report.window.planned_ms - observed});
    if (gaps_.size() > kMaxGaps) gaps_.pop_front();
  }
}

void CoverageLedger::SetFault(Radio radio, std::string reason) {
  faults_[Index(radio)] = std::move(reason);
}

void CoverageLedger::ClearFault(Radio radio) { faults_[Index(radio)].clear(); }

namespace {

RadioCoverageView MakeRadioView(bool enabled, const RadioTotals& totals, const std::string& fault,
                                uint32_t revisit_ms, uint64_t now_ms) {
  RadioCoverageView view;
  if (!enabled) {
    view.state = RadioState::kOperatorDisabled;
    return view;
  }
  view.dropped_events = totals.dropped_events;
  view.last_channel = totals.last_channel;
  if (totals.planned_ms > 0) {
    view.observed_permille = static_cast<uint32_t>(totals.observed_ms * 1000 / totals.planned_ms);
  }
  if (!fault.empty()) {
    view.state = RadioState::kFault;
    view.note = fault;
    return view;
  }
  const bool stale = !totals.last_report_ms.has_value() ||
                     now_ms > *totals.last_report_ms + uint64_t{kStaleRevisitPeriods} * revisit_ms;
  if (stale) {
    view.state = RadioState::kNoData;
    return view;
  }
  // The latest window decides between observing and gap. Sub-millisecond
  // switching overhead is not a gap; losing half the window or more is.
  const bool latest_is_gap = uint64_t{totals.last_observed_ms} * 2 < totals.last_planned_ms;
  view.state = latest_is_gap ? RadioState::kGap : RadioState::kObserving;
  return view;
}

}  // namespace

CoverageView MakeCoverageView(const ObservationScheduler& scheduler, const CoverageLedger& ledger,
                              uint64_t now_ms) {
  const auto& config = scheduler.config();
  CoverageView view;
  view.host_only = !config.wifi_enabled && !config.ble_enabled;
  view.wifi_revisit_ms = scheduler.WifiRevisitMs();
  // A radio's own window repeats every wifi revisit / channels; BLE's window
  // comes round once per WiFi dwell when both are on.
  const uint32_t ble_revisit =
      config.wifi_enabled ? config.wifi_dwell_ms + config.ble_window_ms : config.ble_window_ms;
  view.wifi = MakeRadioView(config.wifi_enabled, ledger.totals(Radio::kWifi),
                            ledger.fault(Radio::kWifi), view.wifi_revisit_ms, now_ms);
  view.ble = MakeRadioView(config.ble_enabled, ledger.totals(Radio::kBle),
                           ledger.fault(Radio::kBle), ble_revisit, now_ms);
  return view;
}

std::string FormatCoverage(const CoverageView& view) {
  const auto radio = [](const char* name, const RadioCoverageView& v) {
    std::string text = std::string(name) + "=" + StateName(v.state);
    if (!v.note.empty()) text += "(" + v.note + ")";
    text += " " + std::to_string(v.observed_permille) +
            "/1000 drop=" + std::to_string(v.dropped_events) +
            " ch=" + std::to_string(v.last_channel);
    return text;
  };
  return radio("wifi", view.wifi) + " | " + radio("ble", view.ble) +
         " | host_only=" + (view.host_only ? "1" : "0");
}

}  // namespace firmware::domain
