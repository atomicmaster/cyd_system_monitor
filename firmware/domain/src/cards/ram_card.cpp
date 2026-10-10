// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

double Gib(uint64_t bytes) { return static_cast<double>(bytes) / (1ULL << 30); }

}  // namespace

// Physical RAM Occupied: total minus free pages, as a dial of the occupied
// share with "used / total GB" (binary gigabytes) under it. Never relabelled
// as application memory or pressure.
MetricCardView MakeRamCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "RAM", .value = "--", .layout = CardLayout::kDial};
  const RamMetricState& ram = state.ram;
  if (!ram.has_data) return view;
  if (ram.availability != protocol::Availability::kAvailable || ram.total_bytes == 0) {
    view.note = AvailabilityNote(ram.availability);
    return view;
  }
  const bool retained = state.LinkLost() || ram.freshness == protocol::Freshness::kStale;
  view.bar_per_mille = static_cast<uint16_t>(
      ram.used_bytes >= ram.total_bytes ? 1000 : ram.used_bytes * 1000 / ram.total_bytes);
  char text[40];
  std::snprintf(text, sizeof text, "%s%u%%", retained ? "Last " : "",
                static_cast<unsigned>((view.bar_per_mille + 5) / 10));
  view.value = text;
  std::snprintf(text, sizeof text, "%.1f / %.1f GB", Gib(ram.used_bytes), Gib(ram.total_bytes));
  view.note = text;
  view.has_value = true;
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
