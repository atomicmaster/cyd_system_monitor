// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../card_scenario.hpp"
#include "firmware/domain/agents_bar.hpp"
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

const char* ActivityName(firmware::domain::AgentActivity activity) {
  switch (activity) {
    case firmware::domain::AgentActivity::kBusy:
      return "busy";
    case firmware::domain::AgentActivity::kIdle:
      return "idle";
    case firmware::domain::AgentActivity::kUnknown:
      break;
  }
  return "unknown";
}

// The Agents bar's view over a scripted timeline, recorded as a transcript and
// compared with simulator/scenarios/agents-bar.expected.
class BarScenario {
 public:
  BarScenario() : feed_(clock_, 1) {
    std::array<uint8_t, proto::kSessionIdBytes> session{};
    for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
    feed_.StartSession(session);
  }
  ScenarioResult& result() { return result_; }
  void Push(uint64_t ms, uint32_t sequence, const proto::SnapshotGroups& groups) {
    clock_.Set(ms);
    feed_.Push(SnapshotFrame(sequence, groups));
  }
  firmware::domain::AgentsBarView At(uint64_t ms, const char* label) {
    clock_.Set(ms);
    feed_.Tick();
    const auto view = firmware::domain::MakeAgentsBar(feed_.state(), ms);
    transcript_ << label << "\n  message=\"" << view.message << "\"";
    for (const auto& badge : view.badges) {
      transcript_ << " | " << static_cast<unsigned>(badge.slot) << ":" << badge.label << " ("
                  << ActivityName(badge.activity) << ")";
    }
    transcript_ << " | dimmed=" << (view.dimmed ? 1 : 0) << "\n";
    return view;
  }
  int Finish() {
    const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/agents-bar.expected";
    if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript_.str();
    std::ifstream expected_file(path);
    std::stringstream expected;
    expected << expected_file.rdbuf();
    result_.Expect(expected.str() == transcript_.str(),
                   "the transcript matches simulator/scenarios/agents-bar.expected (rerun with "
                   "CYD_UPDATE_EXPECTED=1 to review an intended change)");
    return result_.Finish("simulator agents-bar");
  }

 private:
  ScriptedClock clock_;
  firmware::domain::HostFeed feed_;
  ScenarioResult result_;
  std::ostringstream transcript_;
};

std::string Labels(const firmware::domain::AgentsBarView& view) {
  std::string labels;
  for (const auto& badge : view.badges) labels += (labels.empty() ? "" : ",") + badge.label;
  return labels;
}

}  // namespace

