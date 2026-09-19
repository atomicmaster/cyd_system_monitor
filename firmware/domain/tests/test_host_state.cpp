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
    // 1,700,000,000,000 ms is 22:13:20 UTC. The Host's offset turns it into its
    // own local time; with no offset known the clock is UTC and always says so.
    using firmware::domain::DisplaySettings;
    struct Case {
      std::optional<int16_t> host_offset;
      DisplaySettings settings;
      const char* expected;
    };
    const DisplaySettings zone{.show_zone = true};
    const DisplaySettings twelve{.use_24h = false};
    const DisplaySettings twelve_zone{.use_24h = false, .show_zone = true};
    const Case cases[] = {
        {std::nullopt, {}, "22:13 UTC"},  // no offset: never passes as local
        {std::nullopt, twelve, "10:13 PM UTC"},
        {0, {}, "22:13"},  // Host says UTC: known, zone hidden
        {0, zone, "22:13 UTC"},
        {-300, {}, "17:13"},
        {-300, zone, "17:13 UTC-5"},
        {-300, twelve, "5:13 PM"},
        {-300, twelve_zone, "5:13 PM UTC-5"},
        {330, zone, "03:43 UTC+5:30"},
        {330, twelve, "3:43 AM"},
        {-570, zone, "12:43 UTC-9:30"},
        {840, zone, "12:13 UTC+14"},
        {-600, twelve, "12:13 PM"},  // noon reads 12, not 0
        {120, twelve, "12:13 AM"},   // just after midnight reads 12, not 0
        // A manual offset overrides the Host's, and is known even without one.
        {-300, {.manual_zone = true, .manual_offset_minutes = 60}, "23:13"},
        {std::nullopt,
         {.show_zone = true, .manual_zone = true, .manual_offset_minutes = -480},
         "14:13 UTC-8"},
        {std::nullopt, {.manual_zone = true, .manual_offset_minutes = 0}, "22:13"},
    };
    for (const Case& c : cases) {
      firmware::domain::HostStateReducer clock_state(1);
      clock_state.SetUtcAnchor(1'700'000'000'000ULL, 5000, c.host_offset);
      const auto view = firmware::domain::MakeCpuView(clock_state.state(), 5000, c.settings);
      if (view.clock != c.expected) {
        std::printf("clock: expected \"%s\" got \"%s\"\n", c.expected, view.clock.c_str());
        return 6;
      }
    }
    firmware::domain::HostStateReducer ticking(1);
    ticking.SetUtcAnchor(1'700'000'000'000ULL, 5000, -300);
    if (firmware::domain::MakeCpuView(ticking.state(), 5000 + 120'000, zone).clock != "17:15 UTC-5")
      return 7;
    // Crossing local midnight: 22:13 UTC + 3 h is 01:13 the next day.
    firmware::domain::HostStateReducer east(1);
    east.SetUtcAnchor(1'700'000'000'000ULL, 0, 180);
    if (firmware::domain::MakeCpuView(east.state(), 0, zone).clock != "01:13 UTC+3") return 8;
    // A later anchor without an offset reverts to UTC (the Host stopped sending it).
    east.SetUtcAnchor(1'700'000'000'000ULL, 0);
    if (firmware::domain::MakeCpuView(east.state(), 0).clock != "22:13 UTC") return 9;
    if (firmware::domain::MakeCpuView(firmware::domain::HostStateReducer(1).state(), 0).clock !=
        "Time unavailable")
      return 10;
  }
  std::puts("firmware_domain_host_state_tests: ok");
  return 0;
}
