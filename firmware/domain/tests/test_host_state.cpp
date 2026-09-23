// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
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
  {
    // Storage freshness is derived on the device: I/O 3 s after receipt,
    // capacity 90 s after its *source* sample. A new snapshot that re-carries
    // an old capacity sample (large age) must not make capacity fresh.
    namespace proto = cyd::protocol;
    using firmware::domain::HostStateReducer;
    const auto snapshot = [&](uint32_t seq, uint32_t capacity_age, uint32_t io_age) {
      proto::Envelope env;
      env.type = proto::MessageType::kSnapshot;
      env.sequence = seq;
      const proto::CpuSnapshot c{proto::Availability::kAvailable, 1, 0, proto::Freshness::kFresh,
                                 0};
      const proto::StorageSnapshot g{proto::Availability::kAvailable, 500, 200, capacity_age,
                                     proto::Availability::kAvailable, 10,  20,  io_age};
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(env, c, nullptr, &g, &frame);
      proto::Decoded decoded;
      if (proto::Decode(frame, &decoded) != proto::Error::kNone || !decoded.has_storage)
        std::abort();
      return decoded;
    };
    HostStateReducer storage(1);
    storage.ApplySnapshot(snapshot(1, 1000, 0), 100'000, 0);
    const auto& s = storage.state().storage;
    if (!s.has_data || s.total_bytes != 500 || s.available_bytes != 200) return 11;
    storage.Advance(102'900);
    if (s.io_freshness != proto::Freshness::kFresh) return 12;
    storage.Advance(103'000);
    if (s.io_freshness != proto::Freshness::kStale ||
        s.capacity_freshness != proto::Freshness::kFresh)
      return 13;
    // Capacity sampled at 99,000; stale once 90 s old, i.e. at 189,000.
    storage.Advance(188'999);
    if (s.capacity_freshness != proto::Freshness::kFresh) return 14;
    storage.Advance(189'000);
    if (s.capacity_freshness != proto::Freshness::kStale) return 15;
    // A new snapshot with fresh I/O that still carries the same old capacity
    // sample (its age is now 91 s): I/O is live again, capacity stays stale.
    storage.ApplySnapshot(snapshot(2, 91'000, 0), 190'000, 0);
    if (s.io_freshness != proto::Freshness::kFresh ||
        s.capacity_freshness != proto::Freshness::kStale)
      return 16;
    // A genuinely refreshed capacity sample becomes fresh.
    storage.ApplySnapshot(snapshot(3, 500, 0), 191'000, 0);
    if (s.capacity_freshness != proto::Freshness::kFresh) return 17;
    // Just after boot a capacity sample older than the uptime is still stale.
    HostStateReducer early(1);
    early.ApplySnapshot(snapshot(1, 200'000, 0), 5'000, 0);
    if (early.state().storage.capacity_freshness != proto::Freshness::kStale) return 40;
  }
  {
    // Battery freshness is derived from the source ages: charge is stale 90 s
    // after it was sampled, condition 3 h after. A re-sent old sample must not
    // look fresh.
    namespace proto = cyd::protocol;
    using firmware::domain::HostStateReducer;
    const auto snapshot = [&](uint32_t seq, uint32_t charge_age, uint32_t condition_age) {
      proto::Envelope env;
      env.type = proto::MessageType::kSnapshot;
      env.sequence = seq;
      const proto::CpuSnapshot c{proto::Availability::kAvailable, 1, 0, proto::Freshness::kFresh,
                                 0};
      proto::BatterySnapshot g;
      g.charge_availability = proto::Availability::kAvailable;
      g.charge_per_mille = 870;
      g.state = 1;
      g.charge_age_ms = charge_age;
      g.condition_availability = proto::Availability::kAvailable;
      g.max_capacity_per_mille = 910;
      g.condition = 1;
      g.condition_age_ms = condition_age;
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(env, c, nullptr, nullptr, &g, &frame);
      proto::Decoded decoded;
      if (proto::Decode(frame, &decoded) != proto::Error::kNone || !decoded.has_battery)
        std::abort();
      return decoded;
    };
    constexpr uint64_t kHour = 3600'000;
    HostStateReducer reducer(1);
    reducer.ApplySnapshot(snapshot(1, 1000, 1000), 100'000, 0);
    const auto& b = reducer.state().battery;
    if (!b.has_data || b.charge_per_mille != 870 || b.max_capacity_per_mille != 910) return 18;
    // Charge sampled at 99,000: fresh until 189,000. Condition likewise 3 h on.
    reducer.Advance(188'999);
    if (b.charge_freshness != proto::Freshness::kFresh) return 19;
    reducer.Advance(189'000);
    if (b.charge_freshness != proto::Freshness::kStale ||
        b.condition_freshness != proto::Freshness::kFresh)
      return 20;
    reducer.Advance(99'000 + 3 * kHour - 1);
    if (b.condition_freshness != proto::Freshness::kFresh) return 21;
    reducer.Advance(99'000 + 3 * kHour);
    if (b.condition_freshness != proto::Freshness::kStale) return 22;
    // A new snapshot re-carrying the same old samples (ages now 91 s / 3 h):
    // neither becomes fresh merely because a snapshot arrived.
    const uint64_t later = 99'000 + 3 * kHour + 1000;
    reducer.ApplySnapshot(
        snapshot(2, static_cast<uint32_t>(later - 99'000), static_cast<uint32_t>(later - 99'000)),
        later, 0);
    if (b.charge_freshness != proto::Freshness::kStale ||
        b.condition_freshness != proto::Freshness::kStale)
      return 23;
    // A genuinely refreshed charge sample is fresh while condition stays stale.
    reducer.ApplySnapshot(snapshot(3, 500, static_cast<uint32_t>(later + 1000 - 99'000)),
                          later + 1000, 0);
    if (b.charge_freshness != proto::Freshness::kFresh ||
        b.condition_freshness != proto::Freshness::kStale)
      return 24;
    // Just after device boot, a 3 h old condition predates the device clock;
    // it must not be clamped to "sampled at boot" and look fresh.
    HostStateReducer booted(1);
    booted.ApplySnapshot(snapshot(1, 1000, 3 * kHour), 10'000, 0);
    if (booted.state().battery.condition_freshness != proto::Freshness::kStale) return 25;
  }
  {
    // Agents freshness comes from the source age: stale 15 s after the sample,
    // an old sample re-sent is still stale, and one older than device uptime
    // (just after boot) is stale rather than clamped to "sampled at boot".
    namespace proto = cyd::protocol;
    using firmware::domain::HostStateReducer;
    const auto snapshot = [&](uint32_t seq, uint32_t age) {
      proto::Envelope env;
      env.type = proto::MessageType::kSnapshot;
      env.sequence = seq;
      const proto::CpuSnapshot c{proto::Availability::kAvailable, 1, 0, proto::Freshness::kFresh,
                                 0};
      proto::AgentsSnapshot g;
      g.availability = proto::Availability::kAvailable;
      g.source_age_ms = age;
      g.state[1] = 1;
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(env, c, nullptr, nullptr, nullptr, &g, &frame);
      proto::Decoded decoded;
      if (proto::Decode(frame, &decoded) != proto::Error::kNone || !decoded.has_agents)
        std::abort();
      return decoded;
    };
    HostStateReducer reducer(1);
    reducer.ApplySnapshot(snapshot(1, 1000), 100'000, 0);
    const auto& a = reducer.state().agents;
    if (!a.has_data || a.state[1] != 1 || a.state[0] != 0) return 50;
    reducer.Advance(113'999);  // sampled at 99,000
    if (a.freshness != proto::Freshness::kFresh) return 51;
    reducer.Advance(114'000);
    if (a.freshness != proto::Freshness::kStale) return 52;
    reducer.ApplySnapshot(snapshot(2, 16'000), 115'000, 0);  // same old sample re-sent
    if (a.freshness != proto::Freshness::kStale) return 53;
    reducer.ApplySnapshot(snapshot(3, 200), 116'000, 0);
    if (a.freshness != proto::Freshness::kFresh) return 54;
    HostStateReducer booted(1);
    booted.ApplySnapshot(snapshot(1, 15'000), 5'000, 0);
    if (booted.state().agents.freshness != proto::Freshness::kStale) return 55;
  }
  {
    // Network freshness is per half and comes from each source age: traffic is
    // stale 3 s after its sample, WiFi context 15 s; a re-sent old sample stays
    // stale; one older than device uptime is stale, not clamped to "at boot".
    namespace proto = cyd::protocol;
    using firmware::domain::HostStateReducer;
    const auto snapshot = [&](uint32_t seq, uint32_t traffic_age, uint32_t context_age) {
      proto::Envelope env;
      env.type = proto::MessageType::kSnapshot;
      env.sequence = seq;
      const proto::CpuSnapshot c{proto::Availability::kAvailable, 1, 0, proto::Freshness::kFresh,
                                 0};
      proto::NetworkSnapshot g;
      g.traffic_availability = proto::Availability::kAvailable;
      g.traffic_age_ms = traffic_age;
      g.route_kind = proto::RouteKind::kWifi;
      g.rx_bytes_per_s = 1000;
      g.tx_bytes_per_s = 2000;
      g.context_availability = proto::AssociationContext::kAvailable;
      g.context_age_ms = context_age;
      g.coverage = proto::WifiCoverage::kObservable;
      g.channel = 6;
      std::vector<uint8_t> frame;
      proto::EncodeSnapshot(env, c, nullptr, nullptr, nullptr, nullptr, &g, &frame);
      proto::Decoded decoded;
      if (proto::Decode(frame, &decoded) != proto::Error::kNone || !decoded.has_network)
        std::abort();
      return decoded;
    };
    HostStateReducer reducer(1);
    reducer.ApplySnapshot(snapshot(1, 500, 1000), 100'000, 0);
    const auto& n = reducer.state().network;
    if (!n.has_data || n.rx_bytes_per_s != 1000 || n.channel != 6) return 60;
    reducer.Advance(102'499);  // traffic sampled at 99,500; context at 99,000
    if (n.traffic_freshness != proto::Freshness::kFresh) return 61;
    reducer.Advance(102'500);
    if (n.traffic_freshness != proto::Freshness::kStale ||
        n.context_freshness != proto::Freshness::kFresh)
      return 62;  // the halves age independently
    reducer.Advance(113'999);
    if (n.context_freshness != proto::Freshness::kFresh) return 63;
    reducer.Advance(114'000);
    if (n.context_freshness != proto::Freshness::kStale) return 64;
    reducer.ApplySnapshot(snapshot(2, 4'000, 20'000), 115'000, 0);  // old samples re-sent
    if (n.traffic_freshness != proto::Freshness::kStale ||
        n.context_freshness != proto::Freshness::kStale)
      return 65;
    reducer.ApplySnapshot(snapshot(3, 0, 200), 116'000, 0);
    if (n.traffic_freshness != proto::Freshness::kFresh ||
        n.context_freshness != proto::Freshness::kFresh)
      return 66;
    HostStateReducer booted(1);
    booted.ApplySnapshot(snapshot(1, 3'000, 15'000), 5'000, 0);
    if (booted.state().network.traffic_freshness != proto::Freshness::kStale ||
        booted.state().network.context_freshness != proto::Freshness::kStale)
      return 67;
  }
  std::puts("firmware_domain_host_state_tests: ok");
  return 0;
}
