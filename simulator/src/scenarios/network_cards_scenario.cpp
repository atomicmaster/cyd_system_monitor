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

constexpr std::array<uint8_t, 6> kStation = {0xf2, 0xa1, 0xb3, 0xc4, 0xd5, 0xe6};

// A live primary route with rates, no WiFi context, station identity present:
// what a Host without a provider reports.
proto::NetworkSnapshot Base(proto::RouteKind kind, uint64_t rx, uint64_t tx) {
  proto::NetworkSnapshot g;
  g.traffic_availability = kOk;
  g.route_kind = kind;
  g.rx_bytes_per_s = rx;
  g.tx_bytes_per_s = tx;
  g.context_availability = proto::AssociationContext::kPermissionNotRequested;
  if (kind == proto::RouteKind::kWifi) g.station = kStation;
  return g;
}

proto::NetworkSnapshot Wifi(uint8_t channel, proto::WifiCoverage coverage, uint32_t traffic_age_ms,
                            uint32_t context_age_ms) {
  auto g = Base(proto::RouteKind::kWifi, 1'200'000, 340'000);
  g.traffic_age_ms = traffic_age_ms;
  g.context_availability = proto::AssociationContext::kAvailable;
  g.context_age_ms = context_age_ms;
  g.coverage = coverage;
  g.channel = channel;
  g.ssid = {'C', 'Y', 'D'};
  g.bssid = std::array<uint8_t, 6>{1, 2, 3, 4, 5, 6};
  return g;
}

}  // namespace