int RunAgentsBar() {
  BarScenario scenario;
  ScenarioResult& result = scenario.result();
  const auto at = [&](uint64_t ms, const char* label) { return scenario.At(ms, label); };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::AgentsSnapshot& agents) {
    scenario.Push(ms, sequence, {.agents = agents});
  };

  uint32_t seq = 0;
  auto view = at(1000, "no Host data yet");
  result.Expect(view.message == "--" && view.badges.empty(),
                "no data is -- and never 'none running'");

  push(2000, ++seq, Snap(kOff, kOff, kOff, 100));
  view = at(2000, "source available, nothing running");
  result.Expect(view.message == "None running" && view.badges.empty() && !view.dimmed,
                "none running is a real reading, undimmed");

  push(3000, ++seq, Snap(kOff, kOn, kOff, 100));
  view = at(3000, "Claude running, no activity source");
  result.Expect(view.message.empty() && Labels(view) == "Claude ?" && view.badges[0].slot == 1 &&
                    view.badges[0].activity == firmware::domain::AgentActivity::kUnknown,
                "running without an adapter is a badge of unknown activity, never busy or idle");

  push(4000, ++seq, Snap(kOn, kOn, kOff, 100));
  view = at(4000, "Codex and Claude running, no activity source");
  result.Expect(Labels(view) == "Codex ?,Claude ?", "each running agent gets its own badge");
  push(4500, ++seq, Snap(kOn, kOn, kOn, 100));
  at(4500, "all three running, no activity source");

  // The source could not list processes: never "none running".
  proto::AgentsSnapshot down;
  down.availability = proto::Availability::kTemporarilyUnavailable;
  push(5000, ++seq, down);
  view = at(5000, "process listing temporarily unavailable");
  result.Expect(view.message == "-- Unavailable" && view.badges.empty() && !view.dimmed,
                "an unavailable source is -- with its cause, never none running");
  proto::AgentsSnapshot broken;
  broken.availability = proto::Availability::kError;
  push(5500, ++seq, broken);
  view = at(5500, "process listing error");
  result.Expect(view.message == "-- Read error", "an error source is -- too");

  // Busy and idle exist on the wire for a future adapter; the badges label them.
  push(6000, ++seq, Snap(kOff, kBusy, kOff, 100));
  view = at(6000, "future adapter: Claude busy");
  result.Expect(Labels(view) == "Claude busy" &&
                    view.badges[0].activity == firmware::domain::AgentActivity::kBusy,
                "busy is labelled busy");
  push(6500, ++seq, Snap(kOff, kOff, kIdle, 100));
  view = at(6500, "future adapter: Grok idle");
  result.Expect(Labels(view) == "Grok idle" &&
                    view.badges[0].activity == firmware::domain::AgentActivity::kIdle,
                "idle is labelled idle");
  push(7000, ++seq, Snap(kBusy, kIdle, kOn, 100));
  at(7000, "future adapter: busy, idle and unknown together");

  // Freshness is derived on the device from the source age (15 s).
  push(20'000, ++seq, Snap(kOff, kOn, kOff, 14'999));
  view = at(20'000, "source sample 14.999 s old: fresh");
  result.Expect(!view.dimmed && view.message.empty(), "just under 15 s is fresh");
  push(21'000, ++seq, Snap(kOff, kOn, kOff, 15'000));
  view = at(21'000, "source sample 15 s old in a new snapshot: stale");
  result.Expect(view.dimmed && view.message == "Last" && Labels(view) == "Claude ?",
                "a re-sent old sample does not look fresh");
  // Frames keep coming (link live) but the sample keeps being re-sent.
  push(30'000, ++seq, Snap(kOff, kOn, kOff, 24'000));
  view = at(30'000, "the same old sample re-sent 9 s later: still stale");
  result.Expect(view.dimmed, "an aging re-sent sample stays stale");
  push(31'000, ++seq, Snap(kOff, kOff, kOff, 200));
  view = at(31'000, "refreshed sample: nothing running now");
  result.Expect(!view.dimmed && view.message == "None running", "a fresh sample is fresh");

  // The source ages while frames keep arriving (the Host re-sends the same
  // sample, sampled at 30.8 s): retained once it is 15 s old.
  push(45'000, ++seq, Snap(kOff, kOff, kOff, 14'200));
  view = at(45'000, "same sample re-sent, 14.2 s old: still fresh");
  result.Expect(!view.dimmed && view.message == "None running", "14.2 s old is fresh");
  push(46'000, ++seq, Snap(kOff, kOff, kOff, 15'200));
  view = at(46'000, "same sample re-sent, 15.2 s old: stale, retained as Last");
  result.Expect(view.dimmed && view.message == "Last none running",
                "a sample that ages past 15 s is retained and dimmed");

  // Frames stop: link lost retains the last reading, dimmed.
  push(50'000, ++seq, Snap(kOff, kOn, kOff, 100));
  at(52'900, "last frame 2.9 s ago: still live");
  view = at(85'000, "35 s without a frame: link stale, retained and dimmed");
  result.Expect(view.dimmed && view.message == "Last" && Labels(view) == "Claude ?",
                "link loss retains the badges with Last");

  return scenario.Finish();
}

}  // namespace simulator::scenarios
