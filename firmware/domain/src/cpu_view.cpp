// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/cpu_view.hpp"

#include <cstdio>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

const char* ConnectionText(ConnectionState state) {
  switch (state) {
    case ConnectionState::kNegotiating:
      return "Waiting for Host";
    case ConnectionState::kLive:
      return "Connected";
    case ConnectionState::kStale:
      return "Stale";
    case ConnectionState::kDisconnected:
      return "Disconnected";
    case ConnectionState::kIncompatible:
      return "Incompatible Host";
  }
  return "";
}

// The clock text. The offset is the manual one if chosen, else the Host's own;
// with neither, the time is UTC and says so, so it can never pass as local.
// Otherwise the zone suffix follows the setting: "5:13 PM", "17:13 UTC-5".
std::string ClockText(const HostState& state, uint64_t now_ms, const DisplaySettings& settings) {
  if (!state.time_available) return "Time unavailable";
  const uint64_t elapsed =
      now_ms >= state.utc_anchor_monotonic_ms ? now_ms - state.utc_anchor_monotonic_ms : 0;
  const bool known_zone = settings.manual_zone || state.has_utc_offset;
  const int64_t offset_minutes = settings.manual_zone
                                     ? settings.manual_offset_minutes
                                     : (state.has_utc_offset ? state.utc_offset_minutes : 0);
  const int64_t local_seconds =
      static_cast<int64_t>((state.utc_anchor_ms + elapsed) / 1000) + offset_minutes * 60;
  const int64_t seconds_of_day = ((local_seconds % 86400) + 86400) % 86400;
  const unsigned hour = static_cast<unsigned>(seconds_of_day / 3600);
  const unsigned minute = static_cast<unsigned>(seconds_of_day / 60 % 60);

  char time[16];
  if (settings.use_24h) {
    std::snprintf(time, sizeof time, "%02u:%02u", hour, minute);
  } else {
    std::snprintf(time, sizeof time, "%u:%02u %s", hour % 12 == 0 ? 12 : hour % 12, minute,
                  hour < 12 ? "AM" : "PM");
  }
  if (known_zone && !settings.show_zone) return time;

  char zone[16] = "UTC";
  if (offset_minutes != 0) {
    const int64_t magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    if (magnitude % 60 == 0) {
      std::snprintf(zone, sizeof zone, "UTC%c%d", offset_minutes < 0 ? '-' : '+',
                    static_cast<int>(magnitude / 60));
    } else {
      std::snprintf(zone, sizeof zone, "UTC%c%d:%02d", offset_minutes < 0 ? '-' : '+',
                    static_cast<int>(magnitude / 60), static_cast<int>(magnitude % 60));
    }
  }
  return std::string(time) + " " + zone;
}

}  // namespace

CpuView MakeCpuView(const HostState& state, uint64_t now_ms, const DisplaySettings& settings) {
  CpuView view;
  view.connection = ConnectionText(state.connection);
  view.clock = ClockText(state, now_ms, settings);
  const bool measured =
      state.has_seen_host && state.cpu.availability == protocol::Availability::kAvailable;
  if (!measured) {
    view.value = "--";
    view.note = state.has_seen_host ? AvailabilityNote(state.cpu.availability) : "";
    return view;
  }
  view.has_value = true;
  view.bar_per_mille = state.cpu.value_per_mille;
  const bool retained = state.LinkLost() || state.cpu.freshness == protocol::Freshness::kStale;
  char text[24];
  std::snprintf(text, sizeof text, "%s%u%%", retained ? "Last " : "",
                static_cast<unsigned>((state.cpu.value_per_mille + 5) / 10));
  view.value = text;
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
