// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/agents_bar.hpp"

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
namespace {

// Fixed wire slot order: codex, claude, grok.
constexpr const char* kNames[protocol::kAgentSlots] = {"Codex", "Claude", "Grok"};

}  // namespace

// Agents (Codex, Claude, Grok Build) with a same-user process, each as a
// badge naming what is known about it: "?" when there is no activity source
// (the MVP case), never a guessed busy or idle.
AgentsBarView MakeAgentsBar(const HostState& state, uint64_t /*now_ms*/) {
  AgentsBarView view{.message = "--"};
  const AgentsMetricState& agents = state.agents;
  if (!agents.has_data) return view;
  if (agents.availability != protocol::Availability::kAvailable) {
    view.message = std::string("-- ") + AvailabilityNote(agents.availability);
    return view;
  }
  const bool retained = state.LinkLost() || agents.freshness == protocol::Freshness::kStale;
  // The decoder refuses any state outside the enum.
  for (uint8_t slot = 0; slot < agents.state.size(); ++slot) {
    AgentBadge badge{.slot = slot, .label = kNames[slot]};
    switch (agents.state[slot]) {
      case protocol::AgentState::kNotRunning:
        continue;
      case protocol::AgentState::kRunning:
        badge.label += " ?";
        break;
      case protocol::AgentState::kBusy:
        badge.label += " busy";
        badge.activity = AgentActivity::kBusy;
        break;
      case protocol::AgentState::kIdle:
        badge.label += " idle";
        badge.activity = AgentActivity::kIdle;
        break;
    }
    view.badges.push_back(badge);
  }
  if (view.badges.empty()) {
    view.message = retained ? "Last none running" : "None running";
  } else {
    view.message = retained ? "Last" : "";
  }
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
