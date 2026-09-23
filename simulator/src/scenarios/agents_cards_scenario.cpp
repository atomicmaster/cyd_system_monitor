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

std::vector<uint8_t> Frame(uint32_t sequence, const proto::AgentsSnapshot* agents) {
  proto::Envelope envelope;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.sequence = sequence;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const proto::CpuSnapshot cpu{kOk, 100, 0, proto::Freshness::kFresh, 0};
  std::vector<uint8_t> frame;
  proto::EncodeSnapshot(envelope, cpu, nullptr, nullptr, nullptr, agents, &frame);
  return frame;
}

// States in slot order [codex, claude, grok]: 0 not running, 1 running with
// activity unknown, 2 busy, 3 idle.
proto::AgentsSnapshot Snap(uint8_t codex, uint8_t claude, uint8_t grok, uint32_t age_ms) {
  proto::AgentsSnapshot g;
  g.availability = kOk;
  g.source_age_ms = age_ms;
  g.state[0] = codex;
  g.state[1] = claude;
  g.state[2] = grok;
  return g;
}

const domain::CardDescriptor* AgentsCard() {
  for (const auto& card : domain::Cards()) {
    if (std::string(card.id) == "agents") return &card;
  }
  return nullptr;
}

}  // namespace

int RunAgentsCards() {
  ScenarioResult result;
  ScriptedClock clock;
  domain::HostFeed feed(clock, 1);
  std::array<uint8_t, proto::kSessionIdBytes> session{};
  for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
  feed.StartSession(session);
  const domain::CardDescriptor* card = AgentsCard();
  result.Expect(card != nullptr, "the registry lists an agents card");
  if (card == nullptr) return result.Finish("simulator agents-cards");

  std::ostringstream transcript;
  const auto at = [&](uint64_t ms, const char* label) {
    clock.Set(ms);
    feed.Tick();
    const auto view = card->make(feed.state(), ms);
    transcript << label << "\n  " << view.title << " | " << view.value << " | " << view.note
               << " | show_bar=" << (view.show_bar ? 1 : 0)
               << " | has_value=" << (view.has_value ? 1 : 0)
               << " | dimmed=" << (view.dimmed ? 1 : 0) << "\n";
    return view;
  };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::AgentsSnapshot& agents) {
    clock.Set(ms);
    feed.Push(Frame(sequence, &agents));
  };

  uint32_t seq = 0;
  auto view = at(1000, "no Host data yet");
  result.Expect(view.value == "--" && !view.has_value && !view.show_bar,
                "no data is -- and never 'none running'");

  push(2000, ++seq, Snap(0, 0, 0, 100));
  view = at(2000, "source available, nothing running");
  result.Expect(view.value == "None running" && view.has_value && !view.dimmed && !view.show_bar &&
                    view.note.empty(),
                "none running is a real value, undimmed, without a bar");

  push(3000, ++seq, Snap(0, 1, 0, 100));
  view = at(3000, "Claude running, no activity source");
  result.Expect(view.value == "1 running" && view.note == "Claude, unknown" && !view.dimmed,
                "running without an adapter reads activity unknown, never busy or idle");

  push(4000, ++seq, Snap(1, 1, 0, 100));
  view = at(4000, "Codex and Claude running, no activity source");
  result.Expect(view.value == "2 running" && view.note == "Activity unknown",
                "several agents without an adapter stay activity unknown");
  push(4500, ++seq, Snap(1, 1, 1, 100));
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
  push(6000, ++seq, Snap(0, 2, 0, 100));
  view = at(6000, "future adapter: Claude busy");
  result.Expect(view.value == "1 running" && view.note == "Claude busy", "busy is labelled busy");
  push(6500, ++seq, Snap(0, 0, 3, 100));
  view = at(6500, "future adapter: Grok idle");
  result.Expect(view.value == "1 running" && view.note == "Grok idle", "idle is labelled idle");
  push(7000, ++seq, Snap(2, 3, 1, 100));
  at(7000, "future adapter: busy, idle and unknown together");
  push(7500, ++seq, Snap(2, 3, 0, 100));
  at(7500, "future adapter: one busy, one idle");

  // Freshness is derived on the device from the source age (15 s).
  push(20'000, ++seq, Snap(0, 1, 0, 14'999));
  view = at(20'000, "source sample 14.999 s old: fresh");
  result.Expect(!view.dimmed && view.value == "1 running", "just under 15 s is fresh");
  push(21'000, ++seq, Snap(0, 1, 0, 15'000));
  view = at(21'000, "source sample 15 s old in a new snapshot: stale");
  result.Expect(view.dimmed && view.value == "Last 1 running" && view.note == "Claude, unknown",
                "a re-sent old sample does not look fresh");
  // Frames keep coming (link live) but the sample keeps being re-sent.
  push(30'000, ++seq, Snap(0, 1, 0, 24'000));
  view = at(30'000, "the same old sample re-sent 9 s later: still stale");
  result.Expect(view.dimmed, "an aging re-sent sample stays stale");
  push(31'000, ++seq, Snap(0, 0, 0, 200));
  view = at(31'000, "refreshed sample: nothing running now");
  result.Expect(!view.dimmed && view.value == "None running", "a fresh sample is fresh");

  // The source ages while frames keep arriving (the Host re-sends the same
  // sample, sampled at 30.8 s): retained once it is 15 s old.
  push(45'000, ++seq, Snap(0, 0, 0, 14'200));
  view = at(45'000, "same sample re-sent, 14.2 s old: still fresh");
  result.Expect(!view.dimmed && view.value == "None running", "14.2 s old is fresh");
  push(46'000, ++seq, Snap(0, 0, 0, 15'200));
  view = at(46'000, "same sample re-sent, 15.2 s old: stale, retained as Last");
  result.Expect(view.dimmed && view.value == "Last none running",
                "a sample that ages past 15 s is retained and dimmed");

  // Frames stop: link lost retains the last reading, dimmed.
  push(50'000, ++seq, Snap(0, 1, 0, 100));
  at(52'900, "last frame 2.9 s ago: still live");
  view = at(85'000, "35 s without a frame: link stale, retained and dimmed");
  result.Expect(view.dimmed && view.value == "Last 1 running", "link loss retains with Last");

  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/agents-cards.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the transcript matches simulator/scenarios/agents-cards.expected "
                "(rerun with CYD_UPDATE_EXPECTED=1 to review an intended change)");
  return result.Finish("simulator agents-cards");
}

}  // namespace simulator::scenarios
