// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "firmware/domain/host_state.hpp"

namespace firmware::domain {

// What an agent is known to be doing. An MVP Host reports presence only, so a
// running agent is kUnknown; busy and idle need an activity adapter.
enum class AgentActivity : uint8_t { kUnknown, kBusy, kIdle };

// One detected agent in the Overview's bottom bar.
struct AgentBadge {
  uint8_t slot = 0;   // wire slot: 0 codex, 1 claude, 2 grok (fixed accent per slot)
  std::string label;  // "Claude ?", "Claude busy", "Claude idle"
  AgentActivity activity = AgentActivity::kUnknown;
};

// The Agents bar: one badge per detected agent, in slot order. `message`
// comes first and carries everything that is not a badge: "--" before any
// data, "-- Read error" when processes cannot be listed (never "None
// running"), "None running", and "Last" when a retained reading is shown.
// A dimmed bar also stops its animations: a retained reading is not activity.
struct AgentsBarView {
  std::string message;
  std::vector<AgentBadge> badges;
  bool dimmed = false;
};

AgentsBarView MakeAgentsBar(const HostState& state, uint64_t now_ms);

}  // namespace firmware::domain
