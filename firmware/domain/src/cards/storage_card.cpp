// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {

// Root storage: a dial of the used share of the filesystem containing "/",
// with the physical device's read/write rates as the note ("R12M W4.5M"). Capacity and I/O are
// independent: each has its own availability, `--` plus wording when it is not
// available, and its own retained-and-dimmed state.
MetricCardView MakeStorageCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Storage", .value = "--", .dial = true};
  const StorageMetricState& storage = state.storage;
  if (!storage.has_data) return view;
  const bool link_lost = state.LinkLost();
  const bool capacity_ok = storage.capacity_availability == protocol::Availability::kAvailable &&
                           storage.total_bytes != 0;
  const bool io_ok = storage.io_availability == protocol::Availability::kAvailable;
  const bool capacity_retained =
      link_lost || storage.capacity_freshness == protocol::Freshness::kStale;
  const bool io_retained = link_lost || storage.io_freshness == protocol::Freshness::kStale;

  if (capacity_ok) {
    const uint64_t used = storage.total_bytes - storage.available_bytes;
    view.bar_per_mille = static_cast<uint16_t>(
        used >= storage.total_bytes
            ? 1000
            : static_cast<double>(used) / static_cast<double>(storage.total_bytes) * 1000.0);
    char text[16];
    std::snprintf(text, sizeof text, "%s%u%%", capacity_retained ? "Last " : "",
                  static_cast<unsigned>((view.bar_per_mille + 5) / 10));
    view.value = text;
    view.has_value = true;
    view.dimmed = capacity_retained;
  }
  if (io_ok) {
    // Retained rates are shown only beside a retained capacity, whose "Last"
    // covers the card; otherwise "Last R999G W999G" is too wide for the dial,
    // and bare rates beside a current capacity would pass as current.
    view.note = !io_retained || (capacity_ok && capacity_retained)
                    ? "R" + CompactRate(storage.read_bytes_per_s) + " W" +
                          CompactRate(storage.write_bytes_per_s)
                    : "I/O stale";
    view.dimmed = view.dimmed || io_retained;
  } else if (capacity_ok) {
    // "I/O Unsupported" is too wide for the dial: unsupported and absent both
    // mean this Host has no I/O rates to give.
    const bool none = storage.io_availability == protocol::Availability::kUnsupported ||
                      storage.io_availability == protocol::Availability::kAbsent;
    view.note = none ? "No I/O" : std::string("I/O ") + AvailabilityNote(storage.io_availability);
  } else if (storage.capacity_availability == protocol::Availability::kAvailable) {
    view.note = "Read error";  // "available" with a zero total is a broken read
  } else {
    view.note = AvailabilityNote(storage.capacity_availability);
  }
  return view;
}

}  // namespace firmware::domain
