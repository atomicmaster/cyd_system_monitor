// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../expect.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/host_feed.hpp"
#include "protocol.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

namespace domain = firmware::domain;
namespace proto = cyd::protocol;

class ScriptedClock : public domain::MonotonicClock {
 public:
  uint64_t NowMs() const override { return now_ms_; }
  void Set(uint64_t now_ms) { now_ms_ = now_ms; }

 private:
  uint64_t now_ms_ = 1000;
};

constexpr auto kOk = proto::Availability::kAvailable;
constexpr uint32_t kHourMs = 3600 * 1000;

std::vector<uint8_t> Frame(uint32_t sequence, const proto::BatterySnapshot* battery) {
  proto::Envelope envelope;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.sequence = sequence;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const proto::CpuSnapshot cpu{kOk, 100, 0, proto::Freshness::kFresh, 0};
  std::vector<uint8_t> frame;
  proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, battery, &frame);
  return frame;
}

// state: 0 unknown, 1 charging, 2 discharging, 3 charged, 4 on AC idle.
proto::BatterySnapshot Snap(uint16_t charge, uint8_t state, uint16_t minutes, uint32_t charge_age,
                            uint32_t condition_age) {
  proto::BatterySnapshot g;
  g.charge_availability = kOk;
  g.charge_per_mille = charge;
  g.state = state;
  g.time_remaining_min = minutes;
  g.charge_age_ms = charge_age;
  g.condition_availability = kOk;
  g.max_capacity_per_mille = 910;
  g.condition = 1;
  g.cycle_count = 312;
  g.condition_age_ms = condition_age;
  return g;
}

const domain::CardDescriptor* BatteryCard() {
  for (const auto& card : domain::Cards()) {
    if (std::string(card.id) == "battery") return &card;
  }
  return nullptr;
}

}  // namespace

