// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

using AgentStates = std::array<protocol::AgentState, protocol::kAgentSlots>;

// Fixed wire slot order: codex, claude, grok.
constexpr const char* kNames[protocol::kAgentSlots] = {"Codex", "Claude", "Grok"};

// An MVP Host only ever sends not running and running; busy and idle need an
// activity adapter.
std::string NoteFor(const AgentStates& state) {
  unsigned running = 0, unknown = 0, busy = 0, idle = 0;
  std::size_t only = 0;
  for (std::size_t i = 0; i < state.size(); ++i) {
    if (state[i] == protocol::AgentState::kNotRunning) continue;
    ++running;
    only = i;
    if (state[i] == protocol::AgentState::kBusy)
      ++busy;
    else if (state[i] == protocol::AgentState::kIdle)
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
    view.note = AvailabilityNote(agents.availability);
    return view;
  }
  // The decoder refuses any state outside the enum.
  unsigned running = 0;
  for (const protocol::AgentState s : agents.state) {
    if (s != protocol::AgentState::kNotRunning) ++running;
  }
  const bool link_lost = state.LinkLost();
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
