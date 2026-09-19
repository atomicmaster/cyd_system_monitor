// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <vector>

#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/host_state.hpp"

int main() {
  cyd::protocol::Envelope e;
  e.type = cyd::protocol::MessageType::kSnapshot;
  e.sequence = 1;
  cyd::protocol::CpuSnapshot cpu{cyd::protocol::Availability::kAvailable, 500, 7000,
                                 cyd::protocol::Freshness::kFresh, 2};
  std::vector<uint8_t> wire;
  cyd::protocol::EncodeCpuSnapshot(e, cpu, &wire);
  cyd::protocol::Decoded d;
  if (cyd::protocol::Decode(wire, &d) != cyd::protocol::Error::kNone) return 1;
  firmware::domain::HostStateReducer state(42);
  state.ApplySnapshot(d, 1000, 900);
  state.Advance(4000);
  if (state.state().connection != firmware::domain::ConnectionState::kStale) return 2;
  state.Advance(16000);
  if (state.state().connection != firmware::domain::ConnectionState::kDisconnected) return 3;
  firmware::domain::AlertEpisode alert{.first_evidence_order = 9};
  firmware::domain::Acknowledge(&alert);
  if (!alert.acknowledged || alert.state != firmware::domain::EpisodeState::kActive) return 4;
  firmware::domain::InterruptForRecovery(&alert);
  if (alert.state != firmware::domain::EpisodeState::kInterrupted) return 5;
  {
    // 1,700,000,000,000 ms is 22:13:20 UTC. The Host's offset turns it into
    // its own local time; a missing offset keeps showing UTC.
    struct Case {
      std::optional<int16_t> offset;
      const char* expected;
    };
    const Case cases[] = {{std::nullopt, "22:13 UTC"}, {0, "22:13 UTC"},
                          {-300, "17:13 UTC-5"},       {330, "03:43 UTC+5:30"},
                          {-570, "12:43 UTC-9:30"},    {840, "12:13 UTC+14"}};
    for (const Case& c : cases) {
      firmware::domain::HostStateReducer clock_state(1);
      clock_state.SetUtcAnchor(1'700'000'000'000ULL, 5000, c.offset);
      const auto view = firmware::domain::MakeCpuView(clock_state.state(), 5000);
      if (view.clock != c.expected) {
        std::printf("clock: expected \"%s\" got \"%s\"\n", c.expected, view.clock.c_str());
        return 6;
      }
    }
    firmware::domain::HostStateReducer ticking(1);
    ticking.SetUtcAnchor(1'700'000'000'000ULL, 5000, -300);
    if (firmware::domain::MakeCpuView(ticking.state(), 5000 + 120'000).clock != "17:15 UTC-5")
      return 7;
    // Crossing local midnight: 22:13 UTC + 3 h is 01:13 the next day.
    firmware::domain::HostStateReducer east(1);
    east.SetUtcAnchor(1'700'000'000'000ULL, 0, 180);
    if (firmware::domain::MakeCpuView(east.state(), 0).clock != "01:13 UTC+3") return 8;
    // A later anchor without an offset reverts to UTC (the Host stopped sending it).
    east.SetUtcAnchor(1'700'000'000'000ULL, 0);
    if (firmware::domain::MakeCpuView(east.state(), 0).clock != "22:13 UTC") return 9;
  }
  std::puts("firmware_domain_host_state_tests: ok");
  return 0;
}
