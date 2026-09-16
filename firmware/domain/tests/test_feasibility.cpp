// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <limits>

#include "firmware/domain/feasibility.hpp"

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
  using firmware::domain::feasibility::CoverageMeasurement;
  using firmware::domain::feasibility::ResourceBudget;
  using firmware::domain::feasibility::SerialLoad;

  const auto serial =
      SerialLoad::Assess({.snapshot_bytes = 1024, .snapshots_per_second = 1, .uart_baud = 115200});
  Expect(serial.required_bits_per_second == 10240,
         "one 1024-byte snapshot needs 10,240 UART framing bits per second");
  Expect(serial.headroom_bits_per_second == 104960,
         "115200-baud UART keeps measured headroom after one full snapshot per second");
  Expect(serial.has_headroom, "serial assessment explicitly passes when capacity exceeds load");

  const auto overloaded =
      SerialLoad::Assess({.snapshot_bytes = 12000, .snapshots_per_second = 1, .uart_baud = 115200});
  Expect(!overloaded.has_headroom, "serial assessment fails rather than hiding an overloaded link");

  const auto impossible_serial =
      SerialLoad::Assess({.snapshot_bytes = std::numeric_limits<uint32_t>::max(),
                          .snapshots_per_second = std::numeric_limits<uint32_t>::max(),
                          .uart_baud = std::numeric_limits<uint32_t>::max()});
  Expect(!impossible_serial.has_headroom,
         "an overflowing serial load saturates and never passes as available capacity");

  const CoverageMeasurement coverage{.requested_window_ms = 1000,
                                     .receiver_available_ms = 720,
                                     .uncertain_ms = 180,
                                     .packet_processing_loss = 12};
  Expect(coverage.verified_receive_ms() == 540,
         "only measured, certain receiver availability is counted as verified receive time");
  Expect(coverage.coverage_gap_ms() == 460,
         "unavailable and uncertain time remains a visible Coverage Gap");
  Expect(!coverage.requested_window_is_verified(),
         "a requested Observation Window is not silently accepted as receive time");

  const CoverageMeasurement uncertain_full_window{
      .requested_window_ms = 1000, .receiver_available_ms = 1000, .uncertain_ms = 200};
  Expect(uncertain_full_window.verified_receive_ms() == 800,
         "uncertain receiver time is excluded from verified receive time");
  Expect(uncertain_full_window.coverage_gap_ms() == 200,
         "uncertain receiver time is retained as Coverage Gap");

  const ResourceBudget budget{
      .flash_partition_bytes = 1'048'576,
      .firmware_image_bytes = 800'000,
      .settings_bytes = 32'768,
      .journal_reclamation_bytes = 65'536,
      .operational_alert_reservation_bytes = 8'192,
      .capacity_pressure_alert_reservation_bytes = 8'192,
      .active_alert_count = 32,
      .active_alert_max_bytes = 512,
      .ended_alert_count = 128,
      .ended_alert_max_bytes = 256,
      .baseline_count = 512,
      .baseline_max_bytes = 128,
  };
  Expect(budget.retained_record_bytes() == 114688,
         "retained-record budget totals the declared bounded record capacities");
  Expect(budget.available_headroom_bytes() == 19'200,
         "budget exposes remaining partition headroom instead of assuming capacities fit");
  Expect(budget.fits(), "budget passes only because every declared reservation fits");

  const ResourceBudget overflowed_budget{
      .flash_partition_bytes = std::numeric_limits<uint32_t>::max(),
      .active_alert_count = std::numeric_limits<uint32_t>::max(),
      .active_alert_max_bytes = std::numeric_limits<uint32_t>::max(),
      .ended_alert_count = std::numeric_limits<uint32_t>::max(),
      .ended_alert_max_bytes = std::numeric_limits<uint32_t>::max(),
      .baseline_count = std::numeric_limits<uint32_t>::max(),
      .baseline_max_bytes = std::numeric_limits<uint32_t>::max(),
  };
  Expect(overflowed_budget.retained_record_bytes() == std::numeric_limits<uint64_t>::max(),
         "overflowing record capacities saturate rather than wrapping");
  Expect(!overflowed_budget.fits(), "an overflowing record capacity never passes the budget");

  if (failures == 0) {
    std::printf("firmware_domain_feasibility_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_feasibility_tests: %d failure(s)\n", failures);
  return 1;
}