int RunBatteryCards() {
  ScenarioResult result;
  ScriptedClock clock;
  domain::HostFeed feed(clock, 1);
  std::array<uint8_t, proto::kSessionIdBytes> session{};
  for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
  feed.StartSession(session);
  const domain::CardDescriptor* card = BatteryCard();
  result.Expect(card != nullptr, "the registry lists a battery card");
  if (card == nullptr) return result.Finish("simulator battery-cards");

  std::ostringstream transcript;
  const auto at = [&](uint64_t ms, const char* label) {
    clock.Set(ms);
    feed.Tick();
    const auto view = card->make(feed.state(), ms);
    transcript << label << "\n  " << view.title << " | " << view.value << " | " << view.note
               << " | bar=" << view.bar_per_mille << " | has_value=" << (view.has_value ? 1 : 0)
               << " | dimmed=" << (view.dimmed ? 1 : 0) << "\n";
    return view;
  };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::BatterySnapshot& battery) {
    clock.Set(ms);
    feed.Push(Frame(sequence, &battery));
  };

  uint32_t seq = 0;
  at(1000, "no Host data yet");
  auto g = Snap(870, 1, 95, 250, 30 * 60 * 1000);
  push(2000, ++seq, g);
  auto view = at(2000, "live charging: 87%, health 91% Normal");
  result.Expect(view.has_value && !view.dimmed && view.value == "87% Charging" &&
                    view.note == "Health 91% Normal" && view.bar_per_mille == 870,
                "a live charging battery shows charge, state, health, and the charge bar");
  g = Snap(640, 2, 95, 100, 31 * 60 * 1000);
  push(3000, ++seq, g);
  at(3000, "discharging 64% with 95 min remaining");
  g = Snap(1000, 3, proto::kBatteryUnknownU16, 0, 32 * 60 * 1000);
  push(4000, ++seq, g);
  at(4000, "charged 100%, time remaining unknown");
  g = Snap(1000, 4, proto::kBatteryUnknownU16, 0, 33 * 60 * 1000);
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
  g = Snap(870, 2, 120, 200, 0);
  g.condition_availability = proto::Availability::kTemporarilyUnavailable;
  g.max_capacity_per_mille = 0;
  g.condition = 0;
  g.cycle_count = proto::kBatteryUnknownU16;
  push(130'000, ++seq, g);
  view = at(130'000, "condition unavailable while charge is live");
  result.Expect(view.has_value && !view.dimmed && view.note == "Health Unavailable",
                "charge stays live and undimmed while the condition is unavailable");

  // Condition sample sits at the 3 h boundary; charge is live.
  g = Snap(860, 2, 118, 300, 3 * kHourMs - 1);
  push(200'000, ++seq, g);
  view = at(200'000, "condition 2 h 59 m 59.999 s old: fresh");
  result.Expect(!view.dimmed && view.note == "Health 91% Normal", "condition is fresh below 3 h");
  g = Snap(850, 2, 117, 300, 3 * kHourMs);
  push(201'000, ++seq, g);
  view = at(201'000, "condition 3 h old, charge live: condition stale, charge live");
  result.Expect(view.dimmed && view.value == "85% Draining" && view.note == "Last 91% Normal",
                "a 3 h old condition is retained and dimmed while the charge is live");
  // The same old sample re-sent later (its age keeps growing) is still stale.
  g = Snap(840, 2, 116, 300, 3 * kHourMs + 60'000);
  push(261'000, ++seq, g);
  view = at(261'000, "old condition sample re-sent 60 s later: still stale");
  result.Expect(view.dimmed && view.note.starts_with("Last"),
                "a re-sent old condition sample does not look fresh");
  // A refreshed condition sample is fresh again.
  g = Snap(830, 2, 115, 300, 500);
  push(262'000, ++seq, g);
  view = at(262'000, "condition refreshed: fresh");
  result.Expect(!view.dimmed && view.note == "Health 91% Normal", "a refreshed condition is fresh");

  // Charge sample re-carried past 90 s is stale even in a new snapshot.
  g = Snap(830, 2, 115, 91'000, 1000);
  push(263'000, ++seq, g);
  view = at(263'000, "charge sample 91 s old in a new snapshot: stale");
  result.Expect(view.dimmed && view.value == "Last 83% Draining",
                "a re-sent old charge sample does not look fresh");

  // Charge read error with the condition still reported.
  g = Snap(0, 0, proto::kBatteryUnknownU16, 0, 1000);
  g.charge_availability = proto::Availability::kError;
  push(264'000, ++seq, g);
  view = at(264'000, "charge read error");
  result.Expect(!view.has_value && view.value == "--" && view.note == "Read error",
                "a charge error shows -- and its cause, never 0%");

  // Service recommended and replace wording; unknown state.
  g = Snap(500, 0, proto::kBatteryUnknownU16, 0, 1000);
  g.condition = 2;
  g.max_capacity_per_mille = 790;
  push(265'000, ++seq, g);
  at(265'000, "unknown state, service recommended 79%");
  g = Snap(1000, 1, 5, 0, 1000);
  g.condition = 3;
  g.max_capacity_per_mille = 1000;
  push(266'000, ++seq, g);
  at(266'000, "worst-case text: 100% charging, health 100% Replace");

  // Partial health: only the condition word, only the cycle count.
  g = Snap(500, 2, 60, 0, 1000);
  g.max_capacity_per_mille = 0;
  push(266'500, ++seq, g);
  at(266'500, "condition known but maximum capacity unknown");
  g = Snap(500, 2, 60, 0, 1000);
  g.max_capacity_per_mille = 0;
  g.condition = 0;
  push(266'800, ++seq, g);
  at(266'800, "only the cycle count known: nothing to show for health");

  // Frames stop: everything is retained and dimmed.
  at(269'700, "last frame 2.9 s ago: still live");
  view = at(300'000, "34 s without a frame: link stale, both halves retained and dimmed");
  result.Expect(view.dimmed && view.value.starts_with("Last ") && view.note.starts_with("Last"),
                "link loss retains both readings with Last and dims");

  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/battery-cards.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the transcript matches simulator/scenarios/battery-cards.expected "
                "(rerun with CYD_UPDATE_EXPECTED=1 to review an intended change)");
  return result.Finish("simulator battery-cards");
}

}  // namespace simulator::scenarios
