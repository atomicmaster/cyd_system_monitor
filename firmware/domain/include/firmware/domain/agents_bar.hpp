// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "firmware/domain/host_state.hpp"

namespace firmware::domain {

// What an agent is known to be doing. An MVP Host reports presence only, so a
// running agent is kUnknown; busy and idle need an activity adapter.
enum class AgentActivity : uint8_t { kUnknown, kBusy, kIdle };

// The wire slots, in the order the Host sends them (codex, claude and grok
// keep the slots they had; gemini and opencode were appended).
inline constexpr uint8_t kSlotCodex = 0;
inline constexpr uint8_t kSlotClaude = 1;
inline constexpr uint8_t kSlotGrok = 2;
inline constexpr uint8_t kSlotGemini = 3;
inline constexpr uint8_t kSlotOpenCode = 4;

// The slots left to right on the bar. This is not the wire order: Gemini sits
// between Claude and Grok.
inline constexpr std::array<uint8_t, 5> kAgentVisualOrder = {kSlotCodex, kSlotClaude, kSlotGemini,
                                                             kSlotGrok, kSlotOpenCode};
static_assert(kAgentVisualOrder.size() == protocol::kAgentSlots);

// One running agent in the Overview's bottom bar, drawn as its mascot.
struct AgentBadge {
  uint8_t slot = 0;  // wire slot (kSlot*; fixed mascot per slot)
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
