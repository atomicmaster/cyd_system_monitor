// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "../expect.hpp"
#include "firmware/domain/coverage.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;

// Runs `count` windows back to back, reporting each as fully observed except
// those whose index is in `lossy` (observed 0) -- a scripted radio driver.
uint64_t RunWindows(domain::ObservationScheduler& scheduler, domain::CoverageLedger& ledger,
                    uint64_t now, int count, int lossy_from = -1, int lossy_to = -1) {
  for (int i = 0; i < count; ++i) {
    const auto window = scheduler.NextWindow(now);
    if (!window) break;
    const bool lossy = i >= lossy_from && i < lossy_to;
    ledger.Record({*window, lossy ? 0u : window->planned_ms, lossy ? 2u : 0u});
    now += window->planned_ms;
  }
  return now;
}

}  // namespace

int RunCoverage() {
  ScenarioResult result;
  std::ostringstream transcript;
  const auto line = [&](const char* label, const domain::ObservationScheduler& scheduler,
                        const domain::CoverageLedger& ledger, uint64_t now) {
    transcript << label << " @" << now << " | "
               << domain::FormatCoverage(domain::MakeCoverageView(scheduler, ledger, now)) << "\n";
  };

  domain::ObservationScheduler scheduler;
  domain::CoverageLedger ledger;
  uint64_t now = 1000;

  line("boot", scheduler, ledger, now);
  now = RunWindows(scheduler, ledger, now, 22);  // one full WiFi revisit
  line("one-revisit", scheduler, ledger, now);
  now = RunWindows(scheduler, ledger, now, 4, 2, 4);  // the last WiFi and BLE windows lost
  line("lossy", scheduler, ledger, now);
  now = RunWindows(scheduler, ledger, now, 2);
  line("recovered", scheduler, ledger, now);

  scheduler.Configure({.wifi_enabled = false});
  line("wifi-disabled", scheduler, ledger, now);
  now = RunWindows(scheduler, ledger, now, 6);
  line("ble-only", scheduler, ledger, now);

  scheduler.Configure({.wifi_enabled = true, .ble_enabled = false});
  ledger.SetFault(domain::Radio::kWifi, "init failed");
  line("wifi-fault", scheduler, ledger, now);
  ledger.ClearFault(domain::Radio::kWifi);
  now = RunWindows(scheduler, ledger, now, 11);
  line("wifi-only", scheduler, ledger, now);

  now += 60000;  // the driver stops reporting: coverage must read unknown
  line("silent", scheduler, ledger, now);

  scheduler.Configure({.wifi_enabled = false, .ble_enabled = false});
  line("host-only", scheduler, ledger, now);

  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/coverage.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the coverage transcript matches simulator/scenarios/coverage.expected "
                "(CYD_UPDATE_EXPECTED=1 regenerates it)");
  return result.Finish("simulator coverage");
}

}  // namespace simulator::scenarios
