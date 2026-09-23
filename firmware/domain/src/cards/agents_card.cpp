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

// Fixed wire slot order: codex, claude, grok.
constexpr const char* kNames[kAgentSlotCount] = {"Codex", "Claude", "Grok"};

// Wire states: 0 not running, 1 running with activity unknown, 2 busy, 3 idle.
// An MVP Host only ever sends 0 and 1; busy and idle need an activity adapter.
std::string NoteFor(const uint8_t (&state)[kAgentSlotCount]) {
  unsigned running = 0, unknown = 0, busy = 0, idle = 0;
  int only = -1;
  for (int i = 0; i < kAgentSlotCount; ++i) {
    if (state[i] == 0) continue;
    ++running;
    only = i;
    if (state[i] == 2)
      ++busy;
    else if (state[i] == 3)
      ++idle;
    else
      ++unknown;
  }
  if (running == 0) return "";
  if (running == 1) {
    // One agent: its name and what is known about it ("Claude, activity unknown"
    // does not fit 148 px at 12 pt).
    if (busy) return std::string(kNames[only]) + " busy";
    if (idle) return std::string(kNames[only]) + " idle";
    return std::string(kNames[only]) + ", unknown";
  }
  // Several agents: never a guessed activity. All unknown says so plainly;
  // otherwise the counts per known activity, with what is still unknown.
  if (unknown == running) return "Activity unknown";
  std::string note;
  const auto add = [&](unsigned count, const char* what) {
    if (count == 0) return;
    char part[24];
    std::snprintf(part, sizeof part, "%s%u %s", note.empty() ? "" : " ", count, what);
    note += part;
  };
  add(busy, "busy");
  add(idle, "idle");
  add(unknown, "unknown");
  return note;
}

}  // namespace

// Agents: how many supported agents (Codex, Claude, Grok Build) have a
// same-user process, with the activity known about them as the note. Running
// without an activity source is "activity unknown", never busy or idle. When
// the process listing itself is not available the value is `--` with the
// cause, never "None running". There is no bar: a count is not a proportion.
MetricCardView MakeAgentsCard(const HostState& state, uint64_t /*now_ms*/) {
  MetricCardView view{.title = "Agents", .value = "--", .show_bar = false};
  const AgentsMetricState& agents = state.agents;
  if (!agents.has_data) return view;
  if (agents.availability != protocol::Availability::kAvailable) {
    view.note = NoteText(agents.availability);
    return view;
  }
  bool valid = true;
  unsigned running = 0;
  for (const uint8_t s : agents.state) {
    if (s > 3) valid = false;
    if (s != 0) ++running;
  }
  if (!valid) {
    view.note = "Read error";
    return view;
  }
  const bool link_lost = state.connection == ConnectionState::kStale ||
                         state.connection == ConnectionState::kIncompatible ||
                         state.connection == ConnectionState::kDisconnected;
  const bool retained = link_lost || agents.freshness == protocol::Freshness::kStale;
  char value[32];
  if (running == 0) {
    std::snprintf(value, sizeof value, "%s", retained ? "Last none running" : "None running");
  } else {
    std::snprintf(value, sizeof value, "%s%u running", retained ? "Last " : "", running);
  }
  view.value = value;
  view.note = NoteFor(agents.state);
  view.has_value = true;
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
