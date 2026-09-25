// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

// Short words: "Last 100% Discharging" would not fit the 148 px value line.
const char* StateText(protocol::BatteryState state) {
  switch (state) {
    case protocol::BatteryState::kCharging:
      return "Charging";
    case protocol::BatteryState::kDischarging:
      return "Draining";
    case protocol::BatteryState::kCharged:
      return "Charged";
    case protocol::BatteryState::kAcIdle:
      return "On AC";
    case protocol::BatteryState::kUnknown:
      break;
  }
  return "";
}

const char* ConditionText(protocol::BatteryCondition condition) {
  switch (condition) {
    case protocol::BatteryCondition::kNormal:
      return "Normal";
    case protocol::BatteryCondition::kServiceRecommended:
      return "Service";
    case protocol::BatteryCondition::kPoor:
      return "Replace";
    case protocol::BatteryCondition::kUnknown:
      break;
  }
  return "";
}

// "87% Charging", "Last 100% Charging": at most 18 characters.
std::string ChargeText(uint16_t per_mille, protocol::BatteryState state, bool retained) {
  char text[40];
  std::snprintf(text, sizeof text, "%s%u%%%s%s", retained ? "Last " : "",
                static_cast<unsigned>(per_mille / 10), *StateText(state) ? " " : "",
                StateText(state));
  return text;
}

// "Health 91% Normal"; a stale sample reads "Last 91% Normal" (the longer
// prefix does not fit 148 px at 12 pt). Either part may be missing on its own
// (the wire sends 0 for an unknown maximum capacity and kUnknown for an
// unknown condition); with neither there is only "Health --".
std::string HealthText(uint16_t per_mille, protocol::BatteryCondition condition, bool retained) {
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
  const bool link_lost = state.LinkLost();
  // The decoder refuses a per-mille value over 1000, so an available half
  // always has a displayable value.
  const bool charge_ok = battery.charge_availability == protocol::Availability::kAvailable;
  const bool condition_ok = battery.condition_availability == protocol::Availability::kAvailable;
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
    view.note = AvailabilityNote(battery.charge_availability);
  } else if (condition_ok) {
    view.note = HealthText(battery.max_capacity_per_mille, battery.condition, condition_retained);
    view.dimmed = view.dimmed || condition_retained;
  } else {
    view.note = std::string("Health ") + AvailabilityNote(battery.condition_availability);
  }
  return view;
}

}  // namespace firmware::domain
