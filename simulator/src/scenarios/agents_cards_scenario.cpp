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

// Agent states, in slot order [codex, claude, grok] below.
constexpr auto kOff = proto::AgentState::kNotRunning;
constexpr auto kOn = proto::AgentState::kRunning;  // activity unknown
constexpr auto kBusy = proto::AgentState::kBusy;
constexpr auto kIdle = proto::AgentState::kIdle;

proto::AgentsSnapshot Snap(proto::AgentState codex, proto::AgentState claude,
                           proto::AgentState grok, uint32_t age_ms) {
  proto::AgentsSnapshot g;
  g.availability = kOk;
  g.source_age_ms = age_ms;
  g.state = {codex, claude, grok};
  return g;
}

}  // namespace

int RunAgentsCards() {
  CardScenario scenario("agents", "agents-cards", CardScenario::Columns::kShowBar);
  if (!scenario.ready()) return scenario.Finish();
  ScenarioResult& result = scenario.result();
  const auto at = [&](uint64_t ms, const char* label) { return scenario.At(ms, label); };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::AgentsSnapshot& agents) {
    scenario.Push(ms, sequence, {.agents = agents});
  };

  uint32_t seq = 0;
  auto view = at(1000, "no Host data yet");
  result.Expect(view.value == "--" && !view.has_value && !view.show_bar,
                "no data is -- and never 'none running'");

  push(2000, ++seq, Snap(kOff, kOff, kOff, 100));
  view = at(2000, "source available, nothing running");
  result.Expect(view.value == "None running" && view.has_value && !view.dimmed && !view.show_bar &&
                    view.note.empty(),
                "none running is a real value, undimmed, without a bar");

  push(3000, ++seq, Snap(kOff, kOn, kOff, 100));
  view = at(3000, "Claude running, no activity source");
  result.Expect(view.value == "1 running" && view.note == "Claude, unknown" && !view.dimmed,
                "running without an adapter reads activity unknown, never busy or idle");

  push(4000, ++seq, Snap(kOn, kOn, kOff, 100));
  view = at(4000, "Codex and Claude running, no activity source");
  result.Expect(view.value == "2 running" && view.note == "Activity unknown",
                "several agents without an adapter stay activity unknown");
  push(4500, ++seq, Snap(kOn, kOn, kOn, 100));
  at(4500, "all three running, no activity source");

  // The source could not list processes: never "none running".
  proto::AgentsSnapshot down;
  down.availability = proto::Availability::kTemporarilyUnavailable;
  push(5000, ++seq, down);
  view = at(5000, "process listing temporarily unavailable");
  result.Expect(view.value == "--" && view.note == "Unavailable" && !view.has_value && !view.dimmed,
                "an unavailable source is -- with its cause, never none running");
  proto::AgentsSnapshot broken;
  broken.availability = proto::Availability::kError;
  push(5500, ++seq, broken);
  view = at(5500, "process listing error");
  result.Expect(view.value == "--" && view.note == "Read error", "an error source is -- too");

  // Busy and idle exist on the wire for a future adapter; the card labels them.
  push(6000, ++seq, Snap(kOff, kBusy, kOff, 100));
  view = at(6000, "future adapter: Claude busy");
  result.Expect(view.value == "1 running" && view.note == "Claude busy", "busy is labelled busy");
  push(6500, ++seq, Snap(kOff, kOff, kIdle, 100));
  view = at(6500, "future adapter: Grok idle");
  result.Expect(view.value == "1 running" && view.note == "Grok idle", "idle is labelled idle");
  push(7000, ++seq, Snap(kBusy, kIdle, kOn, 100));
  at(7000, "future adapter: busy, idle and unknown together");
  push(7500, ++seq, Snap(kBusy, kIdle, kOff, 100));
  at(7500, "future adapter: one busy, one idle");

  // Freshness is derived on the device from the source age (15 s).
  push(20'000, ++seq, Snap(kOff, kOn, kOff, 14'999));
  view = at(20'000, "source sample 14.999 s old: fresh");
  result.Expect(!view.dimmed && view.value == "1 running", "just under 15 s is fresh");
  push(21'000, ++seq, Snap(kOff, kOn, kOff, 15'000));
  view = at(21'000, "source sample 15 s old in a new snapshot: stale");
  result.Expect(view.dimmed && view.value == "Last 1 running" && view.note == "Claude, unknown",
                "a re-sent old sample does not look fresh");
  // Frames keep coming (link live) but the sample keeps being re-sent.
  push(30'000, ++seq, Snap(kOff, kOn, kOff, 24'000));
  view = at(30'000, "the same old sample re-sent 9 s later: still stale");
  result.Expect(view.dimmed, "an aging re-sent sample stays stale");
  push(31'000, ++seq, Snap(kOff, kOff, kOff, 200));
  view = at(31'000, "refreshed sample: nothing running now");
  result.Expect(!view.dimmed && view.value == "None running", "a fresh sample is fresh");

  // The source ages while frames keep arriving (the Host re-sends the same
  // sample, sampled at 30.8 s): retained once it is 15 s old.
  push(45'000, ++seq, Snap(kOff, kOff, kOff, 14'200));
  view = at(45'000, "same sample re-sent, 14.2 s old: still fresh");
  result.Expect(!view.dimmed && view.value == "None running", "14.2 s old is fresh");
  push(46'000, ++seq, Snap(kOff, kOff, kOff, 15'200));
  view = at(46'000, "same sample re-sent, 15.2 s old: stale, retained as Last");
  result.Expect(view.dimmed && view.value == "Last none running",
                "a sample that ages past 15 s is retained and dimmed");

  // Frames stop: link lost retains the last reading, dimmed.
  push(50'000, ++seq, Snap(kOff, kOn, kOff, 100));
  at(52'900, "last frame 2.9 s ago: still live");
  view = at(85'000, "35 s without a frame: link stale, retained and dimmed");
  result.Expect(view.dimmed && view.value == "Last 1 running", "link loss retains with Last");

  return scenario.Finish();
}

}  // namespace simulator::scenarios
