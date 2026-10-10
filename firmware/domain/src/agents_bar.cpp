// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/agents_bar.hpp"

#include "firmware/domain/card_registry.hpp"

namespace firmware::domain {
// Agents (Codex, Claude, Grok Build) with a same-user process, each with what
// is known about its activity: unknown when the Host has no activity source
// for it, never a guessed busy or idle.
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
    AgentBadge badge{.slot = slot};
    switch (agents.state[slot]) {
      case protocol::AgentState::kNotRunning:
        continue;
      case protocol::AgentState::kRunning:
        break;
      case protocol::AgentState::kBusy:
        badge.activity = AgentActivity::kBusy;
        break;
      case protocol::AgentState::kIdle:
        badge.activity = AgentActivity::kIdle;
        break;
    }
    view.badges.push_back(badge);
  }
  view.message = retained ? "Last" : "";
  view.dimmed = retained;
  return view;
}

}  // namespace firmware::domain
