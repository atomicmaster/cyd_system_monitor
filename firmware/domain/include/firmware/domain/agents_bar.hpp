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

// One running agent in the Overview's bottom bar, drawn as its mascot.
struct AgentBadge {
  uint8_t slot = 0;  // wire slot: 0 codex, 1 claude, 2 grok (fixed mascot per slot)
  AgentActivity activity = AgentActivity::kUnknown;
};

// The Agents bar: every agent's mascot in its fixed slot, lit when it is in
// `badges` (running) and a dim silhouette otherwise. `message` carries the
// only words: "--" before any data and "-- Read error" (or another cause) when
// processes cannot be listed, when no mascot is drawn so it never reads as
// "nothing running"; "Last" when a retained reading is shown; empty
// otherwise. A dimmed bar also stops its animations: a retained reading is not
// activity.
struct AgentsBarView {
  std::string message;
  std::vector<AgentBadge> badges;
  bool dimmed = false;
};

AgentsBarView MakeAgentsBar(const HostState& state, uint64_t now_ms);

}  // namespace firmware::domain
