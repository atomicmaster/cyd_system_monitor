// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>
#include <type_traits>

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

    ledger.Record(CompletedReport(*scheduler.NextWindow(0), 205, 0));
    auto view = MakeCoverageView(scheduler, ledger, 300);
    Expect(view.wifi.state == RadioState::kObserving && view.wifi.observed_permille == 1000,
           "a fully observed window reads observing at 1000/1000");
    Expect(view.ble.state == RadioState::kOperatorDisabled,
           "a disabled radio reads operator-disabled");

    ledger.Record(CompletedReport(*scheduler.NextWindow(205), 0, 3));
    view = MakeCoverageView(scheduler, ledger, 500);
    Expect(view.wifi.state == RadioState::kGap, "a window with no observed time is a gap");
    Expect(view.wifi.observed_permille == 500 && view.wifi.dropped_events == 3,
           "totals keep observed share and dropped events");
    Expect(ledger.observed_ms(Radio::kWifi) == 205 && ledger.gaps().size() == 1 &&
               ledger.gaps().back().duration_ms == 205,
           "the unobserved window is recorded as a gap and adds no observed time");

    ledger.Record(CompletedReport(*scheduler.NextWindow(410), 204, 0));
    Expect(MakeCoverageView(scheduler, ledger, 700).wifi.state == RadioState::kObserving,
           "sub-millisecond switching loss is not a gap");

    ledger.Record(CompletedReport(*scheduler.NextWindow(615), 999, 0));
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

  // The status bar gives each radio its own slot. A slot's state distinguishes
  // scanning, idle, gap, unknown, off and fault; it never carries a percentage
  // (observed share of window time is not capture coverage of a channel).
  {
    ObservationScheduler scheduler;
    CoverageLedger ledger;
    const auto status = [&](uint64_t now_ms) {
      return MakeCoverageStatus(MakeCoverageView(scheduler, ledger, now_ms));
    };
    const auto is = [](const RadioSlotView& slot, RadioSlotState state, uint8_t channel) {
      return slot.state == state && slot.channel == channel;
    };
    static_assert(RadioSlotView{}.state == RadioSlotState::kNoData,
                  "a default slot is unknown, never idle");
    auto s = status(0);
    Expect(is(s.wifi, RadioSlotState::kNoData, 0) && is(s.ble, RadioSlotState::kNoData, 0),
           "no report reads unknown, never quiet, with no channel");
    ledger.Record(CompletedReport(*scheduler.NextWindow(0), 205, 0));  // WiFi ch1, observed
    s = status(205);
    Expect(is(s.wifi, RadioSlotState::kScanning, 1) && is(s.ble, RadioSlotState::kNoData, 0),
           "the WiFi window scans on its channel; BLE is still unknown");
    ledger.Record(CompletedReport(*scheduler.NextWindow(205), 205, 0));  // BLE, observed
    s = status(410);
    Expect(is(s.wifi, RadioSlotState::kIdle, 1) && is(s.ble, RadioSlotState::kScanning, 0),
           "the BLE window scans; healthy WiFi is idle and keeps its last channel");
    ledger.Record(CompletedReport(*scheduler.NextWindow(410), 205, 0));  // WiFi ch2, observed
    s = status(615);
    Expect(is(s.wifi, RadioSlotState::kScanning, 2) && is(s.ble, RadioSlotState::kIdle, 0),
           "the next WiFi window scans on its own channel");
    ledger.Record(CompletedReport(*scheduler.NextWindow(615), 0, 0));  // BLE, lost
    s = status(820);
    Expect(is(s.wifi, RadioSlotState::kIdle, 2) && is(s.ble, RadioSlotState::kGap, 0),
           "a lost window reads gap, not scanning");
    ledger.Record(CompletedReport(*scheduler.NextWindow(820), 205, 0));  // WiFi ch3, observed
    s = status(1025);
    Expect(is(s.wifi, RadioSlotState::kScanning, 3) && is(s.ble, RadioSlotState::kGap, 0),
           "the gap stays visible while the other radio observes");
    Expect(FormatCoverage(MakeCoverageView(scheduler, ledger, 1025)).find("ble=gap 500/1000") !=
               std::string::npos,
           "the replay checkpoint keeps the observed share");
    ledger.SetFault(Radio::kWifi, "init failed");
    s = status(1025);
    Expect(is(s.wifi, RadioSlotState::kFault, 3) && is(s.ble, RadioSlotState::kGap, 0),
           "a fault wins over scanning and keeps the last channel");
    scheduler.Configure({.wifi_enabled = false});
    s = status(1025);
    Expect(is(s.wifi, RadioSlotState::kOff, 0) && is(s.ble, RadioSlotState::kGap, 0),
           "a disabled radio reads off with no channel");
    scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
    s = status(1025);
    Expect(is(s.wifi, RadioSlotState::kOff, 0) && is(s.ble, RadioSlotState::kOff, 0),
           "both radios off (Host only) is both slots off");
    scheduler.Configure({});
    ledger.ClearFault(Radio::kWifi);
    s = status(60'000);
    Expect(is(s.wifi, RadioSlotState::kNoData, 0) && is(s.ble, RadioSlotState::kNoData, 0),
           "reports gone stale read unknown, not the last window");
  }
  {
    ObservationScheduler scheduler({.ble_enabled = false, .wifi_channels = {6, 11}});
    CoverageLedger ledger;
    ledger.Record(CompletedReport(*scheduler.NextWindow(0), 205, 0));
    auto s = MakeCoverageStatus(MakeCoverageView(scheduler, ledger, 205));
    Expect(s.wifi.state == RadioSlotState::kScanning && s.wifi.channel == 6 &&
               s.ble.state == RadioSlotState::kOff,
           "WiFi alone: its window scans and BLE is off");
    ledger.Record(CompletedReport(*scheduler.NextWindow(205), 60, 0));  // under half observed
    s = MakeCoverageStatus(MakeCoverageView(scheduler, ledger, 410));
    Expect(s.wifi.state == RadioSlotState::kGap && s.wifi.channel == 11,
           "a lost current WiFi window reads gap, with the channel it was on");
  }

  // BLE never shows a channel, whatever the view carries; precedence over idle.
  {
    CoverageView view;
    view.wifi.state = RadioState::kObserving;
    view.wifi.last_channel = 9;
    view.ble.state = RadioState::kObserving;
    view.ble.last_channel = 7;
    view.current = Radio::kBle;
    auto s = MakeCoverageStatus(view);
    Expect(s.ble.state == RadioSlotState::kScanning && s.ble.channel == 0 &&
               s.wifi.state == RadioSlotState::kIdle && s.wifi.channel == 9,
           "BLE carries no channel; idle WiFi keeps its last one");
    view.ble.state = RadioState::kOperatorDisabled;
    view.wifi.state = RadioState::kOperatorDisabled;
    view.wifi.last_channel = 9;
    s = MakeCoverageStatus(view);
    Expect(s.wifi.channel == 0, "a disabled WiFi shows no channel");
    view.wifi.state = RadioState::kNoData;
    Expect(MakeCoverageStatus(view).wifi.channel == 0, "unknown WiFi shows no channel");
  }

  // A Coverage Gap is under half the planned time; exactly half is not a gap, and
  // sub-half losses only reduce the observed share.
  {
    static_assert(std::is_trivially_copyable_v<WindowReport>, "reports cross a task queue");
    ObservationScheduler scheduler({.ble_enabled = false, .wifi_dwell_ms = 200});
    CoverageLedger ledger;
    ledger.Record(CompletedReport(*scheduler.NextWindow(0), 100));
    Expect(ledger.gaps().empty(), "exactly half observed is not a gap");
    Expect(MakeCoverageView(scheduler, ledger, 200).wifi.state == RadioState::kObserving,
           "exactly half observed still reads observing");
    ledger.Record(CompletedReport(*scheduler.NextWindow(200), 150));
    Expect(ledger.gaps().empty() && ledger.totals(Radio::kWifi).observed_ms == 250,
           "a sub-half loss adds no gap and only reduces the observed share");
    ledger.Record(CompletedReport(*scheduler.NextWindow(400), 99));
    Expect(ledger.gaps().size() == 1, "one millisecond under half is a gap");
    Expect(ledger.gaps().back().radio == Radio::kWifi && ledger.gaps().back().start_ms == 400 &&
               ledger.gaps().back().duration_ms == 101,
           "the gap starts with its window and lasts the unavailable time");
    Expect(ledger.totals(Radio::kWifi).observed_ms == 349,
           "a gap window keeps its partial observed time in the totals");
    Expect(MakeCoverageView(scheduler, ledger, 600).wifi.state == RadioState::kGap,
           "the view reads gap for the same under-half rule");
  }

  // The ledger dates a report by when the window actually completed.
  {
    ObservationScheduler scheduler({.ble_enabled = false});
    CoverageLedger ledger;
    WindowReport report = CompletedReport(*scheduler.NextWindow(1000), 205);
    report.completed_ms = 1230;  // overran its 205 ms plan
    ledger.Record(report);
    Expect(ledger.totals(Radio::kWifi).last_report_ms == 1230,
           "last report is the completion time");
    const uint64_t revisit = scheduler.WifiRevisitMs();
    Expect(
        MakeCoverageView(scheduler, ledger, 1230 + kStaleRevisitPeriods * revisit).wifi.state ==
                RadioState::kObserving &&
            MakeCoverageView(scheduler, ledger, 1231 + kStaleRevisitPeriods * revisit).wifi.state ==
                RadioState::kNoData,
        "staleness counts from the completion time");
  }

  // The scheduler plans only what it can run: channels 1-14, no duplicates, sane windows.
  {
    ObservationScheduler scheduler({.wifi_channels = {0, 15, 6, 6, 255, 1, 14, 1}});
    Expect(scheduler.channels() == std::vector<uint8_t>({6, 1, 14}),
           "channels outside 1-14 and duplicates are dropped, order kept");
    Expect(scheduler.config().wifi_channels == scheduler.channels(),
           "config() reports the effective plan");
    scheduler.Configure({.wifi_channels = {0, 99}});
    Expect(scheduler.channels().size() == 11 && scheduler.channels().front() == 1 &&
               scheduler.channels().back() == 11,
           "no valid channel left falls back to the default 1-11 plan");
    scheduler.Configure({.wifi_dwell_ms = 4'000'000'000u, .ble_window_ms = 70'000});
    Expect(scheduler.config().wifi_dwell_ms == kMaxWindowMs &&
               scheduler.config().ble_window_ms == kMaxWindowMs,
           "windows are capped");
    Expect(scheduler.WifiRevisitMs() == 11u * 2 * kMaxWindowMs, "revisit arithmetic does not wrap");
    const ObservationConfig huge{.wifi_channels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
                                 .wifi_dwell_ms = 4'000'000'000u,
                                 .ble_window_ms = 4'000'000'000u};
    Expect(MakeCoverageView(huge, CoverageLedger{}, 0).wifi_revisit_ms == 14u * 2 * kMaxWindowMs,
           "the view normalises the config it is given");
  }

  // The scheduler keeps its place in the channel plan across changes that do
  // not touch it, and restarts when the plan or WiFi's enablement changes.
  {
    ObservationScheduler scheduler({.wifi_channels = {1, 6, 11}});
    scheduler.NextWindow(0);  // WiFi ch1
    scheduler.NextWindow(0);  // BLE
    scheduler.Configure({.wifi_channels = {1, 6, 11}, .wifi_dwell_ms = 100});
    Expect(scheduler.NextWindow(0)->channel == 6,
           "a change that leaves the plan alone keeps its place");
    scheduler.Configure({.ble_enabled = false, .wifi_channels = {1, 6, 11}});
    Expect(scheduler.NextWindow(0)->channel == 11, "BLE-only changes keep the WiFi position");
    scheduler.Configure({.ble_enabled = false, .wifi_channels = {1, 11, 6}});
    Expect(scheduler.NextWindow(0)->channel == 1, "a different plan restarts");
    scheduler.Configure({.wifi_enabled = false, .wifi_channels = {1, 11, 6}});
    scheduler.Configure({.ble_enabled = false, .wifi_channels = {1, 11, 6}});
    Expect(scheduler.NextWindow(0)->channel == 1, "re-enabling WiFi restarts");
  }

  // Staleness is a subtraction, so extreme times cannot wrap it, and a report
  // from the "future" is not stale.
  {
    ObservationConfig config = NormalizeConfig({.ble_enabled = false});
    CoverageLedger ledger;
    WindowReport report = CompletedReport({Radio::kWifi, 1, 0, 205}, 205);
    report.completed_ms = UINT64_MAX - 10;
    ledger.Record(report);
    Expect(MakeCoverageView(config, ledger, UINT64_MAX).wifi.state == RadioState::kObserving,
           "a report near the end of time is not made stale by wrapping");
    Expect(MakeCoverageView(config, ledger, 5).wifi.state == RadioState::kObserving,
           "a report completed after `now` is not stale");
  }

  // An active window is ignored once it is over or already reported.
  {
    ObservationConfig config = NormalizeConfig({});
    CoverageLedger ledger;
    ledger.Record(CompletedReport({Radio::kWifi, 1, 0, 205}, 205));  // completed at 205
    const ActiveWindow active{Radio::kWifi, 2, 3, 205, 205};
    Expect(MakeCoverageView(config, ledger, 300, active).wifi.last_channel == 2 &&
               MakeCoverageView(config, ledger, 410, active).wifi.last_channel == 2,
           "an active window is shown until its planned end");
    Expect(MakeCoverageView(config, ledger, 411, active).wifi.last_channel == 1,
           "a window past its planned end is no longer shown as active");
    ledger.Record(CompletedReport({Radio::kWifi, 2, 205, 205}, 205));  // completed at 410
    Expect(MakeCoverageView(config, ledger, 420, active).wifi.last_channel == 2,
           "a window reported after it started is no longer shown as active");
    ledger.Record(CompletedReport({Radio::kWifi, 3, 410, 205}, 205));  // completed at 615
    Expect(MakeCoverageView(config, ledger, 420, ActiveWindow{Radio::kWifi, 9, 5, 700, 205})
                   .wifi.last_channel == 9,
           "a report that completed before the window started does not hide it");
    Expect(MakeCoverageView(config, ledger, 620, ActiveWindow{Radio::kWifi, 9, 5, 410, 205})
                   .wifi.last_channel == 3,
           "a report that completed after the window started hides it");
    // Strictly inside the window (now < rx_start + planned), so only the
    // superseded-by-a-report branch can hide it.
    CoverageLedger inside;
    inside.Record(CompletedReport({Radio::kWifi, 3, 45, 205}, 205));  // completed at 250
    Expect(MakeCoverageView(config, inside, 300, ActiveWindow{Radio::kWifi, 9, 5, 200, 205})
                   .wifi.last_channel == 3,
           "a report completed inside the active window's span hides it");
  }

  // Only a completed, at-least-half-observed, drop-free window may feed quiet recovery.
  {
    WindowReport report = CompletedReport({Radio::kWifi, 1, 0, 200}, 100);
    Expect(EligibleForQuiet(report), "exactly half observed with no drops is eligible");
    report.observed_ms = 99;
    Expect(!EligibleForQuiet(report), "a Coverage Gap is not eligible");
    report.observed_ms = 200;
    report.dropped_events = 1;
    Expect(!EligibleForQuiet(report), "drops make a lower bound, not observed quiet");
    report.dropped_events = 0;
    report.end = WindowEnd::kConfigurationChanged;
    Expect(!EligibleForQuiet(report),
           "a window cut short by a configuration change is not eligible");
    report.end = WindowEnd::kDriverFault;
    Expect(!EligibleForQuiet(report), "a faulted window is not eligible");
  }

  // The view from a config alone, with and without an active window.
  {
    ObservationConfig config = NormalizeConfig({});
    CoverageLedger ledger;
    ledger.Record(CompletedReport({Radio::kWifi, 1, 0, 205}, 205));
    ledger.Record(CompletedReport({Radio::kBle, 0, 205, 205}, 205));
    auto view = MakeCoverageView(config, ledger, 410);
    Expect(view.current == Radio::kBle, "without an active window the latest report is current");
    view = MakeCoverageView(config, ledger, 500, ActiveWindow{Radio::kWifi, 2, 3, 410, 205});
    const auto active_status = MakeCoverageStatus(view);
    Expect(view.current == Radio::kWifi && view.wifi.last_channel == 2 &&
               active_status.wifi.state == RadioSlotState::kScanning &&
               active_status.wifi.channel == 2 && active_status.ble.state == RadioSlotState::kIdle,
           "an active window scans on its channel before its report arrives");
    Expect(
        MakeCoverageView(config, ledger, 500, ActiveWindow{Radio::kBle, 0, 4, 410, 205}).current ==
            Radio::kBle,
        "an active BLE window is current");
    config.wifi_enabled = false;
    view = MakeCoverageView(config, ledger, 500, ActiveWindow{Radio::kWifi, 2, 3, 410, 205});
    Expect(view.current == Radio::kBle && view.wifi.state == RadioState::kOperatorDisabled,
           "an active window of a disabled radio is ignored");
  }

  // The gap list is bounded; older gaps roll off but stay in the totals.
  {
    ObservationScheduler scheduler({.ble_enabled = false});
    CoverageLedger ledger;
    for (int i = 0; i < 40; ++i)
      ledger.Record(CompletedReport(*scheduler.NextWindow(i * 205u), 0, 0));
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
