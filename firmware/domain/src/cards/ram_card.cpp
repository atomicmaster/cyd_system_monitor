// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

const char* NoteText(protocol::Availability availability) {
  switch (availability) {
    case protocol::Availability::kAvailable:
      return "";
    case protocol::Availability::kUnsupported:
      return "Unsupported";
    case protocol::Availability::kAbsent:
      return "Not present";
    case protocol::Availability::kTemporarilyUnavailable:
      return "Unavailable";
    case protocol::Availability::kError:
      return "Read error";
  }
  return "";
}

double Gib(uint64_t bytes) { return static_cast<double>(bytes) / (1ULL << 30); }

}  // namespace

// Physical RAM Occupied: total minus free pages, as "used / total GB" (binary
// gigabytes). Never relabelled as application memory or pressure.
MetricCardView MakeRamCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "RAM", .value = "--"};
  const RamMetricState& ram = state.ram;
  if (!ram.has_data) return view;
  if (ram.availability != protocol::Availability::kAvailable || ram.total_bytes == 0) {
    view.note = NoteText(ram.availability);
    return view;
  }
  const bool retained = state.connection == ConnectionState::kStale ||
                        state.connection == ConnectionState::kIncompatible ||
                        state.connection == ConnectionState::kDisconnected ||
                        ram.freshness == protocol::Freshness::kStale;
  char text[40];
  std::snprintf(text, sizeof text, "%s%.1f / %.1f GB", retained ? "Last " : "", Gib(ram.used_bytes),
                Gib(ram.total_bytes));
  view.value = text;
  view.has_value = true;
  view.dimmed = retained;
  view.bar_per_mille = static_cast<uint16_t>(
      ram.used_bytes >= ram.total_bytes ? 1000 : ram.used_bytes * 1000 / ram.total_bytes);
  return view;
}

}  // namespace firmware::domain
