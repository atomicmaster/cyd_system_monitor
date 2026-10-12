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
// only the band/channel. `compact` is the short form for a line shared with
// other link icons ("ch6", "5/6G").
std::string WifiText(const NetworkMetricState& network, bool context_retained, bool compact) {
  if (context_retained) return "";
  switch (network.coverage) {
    case protocol::WifiCoverage::kObservable: {
      if (network.channel == 0) return "2.4G";
      char text[24];
      std::snprintf(text, sizeof text, compact ? "ch%u" : "ch%u 2.4G",
                    static_cast<unsigned>(network.channel));
      return text;
    }
    case protocol::WifiCoverage::kOutsideSupported:
      return compact ? "5/6G" : "5/6G unseen";
    case protocol::WifiCoverage::kUnknown:
      break;
  }
  return "";
}

LinkGlyph GlyphFor(protocol::RouteKind kind) {
  switch (kind) {
    case protocol::RouteKind::kWifi:
      return LinkGlyph::kWifi;
    case protocol::RouteKind::kEthernet:
      return LinkGlyph::kEthernet;
    default:
      return LinkGlyph::kOther;
  }
}

// The active physical links, one kind each: as the Host reports them, or for
// a Host that does not, the primary route's kind alone.
std::vector<protocol::RouteKind> Links(const NetworkMetricState& network) {
  if (network.links) return *network.links;
  switch (network.route_kind) {
    case protocol::RouteKind::kWifi:
    case protocol::RouteKind::kEthernet:
    case protocol::RouteKind::kOther:
      return {network.route_kind};
    case protocol::RouteKind::kUnknown:
    case protocol::RouteKind::kNone:
      break;
  }
  return {};
}

// The words after the icons: "+N" for icons that did not fit, "VPN" when
// the primary route is a tunnel, and the WiFi band when a WiFi link is up and
// the Host knows it. Ethernet and other links are named by their icons alone.
std::string LinkText(const NetworkMetricState& network, bool context_retained,
                     const std::vector<protocol::RouteKind>& links) {
  std::vector<std::string> parts;
  if (links.size() > kMaxLinkGlyphs)
    parts.push_back("+" + std::to_string(links.size() - kMaxLinkGlyphs));
  if (network.via_tunnel) parts.push_back("VPN");
  bool wifi = false;
  for (const auto kind : links) wifi = wifi || kind == protocol::RouteKind::kWifi;
  if (wifi) parts.push_back(WifiText(network, context_retained, links.size() > 1));
  if (links.empty() && network.route_kind == protocol::RouteKind::kUnknown) {
    parts.push_back("Link unknown");
  }
  std::string text;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    text += (text.empty() ? "" : " ") + part;
  }
  return text;
}

}  // namespace

// Network: the link line on top (one icon per active physical link, then the
// WiFi band when known, "VPN" for a tunnel), then the download and upload
// totals over those links as two rows with log-scale meters. Tunnel traffic,
// which also crosses a physical link, is never counted twice. An unavailable
// rate is `--` with the reason (a first sample or a joining link is
// unavailable, never 0); offline says so ("Last Offline", dimmed, once
// stale) beside any links still up. Retained rates keep their text, drop
// their meters, and the link line says "Last". The traffic and WiFi-context
// halves have their own freshness.
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
  const auto links = Links(network);
  for (size_t i = 0; i < links.size() && i < kMaxLinkGlyphs; ++i) {
    view.links.push_back(GlyphFor(links[i]));
  }
  const bool offline = network.route_kind == protocol::RouteKind::kNone;
  if (!offline && network.traffic_availability != protocol::Availability::kAvailable) {
    // `--` always carries its cause, beside the links that are up.
    view.note = AvailabilityNote(network.traffic_availability);
    return view;
  }
  std::string text = offline ? "Offline" : LinkText(network, context_retained, links);
  view.dimmed = traffic_retained;
  if (traffic_retained) {
    // "Last" leads; the icons give their width back to any other words.
    if (!text.empty()) view.links.clear();
    text = text.empty() ? "Last" : "Last " + text;
  }
  view.note = text;
  if (offline) return view;
  view.value = "";
  view.rates = RatePair{
      .first = CompactRate(network.rx_bytes_per_s),
      .second = CompactRate(network.tx_bytes_per_s),
      .first_per_mille = traffic_retained ? uint16_t{0} : LogRatePerMille(network.rx_bytes_per_s),
      .second_per_mille = traffic_retained ? uint16_t{0} : LogRatePerMille(network.tx_bytes_per_s)};
  view.has_value = true;
  return view;
}

}  // namespace firmware::domain
