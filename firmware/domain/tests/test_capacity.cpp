// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "firmware/domain/capacity.hpp"

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
  using firmware::domain::capacity::CapacityBudget;
  using firmware::domain::capacity::CapacitySample;
  using firmware::domain::capacity::TaskStackSample;

  CapacitySample sample;
  sample.build_identity = "test-build";
  sample.lvgl_pool_total_bytes = 65536;
  sample.lvgl_pool_peak_used_bytes = 40000;
  sample.lvgl_pool_available_bytes = 25536;
  sample.free_heap_bytes = 120000;
  sample.minimum_free_heap_bytes = 90000;
  sample.task_stacks = {
      {.task_name = "main", .stack_low_water_mark_bytes = 2048},
      {.task_name = "dev_console", .stack_low_water_mark_bytes = 3200},
  };

  const CapacityBudget budget{.sample = sample, .non_reclaimable_bytes = 65536};
  Expect(budget.reclaimable_bytes() == 24464,
         "reclaimable headroom excludes the declared non-reclaimable LVGL pool reservation");
  Expect(!budget.any_task_stack_below(1024),
         "no task stack is flagged below a floor every measured task clears");
  Expect(budget.any_task_stack_below(2500),
         "a task stack below the floor is flagged even when other tasks clear it");

  const CapacityBudget overclaimed_budget{.sample = sample, .non_reclaimable_bytes = 500000};
  Expect(
      overclaimed_budget.reclaimable_bytes() == 0,
      "a non-reclaimable declaration larger than measured headroom saturates at zero rather than "
      "going negative");

  const CapacityBudget empty_budget{};
  Expect(empty_budget.reclaimable_bytes() == 0,
         "an empty sample reports zero reclaimable headroom");
  Expect(!empty_budget.any_task_stack_below(1),
         "no task stacks measured means no task stack can be flagged below any floor");

  if (failures == 0) {
    std::printf("firmware_domain_capacity_tests: ok\n");
    return 0;
  }
  std::fprintf(stderr, "firmware_domain_capacity_tests: %d failure(s)\n", failures);
  return 1;
}
