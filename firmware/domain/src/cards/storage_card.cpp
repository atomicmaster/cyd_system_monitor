// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

// Storage is marketed and shown by Finder in decimal units, so capacity uses
// 10^9 / 10^12 (RAM uses binary gigabytes, the way memory is sold).
constexpr double kGB = 1e9;
constexpr double kTB = 1e12;

// "used / total GB", or TB once the total reaches 1 TB. Longest text: 21
// characters below 1 PB ("Last 999.9 / 999.9 TB"); the wire bound (u64) is 31.
std::string CapacityText(uint64_t used, uint64_t total, bool retained) {
  const bool terabytes = total >= static_cast<uint64_t>(kTB);
  const double unit = terabytes ? kTB : kGB;
  char text[48];
  std::snprintf(text, sizeof text, "%s%.1f / %.1f %s", retained ? "Last " : "", used / unit,
                total / unit, terabytes ? "TB" : "GB");
  return text;
}

// One rate with a unit chosen so the number stays under four digits: "812B/s",
// "12.3K/s", "4.5M/s", "1.2G/s" (bytes; no space, so the note fits a 148 px card
// at 12 pt). Clamped at 999.9 GB/s, so at most 8 characters.
std::string RateText(uint64_t bytes_per_s) {
  constexpr uint64_t kClamp = 999'900'000'000ULL;
  if (bytes_per_s > kClamp) bytes_per_s = kClamp;
  const double value = static_cast<double>(bytes_per_s);
  char text[24];
  if (value < 1e3) {
    std::snprintf(text, sizeof text, "%lluB/s", static_cast<unsigned long long>(bytes_per_s));
  } else if (value < 999.95e3) {
    std::snprintf(text, sizeof text, "%.1fK/s", value / 1e3);
  } else if (value < 999.95e6) {
    std::snprintf(text, sizeof text, "%.1fM/s", value / 1e6);
  } else {
    std::snprintf(text, sizeof text, "%.1fG/s", value / 1e9);
  }
  return text;
}

}  // namespace

// Root storage: capacity of the filesystem containing "/" as the value and the
// physical device's read/write rates as the note ("R12.3M/s W4.5M/s",
// at most 23 characters with the "Last " prefix). Capacity and I/O are
// independent: each has its own availability, `--` plus wording when it is not
// available, and its own retained-and-dimmed state.
MetricCardView MakeStorageCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Storage", .value = "--"};
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
    view.value = CapacityText(used, storage.total_bytes, capacity_retained);
    view.has_value = true;
    view.bar_per_mille = static_cast<uint16_t>(
        used >= storage.total_bytes
            ? 1000
            : static_cast<double>(used) / static_cast<double>(storage.total_bytes) * 1000.0);
    view.dimmed = capacity_retained;
  }
  if (io_ok) {
    view.note = std::string(io_retained ? "Last " : "") + "R" + RateText(storage.read_bytes_per_s) +
                " W" + RateText(storage.write_bytes_per_s);
    view.dimmed = view.dimmed || io_retained;
  } else if (capacity_ok) {
    view.note = std::string("I/O ") + AvailabilityNote(storage.io_availability);
  } else if (storage.capacity_availability == protocol::Availability::kAvailable) {
    view.note = "Read error";  // "available" with a zero total is a broken read
  } else {
    view.note = AvailabilityNote(storage.capacity_availability);
  }
  return view;
}

}  // namespace firmware::domain
