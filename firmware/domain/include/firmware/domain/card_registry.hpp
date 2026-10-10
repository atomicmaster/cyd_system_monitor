// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "firmware/domain/host_state.hpp"

namespace firmware::domain {

// How a card is drawn. The card maker chooses it, so even a card with no data
// yet has its final shape.
enum class CardLayout : uint8_t {
  kPanel,    // title, value, note, and a bar along the bottom
  kDial,     // a 270-degree arc with the value inside and one line (or rates) under it
  kBattery,  // a battery cell filled to the charge, the value and detail beside it,
             // and a meter line (health) underneath
  kRates,    // a link line over two rate rows, each a value and a log-scale meter
};

// Two rates shown in fixed slots: Storage read/write under its dial, Network
// download/upload rows. Each is a CompactRate text; the per-mille levels are a
// log scale (LogRatePerMille) for meters, 0 when the rates are retained.
struct RatePair {
  std::string first;
  std::string second;
  uint16_t first_per_mille = 0;
  uint16_t second_per_mille = 0;
};

// What one metric card shows, as plain text and flags, so the board and the
// simulator render identically and tests need no LVGL. The status bar
// (connection and clock) is not part of a card; see CpuView.
struct MetricCardView {
  std::string title;
  std::string value;  // "62%", "Last 62%", or "--"; a missing value is never "0"
  std::string note;   // availability wording when there is no value, else empty
  bool has_value = false;
  bool show_bar = true;  // false for a card whose value is not a proportion
  // kDial: `value` is a short percentage ("62%", "Last 62%", "--") shown
  // inside the arc, and `note` (or `rates`) is one short line under it.
  CardLayout layout = CardLayout::kPanel;
  bool dimmed = false;
  uint16_t bar_per_mille = 0;  // 0 when there is no value; check has_value
  // Rates in fixed slots, replacing the note (kDial) or the value (kRates).
  std::optional<RatePair> rates;
  // kBattery: the line under the value ("5h12m left", "On AC"), a bolt in the
  // cell while on power, and the meter line (caption, value, bar).
  std::string detail;
  bool charging = false;
  std::string meter_caption;  // "Health", "Service", "Replace", or "Last" when retained
  std::string meter_value;    // "91%" or "--"
  bool show_meter = false;
  uint16_t meter_per_mille = 0;
  // kRates: the note is a WiFi link, drawn after a WiFi glyph.
  bool wifi = false;
};

// The wording under a `--` value for a metric that is not available ("" when
// it is). Shared by every card, so a cause reads the same everywhere.
const char* AvailabilityNote(protocol::Availability availability);

// One byte rate in at most five ASCII characters ("812B", "1.2K", "340K",
// "1.2M", "12M", "999G"; clamped at 999 GB/s), so paired rates such as
// "Last D999M U999M" stay short. No arrow glyphs: the built-in font's arrows
// are not guaranteed, and letters (D/U, R/W) read clearly.
std::string CompactRate(uint64_t bytes_per_s);

// A byte rate on a log scale for a meter: 0 below 100 B/s, 1000 at 1 GB/s and
// above, so a few KB/s and tens of MB/s both register.
uint16_t LogRatePerMille(uint64_t bytes_per_s);

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
