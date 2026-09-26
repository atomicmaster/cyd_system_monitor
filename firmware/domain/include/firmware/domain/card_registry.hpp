// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "firmware/domain/host_state.hpp"

namespace firmware::domain {

// What one metric card shows, as plain text and flags, so the board and the
// simulator render identically and tests need no LVGL. The status bar
// (connection and clock) is not part of a card; see CpuView.
struct MetricCardView {
  std::string title;
  std::string value;  // "62%", "Last 62%", or "--"; a missing value is never "0"
  std::string note;   // availability wording when there is no value, else empty
  bool has_value = false;
  bool show_bar = true;  // false for a card whose value is not a proportion
  // Drawn as a dial: `value` is a short percentage ("62%", "Last 62%", "--")
  // shown inside the arc, and `note` is one short line under it.
  bool dial = false;
  bool dimmed = false;
  uint16_t bar_per_mille = 0;  // 0 when there is no value; check has_value
};

// The wording under a `--` value for a metric that is not available ("" when
// it is). Shared by every card, so a cause reads the same everywhere.
const char* AvailabilityNote(protocol::Availability availability);

// One byte rate in at most five ASCII characters ("812B", "1.2K", "340K",
// "1.2M", "12M", "999G"; clamped at 999 GB/s), so paired rates such as
// "Last D999M U999M" stay short. No arrow glyphs: the built-in font's arrows
// are not guaranteed, and letters (D/U, R/W) read clearly.
std::string CompactRate(uint64_t bytes_per_s);

using MakeCardFn = MetricCardView (*)(const HostState& state, uint64_t now_ms);

struct CardDescriptor {
  const char* id;
  MakeCardFn make;
};

// Every registered card, in display order. The list is card_registry.def:
// one line per feature, so parallel feature tickets touch one line each and
// nothing else shared (see docs/tickets/C02.md, "Adding a metric").
std::span<const CardDescriptor> Cards();

}  // namespace firmware::domain
