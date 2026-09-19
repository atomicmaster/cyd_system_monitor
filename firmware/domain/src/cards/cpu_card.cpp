// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/cpu_view.hpp"

namespace firmware::domain {

// The CPU card is the existing CpuView minus its status bar, so the CPU
// path is unchanged whether it is drawn by CpuScreen or through the registry.
MetricCardView MakeCpuCard(const HostState& state, uint64_t now_ms) {
  const CpuView view = MakeCpuView(state, now_ms);
  return {.title = "CPU",
          .value = view.value,
          .note = view.note,
          .has_value = view.has_value,
          .dimmed = view.dimmed,
          .bar_per_mille = view.bar_per_mille};
}

}  // namespace firmware::domain
