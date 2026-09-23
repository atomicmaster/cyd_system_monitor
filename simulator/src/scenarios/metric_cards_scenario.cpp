// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../expect.hpp"
#include "firmware/domain/card_registry.hpp"
#include "firmware/domain/cpu_view.hpp"
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

constexpr uint64_t kGiB = 1ULL << 30;

std::vector<uint8_t> Frame(uint32_t sequence, uint16_t cpu_per_mille,
                           const proto::RamSnapshot* ram) {
  proto::Envelope envelope;
  envelope.type = proto::MessageType::kSnapshot;
  envelope.sequence = sequence;
  for (uint8_t i = 0; i < envelope.session.size(); ++i) envelope.session[i] = i;
  const proto::CpuSnapshot cpu{proto::Availability::kAvailable, cpu_per_mille, 0,
                               proto::Freshness::kFresh, 0};
  std::vector<uint8_t> frame;
  proto::EncodeSnapshot(envelope, cpu, ram, &frame);
  return frame;
}

// One transcript line per registered card, in registry order.
void Report(std::ostringstream& out, const char* label, const domain::HostState& state,
            uint64_t now) {
  out << label << "\n";
  for (const auto& card : domain::Cards()) {
    const auto view = card.make(state, now);
    out << "  " << card.id << " | " << view.title << " | " << view.value << " | " << view.note
        << " | bar=" << view.bar_per_mille << " | dimmed=" << (view.dimmed ? 1 : 0) << "\n";
  }
}

}  // namespace

int RunMetricCards() {
  ScenarioResult result;
  ScriptedClock clock;
  domain::HostFeed feed(clock, 1);
  std::array<uint8_t, proto::kSessionIdBytes> session{};
  for (uint8_t i = 0; i < session.size(); ++i) session[i] = i;
  feed.StartSession(session);

  std::ostringstream transcript;
  const auto at = [&](uint64_t ms, const char* label) {
    clock.Set(ms);
    feed.Tick();
    Report(transcript, label, feed.state(), ms);
  };
  const auto push = [&](uint64_t ms, const std::vector<uint8_t>& frame) {
    clock.Set(ms);
    feed.Push(frame);
  };

  result.Expect(domain::Cards().size() == 5 && std::string(domain::Cards()[0].id) == "cpu" &&
                    std::string(domain::Cards()[1].id) == "ram" &&
                    std::string(domain::Cards()[2].id) == "storage" &&
                    std::string(domain::Cards()[3].id) == "battery" &&
                    std::string(domain::Cards()[4].id) == "agents",
                "the registry lists cpu, ram, storage, battery, then agents from "
                "card_registry.def");

  at(1000, "before any Host data");
  proto::RamSnapshot ram{proto::Availability::kAvailable, 8 * kGiB, 16 * kGiB, 250,
                         proto::Freshness::kFresh};
  push(2000, Frame(1, 500, &ram));
  at(2000, "live: cpu 50%, ram 8.0 of 16.0 GB");
  ram = {proto::Availability::kTemporarilyUnavailable, 0, 0, 0, proto::Freshness::kFresh};
  push(3000, Frame(2, 600, &ram));
  at(3000, "ram temporarily unavailable, cpu still live");
  ram = {proto::Availability::kAvailable, 12 * kGiB, 16 * kGiB, 0, proto::Freshness::kFresh};
  push(4000, Frame(3, 600, &ram));
  at(4000, "ram back: 12.0 of 16.0 GB");
  push(5000, Frame(4, 610, nullptr));
  at(6900, "ram group last sent 2.9 s ago: still fresh; cpu live");
  at(7100, "ram group last sent 3.1 s ago: ram goes stale, cpu still live");
  at(8100, "cpu frame 3.1 s ago: cpu goes stale too");
  at(20100, "15.1 s without any frame: both retained and dimmed");

  // The CPU card must equal the pre-registry CpuView path.
  const auto cpu_card = domain::Cards()[0].make(feed.state(), 20100);
  const auto cpu_view = domain::MakeCpuView(feed.state(), 20100);
  result.Expect(cpu_card.value == cpu_view.value && cpu_card.dimmed == cpu_view.dimmed &&
                    cpu_card.has_value == cpu_view.has_value,
                "the registered CPU card is exactly the CpuView value path");

  const std::string path = std::string(CYD_SIMULATOR_SCENARIO_DIR) + "/metric-cards.expected";
  if (std::getenv("CYD_UPDATE_EXPECTED") != nullptr) std::ofstream(path) << transcript.str();
  std::ifstream expected_file(path);
  std::stringstream expected;
  expected << expected_file.rdbuf();
  result.Expect(expected.str() == transcript.str(),
                "the card transcript matches simulator/scenarios/metric-cards.expected "
                "(rerun with CYD_UPDATE_EXPECTED=1 to review an intended change)");
  return result.Finish("simulator metric-cards");
}

}  // namespace simulator::scenarios
