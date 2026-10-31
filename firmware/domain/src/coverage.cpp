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

// Observed under half the planned time; exactly half is not a gap.
bool IsGap(uint32_t observed_ms, uint32_t planned_ms) {
  return uint64_t{observed_ms} * 2 < planned_ms;
}

uint32_t NormalizeWindowMs(uint32_t window_ms, uint32_t fallback) {
  // A zero window would spin the driver; fall back to the candidate default.
  return window_ms == 0 ? fallback : std::min(window_ms, kMaxWindowMs);
}

// Time for one WiFi channel to come round again (0 when WiFi is disabled). An
// empty plan counts as the default one. Widened so no input can overflow.
uint32_t WifiRevisitFor(const ObservationConfig& config) {
  if (!config.wifi_enabled) return 0;
  const uint64_t channels = config.wifi_channels.empty()
                                ? kDefaultWifiChannelLast - kDefaultWifiChannelFirst + 1
                                : config.wifi_channels.size();
  const uint64_t per_channel =
      uint64_t{config.wifi_dwell_ms} + (config.ble_enabled ? config.ble_window_ms : 0);
  return static_cast<uint32_t>(std::min<uint64_t>(per_channel * channels, UINT32_MAX));
}

}  // namespace

ObservationConfig NormalizeConfig(ObservationConfig config) {
  std::vector<uint8_t> channels;
  for (const uint8_t channel : config.wifi_channels) {
    if (channel < 1 || channel > kMaxWifiChannel) continue;
    if (std::find(channels.begin(), channels.end(), channel) == channels.end()) {
      channels.push_back(channel);
    }
  }
  config.wifi_channels = channels.empty() ? DefaultChannels() : std::move(channels);
  config.wifi_dwell_ms = NormalizeWindowMs(config.wifi_dwell_ms, kDefaultWifiDwellMs);
  config.ble_window_ms = NormalizeWindowMs(config.ble_window_ms, kDefaultBleWindowMs);
  return config;
}

ObservationScheduler::ObservationScheduler(ObservationConfig config) {
  Configure(std::move(config));
}

void ObservationScheduler::Configure(ObservationConfig config) {
  ObservationConfig next = NormalizeConfig(std::move(config));
  // A change that leaves WiFi's plan alone (a BLE toggle, a dwell change) must
  // not send the walk back to channel 1, or channels late in the plan starve.
  const bool keep_place =
      config_.wifi_enabled && next.wifi_enabled && config_.wifi_channels == next.wifi_channels;
  config_ = std::move(next);
  if (!keep_place) next_channel_ = 0;
  next_radio_ = config_.wifi_enabled ? Radio::kWifi : Radio::kBle;
}

std::optional<Window> ObservationScheduler::NextWindow(uint64_t start_ms) {
  if (!config_.wifi_enabled && !config_.ble_enabled) return std::nullopt;
  Window window;
  window.start_ms = start_ms;
  if (next_radio_ == Radio::kWifi) {
    window.radio = Radio::kWifi;
    window.channel = config_.wifi_channels[next_channel_];
    window.planned_ms = config_.wifi_dwell_ms;
    next_channel_ = (next_channel_ + 1) % config_.wifi_channels.size();
    if (config_.ble_enabled) next_radio_ = Radio::kBle;
  } else {
    window.radio = Radio::kBle;
    window.channel = 0;
    window.planned_ms = config_.ble_window_ms;
    if (config_.wifi_enabled) next_radio_ = Radio::kWifi;
  }
  return window;
}

uint32_t ObservationScheduler::WifiRevisitMs() const { return WifiRevisitFor(config_); }

void CoverageLedger::Record(const WindowReport& report) {
  RadioTotals& totals = totals_[Index(report.window.radio)];
  const uint32_t observed = std::min(report.observed_ms, report.window.planned_ms);
  totals.planned_ms += report.window.planned_ms;
  totals.observed_ms += observed;
  totals.dropped_events += report.dropped_events;
  ++totals.windows;
  totals.last_channel = report.window.channel;
  totals.last_report_ms = report.completed_ms;
  totals.last_planned_ms = report.window.planned_ms;
  totals.last_observed_ms = observed;
  if (IsGap(observed, report.window.planned_ms)) {
    // A window that observed less than half its time is a Coverage Gap, kept
    // visible rather than folded into quiet time. Its duration is the
    // unavailable time; the partial observed time stays in the totals.
    gaps_.push_back(
        {report.window.radio, report.window.start_ms, report.window.planned_ms - observed});
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
  // A subtraction, so extreme times cannot wrap it; a report dated after `now`
  // is not stale.
  const bool stale =
      !totals.last_report_ms.has_value() ||
      (now_ms > *totals.last_report_ms &&
       now_ms - *totals.last_report_ms > uint64_t{kStaleRevisitPeriods} * revisit_ms);
  if (stale) {
    view.state = RadioState::kNoData;
    return view;
  }
  // The latest window decides between observing and gap. Switching overhead is
  // not a gap; observing less than half the window is.
  view.state = IsGap(totals.last_observed_ms, totals.last_planned_ms) ? RadioState::kGap
                                                                      : RadioState::kObserving;
  return view;
}

}  // namespace

