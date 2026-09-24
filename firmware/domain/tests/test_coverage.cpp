// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/coverage.hpp"

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace firmware::domain;

  // Both radios alternate, WiFi walking channels 1-11 in order.
  {
    ObservationScheduler scheduler;
    uint64_t now = 0;
    bool ok = true;
    for (uint8_t expected = 1; expected <= 11; ++expected) {
      const auto wifi = scheduler.NextWindow(now);
      now += kDefaultWifiDwellMs;
      const auto ble = scheduler.NextWindow(now);
      now += kDefaultBleWindowMs;
      ok = ok && wifi && ble && wifi->radio == Radio::kWifi && wifi->channel == expected &&
           ble->radio == Radio::kBle && ble->channel == 0;
    }
    Expect(ok, "both radios alternate WiFi channel 1-11 with BLE between");
    Expect(scheduler.NextWindow(now)->channel == 1, "the channel plan wraps after channel 11");
    Expect(scheduler.WifiRevisitMs() == 11 * (kDefaultWifiDwellMs + kDefaultBleWindowMs),
           "revisit is one WiFi dwell plus one BLE window per channel");
  }

  // A disabled radio gives its whole budget to the other; both off is Host-only.
  {
    ObservationScheduler scheduler({.wifi_enabled = false, .ble_enabled = true});
    Expect(scheduler.NextWindow(0)->radio == Radio::kBle &&
               scheduler.NextWindow(205)->radio == Radio::kBle,
           "BLE gets every window when WiFi is disabled");
    scheduler.Configure({.wifi_enabled = true, .ble_enabled = false});
    const auto a = scheduler.NextWindow(0);
    const auto b = scheduler.NextWindow(205);
    Expect(
        a->radio == Radio::kWifi && b->radio == Radio::kWifi && a->channel == 1 && b->channel == 2,
        "WiFi gets every window when BLE is disabled");
    Expect(scheduler.WifiRevisitMs() == 11 * kDefaultWifiDwellMs,
           "WiFi revisit shortens when BLE gives up its budget");
    scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
    Expect(!scheduler.NextWindow(0).has_value(), "no windows when both radios are disabled");
    Expect(MakeCoverageView(scheduler, CoverageLedger{}, 0).host_only,
           "both disabled is Host-only");
  }

  // A custom channel plan and a zero dwell are honoured / sanitised.
  {
    ObservationScheduler scheduler({.wifi_channels = {1, 6, 11}, .wifi_dwell_ms = 0});
    Expect(scheduler.channels().size() == 3, "a configured channel plan replaces the default");
    Expect(scheduler.NextWindow(0)->planned_ms == kDefaultWifiDwellMs,
           "zero dwell falls back to the default");
  }

  // Coverage: observed time counts, requested time does not.
  {
    ObservationScheduler scheduler({.ble_enabled = false});
    CoverageLedger ledger;
    Expect(MakeCoverageView(scheduler, ledger, 1000).wifi.state == RadioState::kNoData,
           "an enabled radio with no reports is no-data, not quiet");

    ledger.Record({*scheduler.NextWindow(0), 205, 0});
    auto view = MakeCoverageView(scheduler, ledger, 300);
    Expect(view.wifi.state == RadioState::kObserving && view.wifi.observed_permille == 1000,
           "a fully observed window reads observing at 1000/1000");
    Expect(view.ble.state == RadioState::kOperatorDisabled,
           "a disabled radio reads operator-disabled");

    ledger.Record({*scheduler.NextWindow(205), 0, 3});
    view = MakeCoverageView(scheduler, ledger, 500);
    Expect(view.wifi.state == RadioState::kGap, "a window with no observed time is a gap");
    Expect(view.wifi.observed_permille == 500 && view.wifi.dropped_events == 3,
           "totals keep observed share and dropped events");
    Expect(ledger.observed_ms(Radio::kWifi) == 205 && ledger.gaps().size() == 1 &&
               ledger.gaps().back().duration_ms == 205,
           "the unobserved window is recorded as a gap and adds no observed time");

    ledger.Record({*scheduler.NextWindow(410), 204, 0});
    Expect(MakeCoverageView(scheduler, ledger, 700).wifi.state == RadioState::kObserving,
           "sub-millisecond switching loss is not a gap");

    ledger.Record({*scheduler.NextWindow(615), 999, 0});
    Expect(ledger.totals(Radio::kWifi).observed_ms == 205 + 204 + 205,
           "observed time is clamped to the planned window");

    const uint64_t stale_at = 820 + kStaleRevisitPeriods * scheduler.WifiRevisitMs() + 1;
    Expect(MakeCoverageView(scheduler, ledger, stale_at).wifi.state == RadioState::kNoData,
           "reports that stop arriving make coverage unknown, not quiet");

    ledger.SetFault(Radio::kWifi, "init failed");
    view = MakeCoverageView(scheduler, ledger, 820);
    Expect(view.wifi.state == RadioState::kFault && view.wifi.note == "init failed",
           "a driver fault is surfaced with its reason");
    ledger.ClearFault(Radio::kWifi);
    Expect(MakeCoverageView(scheduler, ledger, 820).wifi.state == RadioState::kObserving,
           "clearing the fault restores the coverage state");
  }

  // The persistent status text distinguishes observed, gap, unknown, off, fault, and Host only.
  // The status line names the current (or most recent) Observation Window and
  // each radio's problem state. It never shows a percentage: observed share of
  // the requested window time is not capture coverage of a channel.
  {
    ObservationScheduler scheduler;
    CoverageLedger ledger;
    const auto text = [&](uint64_t now_ms) {
      return CoverageStatusText(MakeCoverageView(scheduler, ledger, now_ms));
    };
    Expect(text(0) == "WiFi ? BLE ?", "no report reads unknown, never quiet");
    ledger.Record({*scheduler.NextWindow(0), 205, 0});  // WiFi ch1, observed
    Expect(text(205) == "WiFi ch1 BLE ?",
           "the WiFi window names its channel; BLE is still unknown");
    ledger.Record({*scheduler.NextWindow(205), 205, 0});  // BLE, observed
    Expect(text(410) == "BLE scan",
           "the BLE window is current; healthy WiFi between windows is not listed");
    ledger.Record({*scheduler.NextWindow(410), 205, 0});  // WiFi ch2, observed
    Expect(text(615) == "WiFi ch2", "the next WiFi window names its own channel");
    Expect(text(615).find('%') == std::string::npos, "no percentage in the status line");
    ledger.Record({*scheduler.NextWindow(615), 0, 0});  // BLE, lost
    Expect(text(820) == "BLE gap", "a lost window reads gap");
    ledger.Record({*scheduler.NextWindow(820), 205, 0});  // WiFi ch3, observed
    Expect(text(1025) == "WiFi ch3 BLE gap",
           "the gap stays visible while the other radio observes");
    Expect(FormatCoverage(MakeCoverageView(scheduler, ledger, 1025)).find("ble=gap 500/1000") !=
               std::string::npos,
           "the replay checkpoint keeps the observed share");
    ledger.SetFault(Radio::kWifi, "init failed");
    Expect(text(1025) == "WiFi fault BLE gap", "a fault reads fault");
    scheduler.Configure({.wifi_enabled = false});
    Expect(text(1025) == "WiFi off BLE gap", "a disabled radio reads off");
    scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
    Expect(text(1025) == "Host only", "both radios off reads Host only");
    scheduler.Configure({});
    ledger.ClearFault(Radio::kWifi);
    Expect(text(60'000) == "WiFi ? BLE ?", "reports gone stale read unknown, not the last window");
  }
  {
    ObservationScheduler scheduler({.ble_enabled = false, .wifi_channels = {6, 11}});
    CoverageLedger ledger;
    ledger.Record({*scheduler.NextWindow(0), 205, 0});
    Expect(CoverageStatusText(MakeCoverageView(scheduler, ledger, 205)) == "WiFi ch6 BLE off",
           "WiFi alone: its window and BLE off");
    ledger.Record({*scheduler.NextWindow(205), 60, 0});  // under half observed
    Expect(CoverageStatusText(MakeCoverageView(scheduler, ledger, 410)) == "WiFi gap BLE off",
           "a lost current WiFi window reads gap, not its channel");
  }

  // The gap list is bounded; older gaps roll off but stay in the totals.
  {
    ObservationScheduler scheduler({.ble_enabled = false});
    CoverageLedger ledger;
    for (int i = 0; i < 40; ++i) ledger.Record({*scheduler.NextWindow(i * 205u), 0, 0});
    Expect(ledger.gaps().size() == CoverageLedger::kMaxGaps, "the gap list is bounded");
    Expect(ledger.totals(Radio::kWifi).planned_ms == 40u * 205u,
           "rolled-off gaps stay in the totals");
  }

  // The event queue drops the oldest event and counts the drops.
  {
    BoundedEventQueue<int, 3> queue;
    for (int i = 1; i <= 5; ++i) queue.Push(i);
    Expect(queue.size() == 3 && queue.dropped() == 2, "overflow drops and counts");
    Expect(queue.Pop() == 3 && queue.Pop() == 4 && queue.Pop() == 5 && !queue.Pop().has_value(),
           "the oldest events are the ones dropped");
  }

  if (failures == 0) {
    std::printf("firmware_domain_coverage_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_coverage_tests: %d failure(s)\n", failures);
  return 1;
}
