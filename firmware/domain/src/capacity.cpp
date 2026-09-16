// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/capacity.hpp"

namespace firmware::domain::capacity {

uint32_t CapacityBudget::reclaimable_bytes() const {
  if (sample.minimum_free_heap_bytes <= non_reclaimable_bytes) {
    return 0;
  }
  return sample.minimum_free_heap_bytes - non_reclaimable_bytes;
}

bool CapacityBudget::any_task_stack_below(uint32_t floor_bytes) const {
  for (const auto& task : sample.task_stacks) {
    if (task.stack_low_water_mark_bytes < floor_bytes) {
      return true;
    }
  }
  return false;
}

}  // namespace firmware::domain::capacity
