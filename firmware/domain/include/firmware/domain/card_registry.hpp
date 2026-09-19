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
  bool dimmed = false;
  uint16_t bar_per_mille = 0;  // 0 when there is no value; check has_value
};

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
