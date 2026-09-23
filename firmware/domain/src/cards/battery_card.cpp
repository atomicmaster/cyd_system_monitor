// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

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

// Wire state 0 unknown, 1 charging, 2 discharging, 3 charged, 4 on AC idle.
// Short words: "Last 100% Discharging" would not fit the 148 px value line.
const char* StateText(uint8_t state) {
  switch (state) {
    case 1:
      return "Charging";
    case 2:
      return "Draining";
    case 3:
      return "Charged";
    case 4:
      return "On AC";
    default:
      return "";
  }
}

// Wire condition 0 unknown, 1 normal, 2 service recommended, 3 poor/replace.
const char* ConditionText(uint8_t condition) {
  switch (condition) {
    case 1:
      return "Normal";
    case 2:
      return "Service";
    case 3:
      return "Replace";
    default:
      return "";
  }
}

// "87% Charging", "Last 100% Charging": at most 18 characters.
std::string ChargeText(uint16_t per_mille, uint8_t state, bool retained) {
  char text[40];
  std::snprintf(text, sizeof text, "%s%u%%%s%s", retained ? "Last " : "",
                static_cast<unsigned>(per_mille / 10), *StateText(state) ? " " : "",
                StateText(state));
  return text;
}

// "Health 91% Normal"; a stale sample reads "Last 91% Normal" (the longer
// prefix does not fit 148 px at 12 pt). Either part may be missing on its own
// (the wire sends 0 for an unknown maximum capacity and condition 0 for an
// unknown condition); with neither there is only "Health --".
std::string HealthText(uint16_t per_mille, uint8_t condition, bool retained) {
  std::string text = retained ? "Last" : "Health";
  if (per_mille != 0) {
    char percent[12];
    std::snprintf(percent, sizeof percent, " %u%%", static_cast<unsigned>(per_mille / 10));
    text += percent;
  }
  if (*ConditionText(condition)) text += std::string(" ") + ConditionText(condition);
  if (per_mille == 0 && !*ConditionText(condition)) text += " --";
  return text;
}

}  // namespace

// Battery: charge and state as the value with the charge as the bar, health
// (maximum capacity and condition) as the note. The two halves are
// independent: each has its own availability, wording, and retained-and-dimmed
// state. A batteryless host is `--` plus "No battery", never 0%, and is not
// dimmed as if it were stale. Time remaining is on the wire but has no room on
// the 154 px card.
MetricCardView MakeBatteryCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Battery", .value = "--"};
  const BatteryMetricState& battery = state.battery;
  if (!battery.has_data) return view;
  if (battery.charge_availability == protocol::Availability::kAbsent) {
    view.note = "No battery";
    return view;
  }
  const bool link_lost = state.connection == ConnectionState::kStale ||
                         state.connection == ConnectionState::kIncompatible ||
                         state.connection == ConnectionState::kDisconnected;
  const bool charge_ok = battery.charge_availability == protocol::Availability::kAvailable &&
                         battery.charge_per_mille <= 1000;
  const bool condition_ok = battery.condition_availability == protocol::Availability::kAvailable &&
                            battery.max_capacity_per_mille <= 1000;
  const bool charge_retained = link_lost || battery.charge_freshness == protocol::Freshness::kStale;
  const bool condition_retained =
      link_lost || battery.condition_freshness == protocol::Freshness::kStale;

  if (charge_ok) {
    view.value = ChargeText(battery.charge_per_mille, battery.state, charge_retained);
    view.has_value = true;
    view.bar_per_mille = battery.charge_per_mille;
    view.dimmed = charge_retained;
  }
  if (!charge_ok) {
    // `--` always carries its cause; the health half is not shown without a charge.
    view.note = battery.charge_availability == protocol::Availability::kAvailable
                    ? "Read error"  // "available" with an impossible value is a broken read
                    : NoteText(battery.charge_availability);
  } else if (condition_ok) {
    view.note = HealthText(battery.max_capacity_per_mille, battery.condition, condition_retained);
    view.dimmed = view.dimmed || condition_retained;
  } else {
    view.note = std::string("Health ") + NoteText(battery.condition_availability);
  }
  return view;
}

}  // namespace firmware::domain
