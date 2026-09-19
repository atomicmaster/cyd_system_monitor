// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/cpu_view.hpp"

#include <cstdio>

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

std::string ClockText(const HostState& state, uint64_t now_ms) {
  if (!state.time_available) return "Time unavailable";
  const uint64_t elapsed =
      now_ms >= state.utc_anchor_monotonic_ms ? now_ms - state.utc_anchor_monotonic_ms : 0;
  const uint64_t seconds_of_day = ((state.utc_anchor_ms + elapsed) / 1000) % 86400;
  char text[16];
  std::snprintf(text, sizeof text, "%02u:%02u UTC", static_cast<unsigned>(seconds_of_day / 3600),
                static_cast<unsigned>(seconds_of_day / 60 % 60));
  return text;
}

}  // namespace

CpuView MakeCpuView(const HostState& state, uint64_t now_ms) {
  CpuView view;
  view.connection = ConnectionText(state.connection);
  view.clock = ClockText(state, now_ms);
  const bool measured =
      state.has_seen_host && state.cpu.availability == protocol::Availability::kAvailable;
  if (!measured) {
    view.value = "--";
    view.note = state.has_seen_host ? NoteText(state.cpu.availability) : "";
    return view;
  }
  view.has_value = true;
  view.bar_per_mille = state.cpu.value_per_mille;
  const bool retained = state.connection == ConnectionState::kStale ||
                        state.connection == ConnectionState::kIncompatible ||
                        state.connection == ConnectionState::kDisconnected ||
                        state.cpu.freshness == protocol::Freshness::kStale;
  char text[24];
  std::snprintf(text, sizeof text, "%s%u%%", retained ? "Last " : "",
                static_cast<unsigned>((state.cpu.value_per_mille + 5) / 10));
  view.value = text;
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
