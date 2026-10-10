// SPDX-License-Identifier: GPL-3.0-only
#include <string>
#include <vector>

#include "../card_scenario.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace proto = cyd::protocol;

constexpr auto kOk = proto::Availability::kAvailable;
constexpr uint32_t kHourMs = 3600 * 1000;

constexpr auto kUnknownState = proto::BatteryState::kUnknown;
constexpr auto kCharging = proto::BatteryState::kCharging;
constexpr auto kDischarging = proto::BatteryState::kDischarging;
constexpr auto kCharged = proto::BatteryState::kCharged;
constexpr auto kAcIdle = proto::BatteryState::kAcIdle;

proto::BatterySnapshot Snap(uint16_t charge, proto::BatteryState state, uint16_t minutes,
                            uint32_t charge_age, uint32_t condition_age) {
  proto::BatterySnapshot g;
  g.charge_availability = kOk;
  g.charge_per_mille = charge;
  g.state = state;
  g.time_remaining_min = minutes;
  g.charge_age_ms = charge_age;
  g.condition_availability = kOk;
  g.max_capacity_per_mille = 910;
  g.condition = proto::BatteryCondition::kNormal;
  g.cycle_count = 312;
  g.condition_age_ms = condition_age;
  return g;
}

}  // namespace

int RunBatteryCards() {
  CardScenario scenario("battery", "battery-cards", CardScenario::Columns::kBar);
  if (!scenario.ready()) return scenario.Finish();
  ScenarioResult& result = scenario.result();
  const auto at = [&](uint64_t ms, const char* label) { return scenario.At(ms, label); };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::BatterySnapshot& battery) {
    scenario.Push(ms, sequence, {.battery = battery});
  };

  uint32_t seq = 0;
  at(1000, "no Host data yet");
  auto g = Snap(870, kCharging, 95, 250, 30 * 60 * 1000);
  push(2000, ++seq, g);
  auto view = at(2000, "live charging: 87%, health 91% Normal");
  result.Expect(view.has_value && !view.dimmed && view.value == "87%" &&
                    view.detail == "1h35m to full" && view.charging &&
                    view.meter_caption == "Health" && view.meter_value == "91%" &&
                    view.show_meter && view.bar_per_mille == 870,
                "a live charging battery shows charge, state, a bolt, health, and the cell fill");
  g = Snap(640, kDischarging, 95, 100, 31 * 60 * 1000);
  push(3000, ++seq, g);
  at(3000, "discharging 64% with 95 min remaining");
  g = Snap(1000, kCharged, proto::kBatteryUnknownU16, 0, 32 * 60 * 1000);
  push(4000, ++seq, g);
  at(4000, "charged 100%, time remaining unknown");
  g = Snap(1000, kAcIdle, proto::kBatteryUnknownU16, 0, 33 * 60 * 1000);
  push(5000, ++seq, g);
  at(5000, "on AC, not charging");

  // A batteryless host: explicit absence, never 0%, never dimmed as stale.
  proto::BatterySnapshot absent;
  absent.charge_availability = proto::Availability::kAbsent;
  absent.condition_availability = proto::Availability::kAbsent;
  push(6000, ++seq, absent);
  view = at(6000, "batteryless host: absent");
  result.Expect(!view.has_value && view.value == "--" && view.note == "No battery" &&
                    !view.dimmed && view.bar_per_mille == 0,
                "absent shows -- and No battery, not 0%");
  view = at(120'000, "batteryless host, 114 s later: still absent, not stale-dimmed");
  result.Expect(!view.dimmed && view.note == "No battery", "absent is not dimmed as stale");

  // Condition unavailable while charge is live.
  g = Snap(870, kDischarging, 120, 200, 0);
  g.condition_availability = proto::Availability::kTemporarilyUnavailable;
  g.max_capacity_per_mille = 0;
  g.condition = proto::BatteryCondition::kUnknown;
  g.cycle_count = proto::kBatteryUnknownU16;
  push(130'000, ++seq, g);
  view = at(130'000, "condition unavailable while charge is live");
  result.Expect(view.has_value && !view.dimmed && view.meter_caption == "Health Unavailable" &&
                    view.meter_value.empty() && !view.show_meter,
                "charge stays live and undimmed while the condition is unavailable");

  // Condition sample sits at the 3 h boundary; charge is live.
  g = Snap(860, kDischarging, 118, 300, 3 * kHourMs - 1);
  push(200'000, ++seq, g);
  view = at(200'000, "condition 2 h 59 m 59.999 s old: fresh");
  result.Expect(!view.dimmed && view.meter_caption == "Health" && view.meter_value == "91%",
                "condition is fresh below 3 h");
  g = Snap(850, kDischarging, 117, 300, 3 * kHourMs);
  push(201'000, ++seq, g);
  view = at(201'000, "condition 3 h old, charge live: condition stale, charge live");
  result.Expect(view.dimmed && view.value == "85%" && view.meter_caption == "Last" &&
                    view.meter_value == "91%",
                "a 3 h old condition is retained and dimmed while the charge is live");
  // The same old sample re-sent later (its age keeps growing) is still stale.
  g = Snap(840, kDischarging, 116, 300, 3 * kHourMs + 60'000);
  push(261'000, ++seq, g);
  view = at(261'000, "old condition sample re-sent 60 s later: still stale");
  result.Expect(view.dimmed && view.meter_caption == "Last",
                "a re-sent old condition sample does not look fresh");
  // A refreshed condition sample is fresh again.
  g = Snap(830, kDischarging, 115, 300, 500);
  push(262'000, ++seq, g);
  view = at(262'000, "condition refreshed: fresh");
  result.Expect(!view.dimmed && view.meter_caption == "Health", "a refreshed condition is fresh");

  // Charge sample re-carried past 90 s is stale even in a new snapshot.
  g = Snap(830, kDischarging, 115, 91'000, 1000);
  push(263'000, ++seq, g);
  view = at(263'000, "charge sample 91 s old in a new snapshot: stale");
  result.Expect(view.dimmed && view.value == "Last 83%",
                "a re-sent old charge sample does not look fresh");

  // Charge read error with the condition still reported.
  g = Snap(0, kUnknownState, proto::kBatteryUnknownU16, 0, 1000);
  g.charge_availability = proto::Availability::kError;
  push(264'000, ++seq, g);
  view = at(264'000, "charge read error");
  result.Expect(!view.has_value && view.value == "--" && view.note == "Read error",
                "a charge error shows -- and its cause, never 0%");

  // Service recommended and replace wording; unknown state.
  g = Snap(500, kUnknownState, proto::kBatteryUnknownU16, 0, 1000);
  g.condition = proto::BatteryCondition::kServiceRecommended;
  g.max_capacity_per_mille = 790;
  push(265'000, ++seq, g);
  at(265'000, "unknown state, service recommended 79%");
  g = Snap(1000, kCharging, 5, 0, 1000);
  g.condition = proto::BatteryCondition::kPoor;
  g.max_capacity_per_mille = 1000;
  push(266'000, ++seq, g);
  at(266'000, "worst-case text: 100% charging, health 100% Replace");

  // Partial health: only the condition word, only the cycle count.
  g = Snap(500, kDischarging, 60, 0, 1000);
  g.max_capacity_per_mille = 0;
  push(266'500, ++seq, g);
  at(266'500, "condition known but maximum capacity unknown");
  g = Snap(500, kDischarging, 60, 0, 1000);
  g.max_capacity_per_mille = 0;
  g.condition = proto::BatteryCondition::kUnknown;
  push(266'800, ++seq, g);
  at(266'800, "only the cycle count known: nothing to show for health");

  // Frames stop: everything is retained and dimmed.
  at(269'700, "last frame 2.9 s ago: still live");
  view = at(300'000, "34 s without a frame: link stale, both halves retained and dimmed");
  result.Expect(view.dimmed && view.value.starts_with("Last ") && view.meter_caption == "Last",
                "link loss retains both readings with Last and dims");

  return scenario.Finish();
}

}  // namespace simulator::scenarios
