// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

// The state when no time estimate is available.
const char* StateText(protocol::BatteryState state) {
  switch (state) {
    case protocol::BatteryState::kCharging:
      return "Charging";
    case protocol::BatteryState::kDischarging:
      return "On battery";
    case protocol::BatteryState::kCharged:
      return "Charged";
    case protocol::BatteryState::kAcIdle:
      return "On AC";
    case protocol::BatteryState::kUnknown:
      break;
  }
  return "";
}

// "5h12m left" while discharging, "1h15m to full" while charging, else the
// state word. At most 16 characters: the wire minutes are a u16 below the
// unknown sentinel, so "1092h14m to full" is the longest.
std::string DetailText(protocol::BatteryState state, uint16_t minutes) {
  const bool timed =
      minutes != 0 && minutes != protocol::kBatteryUnknownU16 &&
      (state == protocol::BatteryState::kCharging || state == protocol::BatteryState::kDischarging);
  if (!timed) return StateText(state);
  char text[24];
  std::snprintf(text, sizeof text, "%uh%02um %s", static_cast<unsigned>(minutes / 60),
                static_cast<unsigned>(minutes % 60),
                state == protocol::BatteryState::kCharging ? "to full" : "left");
  return text;
}

const char* ConditionCaption(protocol::BatteryCondition condition) {
  switch (condition) {
    case protocol::BatteryCondition::kServiceRecommended:
      return "Service";
    case protocol::BatteryCondition::kPoor:
      return "Replace";
    case protocol::BatteryCondition::kNormal:
    case protocol::BatteryCondition::kUnknown:
      break;
  }
  return "Health";
}

}  // namespace

// Battery: a cell filled to the charge with a bolt while on power, the charge
// as the value with the time left (or the state) under it, and a health meter
// (maximum capacity) captioned by the condition. The two halves are
// independent: each has its own availability and retained-and-dimmed state; a
// retained health is captioned "Last". A batteryless host is `--` plus "No
// battery", never 0%, and is not dimmed as if it were stale.
MetricCardView MakeBatteryCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Battery", .value = "--", .layout = CardLayout::kBattery};
  const BatteryMetricState& battery = state.battery;
  if (!battery.has_data) return view;
  if (battery.charge_availability == protocol::Availability::kAbsent) {
    view.note = "No battery";
    return view;
  }
  if (battery.charge_availability != protocol::Availability::kAvailable) {
    // `--` always carries its cause; the health half is not shown without a charge.
    view.note = AvailabilityNote(battery.charge_availability);
    return view;
  }
  const bool link_lost = state.LinkLost();
  const bool charge_retained = link_lost || battery.charge_freshness == protocol::Freshness::kStale;
  // The decoder refuses a per-mille value over 1000.
  view.value = std::string(charge_retained ? "Last " : "") +
               std::to_string(battery.charge_per_mille / 10) + "%";
  view.has_value = true;
  view.bar_per_mille = battery.charge_per_mille;
  view.dimmed = charge_retained;
  view.detail = DetailText(battery.state, battery.time_remaining_min);
  view.charging = battery.state == protocol::BatteryState::kCharging ||
                  battery.state == protocol::BatteryState::kCharged;

  if (battery.condition_availability == protocol::Availability::kAvailable) {
    const bool retained = link_lost || battery.condition_freshness == protocol::Freshness::kStale;
    view.meter_caption = retained ? "Last" : ConditionCaption(battery.condition);
    // The wire sends 0 for an unknown maximum capacity.
    const uint16_t capacity = battery.max_capacity_per_mille;
    view.meter_value = capacity != 0 ? std::to_string(capacity / 10) + "%" : "--";
    view.show_meter = capacity != 0;
    view.meter_per_mille = capacity;
    view.dimmed = view.dimmed || retained;
  } else {
    // No value and no bar: the cause takes the whole meter line.
    view.meter_caption = std::string("Health ") + AvailabilityNote(battery.condition_availability);
  }
  return view;
}

}  // namespace firmware::domain
