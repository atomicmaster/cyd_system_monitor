// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

// The WiFi link after its glyph: only what the Host reported and only while
// it is fresh. A stale, absent or denied context adds nothing (the glyph
// alone says WiFi, never a guessed band), and a 5/6 GHz association says the
// board cannot see it ("unseen").
// Observable is not a promise of continuous capture, so the wording claims
// only the band/channel.
std::string WifiText(const NetworkMetricState& network, bool context_retained) {
  if (context_retained) return "";
  switch (network.coverage) {
    case protocol::WifiCoverage::kObservable: {
      if (network.channel == 0) return "2.4G";
      char text[24];
      std::snprintf(text, sizeof text, "ch%u 2.4G", static_cast<unsigned>(network.channel));
      return text;
    }
    case protocol::WifiCoverage::kOutsideSupported:
      return "5/6G unseen";
    case protocol::WifiCoverage::kUnknown:
      break;
  }
  return "";
}

std::string LinkText(const NetworkMetricState& network, bool context_retained) {
  std::string link;
  switch (network.route_kind) {
    case protocol::RouteKind::kWifi:
      link = WifiText(network, context_retained);
      break;
    case protocol::RouteKind::kEthernet:
      link = "Ethernet";
      break;
    case protocol::RouteKind::kOther:
      link = "Other link";
      break;
    case protocol::RouteKind::kNone:
      return "Offline";
    case protocol::RouteKind::kUnknown:
      link = "Link unknown";
      break;
  }
  // The tunnel's own rate, never summed with the link under it.
  if (!network.via_tunnel) return link;
  return link.empty() ? "VPN" : "VPN " + link;
}

}  // namespace

// Network: the link on top (a WiFi glyph and the Host WiFi coverage, or the
// link kind), then the primary-route interface's download and upload rates as
// two rows with log-scale meters. A tunnel primary route reports the tunnel's
// own rate labelled "VPN" and is never summed with the interface beneath it.
// An unavailable rate is `--` with the reason (a first sample or a route
// change is unavailable, never 0); offline says so ("Last Offline", dimmed,
// once stale). Retained rates keep their text, drop their meters, and the link
// line says "Last". The traffic and WiFi-context halves have their own
// freshness.
MetricCardView MakeNetworkCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{
      .title = "Network", .value = "--", .show_bar = false, .layout = CardLayout::kRates};
  const NetworkMetricState& network = state.network;
  if (!network.has_data) return view;
  const bool link_lost = state.LinkLost();
  const bool traffic_retained =
      link_lost || network.traffic_freshness == protocol::Freshness::kStale;
  const bool context_retained =
      link_lost || network.context_freshness == protocol::Freshness::kStale;
  if (network.route_kind == protocol::RouteKind::kNone) {
    // Offline is a reading like a rate: once stale or the link is lost it is
    // retained and dimmed as "Last Offline", never shown as current.
    view.note = traffic_retained ? "Last Offline" : "Offline";
    view.dimmed = traffic_retained;
    return view;
  }
  if (network.traffic_availability != protocol::Availability::kAvailable) {
    view.note = AvailabilityNote(network.traffic_availability);
    return view;
  }
  view.value = "";
  view.rates = RatePair{
      .first = CompactRate(network.rx_bytes_per_s),
      .second = CompactRate(network.tx_bytes_per_s),
      .first_per_mille = traffic_retained ? uint16_t{0} : LogRatePerMille(network.rx_bytes_per_s),
      .second_per_mille = traffic_retained ? uint16_t{0} : LogRatePerMille(network.tx_bytes_per_s)};
  const std::string link = LinkText(network, context_retained);
  // A retained line leads with "Last" and gives the glyph's width back to
  // the words, unless there are none: then the glyph stays to say WiFi.
  view.wifi =
      network.route_kind == protocol::RouteKind::kWifi && (!traffic_retained || link.empty());
  view.note = !traffic_retained ? link : link.empty() ? "Last" : "Last " + link;
  view.has_value = true;
  view.dimmed = traffic_retained;
  return view;
}

}  // namespace firmware::domain