CoverageView MakeCoverageView(const ObservationConfig& given, const CoverageLedger& ledger,
                              uint64_t now_ms, std::optional<ActiveWindow> active) {
  const ObservationConfig config = NormalizeConfig(given);
  CoverageView view;
  view.host_only = !config.wifi_enabled && !config.ble_enabled;
  view.wifi_revisit_ms = WifiRevisitFor(config);
  // A radio's own window repeats every wifi revisit / channels; BLE's window
  // comes round once per WiFi dwell when both are on.
  const uint32_t ble_revisit = static_cast<uint32_t>(
      std::min<uint64_t>(config.wifi_enabled ? uint64_t{config.wifi_dwell_ms} + config.ble_window_ms
                                             : config.ble_window_ms,
                         UINT32_MAX));
  view.wifi = MakeRadioView(config.wifi_enabled, ledger.totals(Radio::kWifi),
                            ledger.fault(Radio::kWifi), view.wifi_revisit_ms, now_ms);
  view.ble = MakeRadioView(config.ble_enabled, ledger.totals(Radio::kBle),
                           ledger.fault(Radio::kBle), ble_revisit, now_ms);
  if (active) {
    // An active window past its planned end, or one the ledger already holds a
    // later report for, is stale news from the app loop: ignore it.
    const uint64_t planned_end = active->rx_start_ms + active->planned_ms < active->rx_start_ms
                                     ? UINT64_MAX
                                     : active->rx_start_ms + active->planned_ms;
    const auto& last = ledger.totals(active->radio).last_report_ms;
    if (now_ms > planned_end || (last && *last > active->rx_start_ms)) active.reset();
  }
  const bool active_enabled =
      active && (active->radio == Radio::kWifi ? config.wifi_enabled : config.ble_enabled);
  if (active_enabled) {
    // The receiver is on and its report is still to come: that window is the
    // current one, and a normally observing radio shows the channel in progress.
    view.current = active->radio;
    RadioCoverageView& radio = active->radio == Radio::kWifi ? view.wifi : view.ble;
    if (radio.state == RadioState::kObserving) radio.last_channel = active->channel;
    return view;
  }
  // No active window: the radio with the latest fresh report.
  const auto fresh = [](const RadioCoverageView& v) {
    return v.state == RadioState::kObserving || v.state == RadioState::kGap;
  };
  const auto& wifi_report = ledger.totals(Radio::kWifi).last_report_ms;
  const auto& ble_report = ledger.totals(Radio::kBle).last_report_ms;
  if (fresh(view.wifi) && (!fresh(view.ble) || *wifi_report >= *ble_report)) {
    view.current = Radio::kWifi;
  } else if (fresh(view.ble)) {
    view.current = Radio::kBle;
  }
  return view;
}

CoverageView MakeCoverageView(const ObservationScheduler& scheduler, const CoverageLedger& ledger,
                              uint64_t now_ms) {
  return MakeCoverageView(scheduler.config(), ledger, now_ms);
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

CoverageStatus MakeCoverageStatus(const CoverageView& view) {
  const auto slot = [&](Radio which, const RadioCoverageView& v) {
    RadioSlotView slot;
    switch (v.state) {
      case RadioState::kOperatorDisabled:
        slot.state = RadioSlotState::kOff;
        return slot;
      case RadioState::kNoData:
        slot.state = RadioSlotState::kNoData;
        return slot;
      case RadioState::kFault:
        slot.state = RadioSlotState::kFault;
        break;
      case RadioState::kGap:
        slot.state = RadioSlotState::kGap;
        break;
      case RadioState::kObserving:
        // Scanning only while its window is the current one: between windows a
        // radio is not receiving, so it is not shown as if it were.
        slot.state = view.current == which ? RadioSlotState::kScanning : RadioSlotState::kIdle;
        break;
    }
    if (which == Radio::kWifi) slot.channel = v.last_channel;
    return slot;
  };
  return {slot(Radio::kWifi, view.wifi), slot(Radio::kBle, view.ble)};
}

}  // namespace firmware::domain