int RunNetworkCards() {
  CardScenario scenario("network", "network-cards", CardScenario::Columns::kShowBar);
  if (!scenario.ready()) return scenario.Finish();
  ScenarioResult& result = scenario.result();
  const auto at = [&](uint64_t ms, const char* label) { return scenario.At(ms, label); };
  const auto push = [&](uint64_t ms, uint32_t sequence, const proto::NetworkSnapshot& network) {
    scenario.Push(ms, sequence, {.network = network});
  };

  uint32_t seq = 0;
  auto view = at(1000, "no Host data yet");
  result.Expect(view.value == "--" && !view.has_value && !view.show_bar && view.title == "Network",
                "no data is -- and has no bar");

  // WiFi primary route with rates and full context.
  push(2000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 0, 300));
  view = at(2000, "WiFi live with rates, channel 6");
  result.Expect(view.value == "D1.2M U340K" && view.note == "WiFi ch6 2.4GHz" && view.has_value &&
                    !view.dimmed && !view.show_bar,
                "live WiFi rates with the observable channel note");

  // Ethernet.
  push(3000, ++seq, Base(proto::RouteKind::kEthernet, 45'000'000, 2'500'000));
  view = at(3000, "Ethernet primary route");
  result.Expect(view.value == "D45M U2.5M" && view.note == "Ethernet" && !view.dimmed,
                "Ethernet shows its rates and no WiFi wording");

  // A tunnel as primary route: the tunnel's own rate, labelled, never summed
  // with the WiFi link beneath it.
  auto tunnel = Wifi(6, proto::WifiCoverage::kObservable, 0, 300);
  tunnel.via_tunnel = true;
  tunnel.rx_bytes_per_s = 900'000;  // the tunnel's own, below the WiFi link's
  tunnel.tx_bytes_per_s = 100'000;
  push(4000, ++seq, tunnel);
  view = at(4000, "tunnel primary route over WiFi");
  result.Expect(view.value == "D900K U100K" && view.note == "VPN WiFi ch6 2.4G",
                "the tunnel's own rate is labelled VPN, not summed");

  // A route change: the rate is unavailable, not zero, then recovers.
  auto changing = Base(proto::RouteKind::kEthernet, 0, 0);
  changing.traffic_availability = proto::Availability::kTemporarilyUnavailable;
  push(5000, ++seq, changing);
  view = at(5000, "route changed: rate re-baselining");
  result.Expect(view.value == "--" && view.note == "Unavailable" && !view.has_value && !view.dimmed,
                "a re-baselining rate is -- with its cause, never zero");
  push(6000, ++seq, Base(proto::RouteKind::kEthernet, 800, 0));
  view = at(6000, "route settled: rate back");
  result.Expect(view.value == "D800B U0B" && view.has_value,
                "the rate returns; a measured idle direction is a real zero");

  // 5 GHz: outside the board's supported coverage.
  push(7000, ++seq, Wifi(149, proto::WifiCoverage::kOutsideSupported, 0, 100));
  view = at(7000, "WiFi on channel 149 (5 GHz)");
  result.Expect(view.note == "WiFi 5/6GHz: no capture" && view.value == "D1.2M U340K",
                "5/6 GHz says the board cannot capture it");
  push(7500, ++seq, Wifi(0, proto::WifiCoverage::kOutsideSupported, 0, 100));
  at(7500, "WiFi 5/6 GHz, channel unknown");

  // Context unknown: provider connected but nothing usable.
  auto unknown = Base(proto::RouteKind::kWifi, 50'000, 8'000);
  unknown.context_availability = proto::AssociationContext::kAuthorizedUnavailable;
  push(8000, ++seq, unknown);
  view = at(8000, "context unknown (authorized but unavailable)");
  result.Expect(view.note == "WiFi context ?" && view.value == "D50K U8.0K",
                "unknown context is explicit and never a guess");

  // Permission denied still shows a channel the provider could read without
  // Location; without one it is unknown.
  auto denied = Base(proto::RouteKind::kWifi, 50'000, 8'000);
  denied.context_availability = proto::AssociationContext::kDenied;
  denied.coverage = proto::WifiCoverage::kObservable;
  denied.channel = 11;
  push(9000, ++seq, denied);
  view = at(9000, "permission denied, channel still known");
  result.Expect(view.note == "WiFi ch11 2.4GHz", "denied permission does not hide coverage");
  denied.coverage = proto::WifiCoverage::kUnknown;
  denied.channel = 0;
  push(9500, ++seq, denied);
  view = at(9500, "permission denied, no channel");
  result.Expect(view.note == "WiFi context ?", "denied with no band is unknown");

  // Provider absent: no context source at all; traffic still works.
  push(10'000, ++seq, Base(proto::RouteKind::kWifi, 2'000, 1'000));
  view = at(10'000, "provider absent, station identity present");
  result.Expect(view.note == "WiFi context ?" && view.value == "D2.0K U1.0K",
                "an absent provider leaves traffic live and context unknown");

  // Offline and unreadable.
  proto::NetworkSnapshot offline;
  offline.traffic_availability = proto::Availability::kAbsent;
  offline.route_kind = proto::RouteKind::kNone;
  push(11'000, ++seq, offline);
  view = at(11'000, "offline: no primary route");
  result.Expect(view.value == "--" && view.note == "Offline" && !view.has_value,
                "offline is -- and says Offline");
  proto::NetworkSnapshot broken;
  broken.traffic_availability = proto::Availability::kError;
  push(11'500, ++seq, broken);
  view = at(11'500, "traffic read error");
  result.Expect(view.value == "--" && view.note == "Read error", "an error is -- with its cause");

  // Freshness per half: traffic is stale at 3 s, WiFi context at 15 s.
  push(20'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 2'999, 14'999));
  view = at(20'000, "traffic 2.999 s old, context 14.999 s old: both fresh");
  result.Expect(!view.dimmed && view.note == "WiFi ch6 2.4GHz", "just under the limits is fresh");
  push(21'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 3'000, 1'000));
  view = at(21'000, "traffic 3 s old while the WiFi context is live");
  result.Expect(view.dimmed && view.value == "Last D1.2M U340K" && view.note == "WiFi ch6 2.4GHz",
                "stale traffic is retained as Last; the live context still reads");
  push(22'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 0, 15'000));
  view = at(22'000, "traffic live while the WiFi context is 15 s old");
  result.Expect(!view.dimmed && view.value == "D1.2M U340K" && view.note == "WiFi context ?",
                "a stale context reads unknown while live rates continue");
  push(23'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 4'000, 16'000));
  view = at(23'000, "old samples re-sent in a new snapshot: both stale");
  result.Expect(view.dimmed && view.note == "WiFi context ?",
                "a re-sent old sample does not look fresh");
  push(24'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 100, 200));
  view = at(24'000, "fresh samples again");
  result.Expect(!view.dimmed && view.note == "WiFi ch6 2.4GHz", "fresh samples are fresh");

  // Frames stop: the link is lost and the last reading is retained, dimmed.
  push(50'000, ++seq, Wifi(6, proto::WifiCoverage::kObservable, 0, 0));
  at(52'900, "last frame 2.9 s ago: still live");
  view = at(85'000, "35 s without a frame: link lost");
  result.Expect(view.dimmed && view.value == "Last D1.2M U340K" && view.note == "WiFi context ?",
                "link loss retains rates as Last and drops the context to unknown");

  // Offline goes stale like any reading: a stale route sample or a lost link
  // retains it dimmed as "Last Offline", never a live-looking "Offline".
  push(90'000, ++seq, offline);
  view = at(90'000, "offline again, fresh");
  result.Expect(view.note == "Offline" && !view.dimmed, "a fresh offline route is not dimmed");
  auto old_offline = offline;
  old_offline.traffic_age_ms = 3'000;
  push(91'000, ++seq, old_offline);
  view = at(91'000, "offline sample 3 s old");
  result.Expect(view.value == "--" && view.note == "Last Offline" && view.dimmed && !view.has_value,
                "a stale offline sample is retained as Last Offline, dimmed");
  push(92'000, ++seq, offline);
  view = at(126'000, "offline, then 34 s without a frame: link lost");
  result.Expect(view.note == "Last Offline" && view.dimmed,
                "link loss retains offline as Last Offline, dimmed");

  return scenario.Finish();
}

}  // namespace simulator::scenarios
