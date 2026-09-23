// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

const char* NoteText(protocol::Availability availability) {
  switch (availability) {
    case protocol::Availability::kAvailable:
      return "";
    case protocol::Availability::kUnsupported:
      return "Unsupported";
    case protocol::Availability::kAbsent:
      return "Not present";
    case protocol::Availability::kTemporarilyUnavailable:
      return "Unavailable";
    case protocol::Availability::kError:
      return "Read error";
  }
  return "";
}

// One rate in at most five ASCII characters ("812B", "1.2K", "340K", "1.2M",
// "12M", "999G"), so "Last D999M U999M" stays inside 148 px at 14 pt. No arrow
// glyphs: the built-in font's arrows are not guaranteed, and D/U read clearly.
std::string RateText(uint64_t bytes_per_s) {
  constexpr double kClamp = 999.4e9;
  const double value =
      bytes_per_s > static_cast<uint64_t>(kClamp) ? kClamp : static_cast<double>(bytes_per_s);
  char text[16];
  if (value < 1e3) {
    std::snprintf(text, sizeof text, "%uB", static_cast<unsigned>(value));
    return text;
  }
  const auto scaled = [&](double x, char unit) {
    std::snprintf(text, sizeof text, x < 9.95 ? "%.1f%c" : "%.0f%c", x, unit);
  };
  if (value < 999.5e3) {
    scaled(value / 1e3, 'K');
  } else if (value < 999.5e6) {
    scaled(value / 1e6, 'M');
  } else {
    scaled(value / 1e9, 'G');
  }
  return text;
}

// The WiFi half of the note. Only what the Host reported and only while it is
// fresh: a stale, absent or denied context is "unknown" (never a guess), and
// a 5/6 GHz association says the board cannot capture it. Observable is not a
// promise of continuous capture, so the wording claims only the band/channel.
// `compact` is used behind the "VPN " prefix, where the full wording would not
// fit 148 px at 12 pt.
std::string WifiNote(const NetworkMetricState& network, bool context_retained, bool compact) {
  if (context_retained) return "WiFi context ?";
  switch (network.coverage) {
    case protocol::WifiCoverage::kObservable: {
      const char* band = compact ? "2.4G" : "2.4GHz";
      if (network.channel == 0) return std::string("WiFi ") + band;
      char text[40];
      std::snprintf(text, sizeof text, "WiFi ch%u %s", static_cast<unsigned>(network.channel),
                    band);
      return text;
    }
    case protocol::WifiCoverage::kOutsideSupported:
      return compact ? "5/6GHz: no capture" : "WiFi 5/6GHz: no capture";
    case protocol::WifiCoverage::kUnknown:
      break;
  }
  return "WiFi context ?";
}

std::string RouteNote(const NetworkMetricState& network, bool context_retained) {
  std::string note;
  switch (network.route_kind) {
    case protocol::RouteKind::kWifi:
      note = WifiNote(network, context_retained, network.via_tunnel);
      break;
    case protocol::RouteKind::kEthernet:
      note = "Ethernet";
      break;
    case protocol::RouteKind::kOther:
      note = "Other link";
      break;
    case protocol::RouteKind::kNone:
      return "Offline";
    case protocol::RouteKind::kUnknown:
      note = "Link unknown";
      break;
  }
  // The tunnel's own rate, never summed with the link under it.
  return network.via_tunnel ? "VPN " + note : note;
}

}  // namespace

// Network: the primary-route interface's download and upload rates as the
// value ("D1.2M U340K"), the link and Host WiFi coverage as the note. A tunnel
// primary route reports the tunnel's own rate labelled "VPN" and is never
// summed with the interface beneath it. An unavailable rate is `--` with the
// reason (a first sample or a route change is unavailable, never 0); offline
// says so. The traffic and WiFi-context halves have their own freshness; a
// stale context reads "WiFi context ?" while live rates continue. No bar: a
// rate is not a proportion.
MetricCardView MakeNetworkCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Network", .value = "--", .show_bar = false};
  const NetworkMetricState& network = state.network;
  if (!network.has_data) return view;
  const bool link_lost = state.connection == ConnectionState::kStale ||
                         state.connection == ConnectionState::kIncompatible ||
                         state.connection == ConnectionState::kDisconnected;
  const bool traffic_retained =
      link_lost || network.traffic_freshness == protocol::Freshness::kStale;
  const bool context_retained =
      link_lost || network.context_freshness == protocol::Freshness::kStale;
  if (network.route_kind == protocol::RouteKind::kNone) {
    view.note = "Offline";
    return view;
  }
  if (network.traffic_availability != protocol::Availability::kAvailable) {
    view.note = NoteText(network.traffic_availability);
    return view;
  }
  view.value = std::string(traffic_retained ? "Last " : "") + "D" +
               RateText(network.rx_bytes_per_s) + " U" + RateText(network.tx_bytes_per_s);
  view.note = RouteNote(network, context_retained);
  view.has_value = true;
  view.dimmed = traffic_retained;
  return view;
}

}  // namespace firmware::domain
