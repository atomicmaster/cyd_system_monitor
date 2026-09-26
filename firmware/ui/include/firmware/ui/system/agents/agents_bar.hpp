// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <array>
#include <string>

#include "firmware/domain/agents_bar.hpp"
#include "protocol.hpp"

namespace firmware::ui::system {

// The Overview's bottom bar: one badge per detected agent, each an animated
// icon in the agent's accent and a label. The icon's motion is its meaning,
// alongside the label's words:
//   activity unknown ("Claude ?")  a slowly pulsing dot;
//   busy                           a spinner orbiting the dot;
//   idle                           a still, hollow ring.
// A dimmed bar is a retained reading, so every icon stops (docs/ui-design.md:
// animations decay to neutral when stale). A non-empty notice (a link fault)
// replaces the agents, so the cause stays visible on the Overview.
class AgentsBar {
 public:
  AgentsBar(lv_obj_t* parent, int32_t width, int32_t height);
  ~AgentsBar();
  AgentsBar(const AgentsBar&) = delete;
  AgentsBar& operator=(const AgentsBar&) = delete;

  void Update(const firmware::domain::AgentsBarView& view, const std::string& notice = {});

  lv_obj_t* root() const { return root_; }
  lv_obj_t* message_label() const { return message_label_; }
  lv_obj_t* notice_label() const { return notice_label_; }
  bool dimmed() const { return dimmed_; }
  // Per wire slot (codex, claude, grok).
  bool badge_visible(size_t slot) const;
  lv_obj_t* badge_label(size_t slot) const { return badges_[slot].label; }
  bool pulsing(size_t slot) const;
  bool spinning(size_t slot) const;

 private:
  struct Badge {
    lv_obj_t* box = nullptr;
    lv_obj_t* dot = nullptr;
    lv_obj_t* spinner = nullptr;
    lv_obj_t* label = nullptr;
    bool shown = false;
    firmware::domain::AgentActivity activity = firmware::domain::AgentActivity::kUnknown;
    bool still = false;  // animations stopped (dimmed)
  };
  void Show(Badge& badge, firmware::domain::AgentActivity activity, bool still);

  lv_obj_t* root_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* message_label_ = nullptr;
  lv_obj_t* notice_label_ = nullptr;
  std::array<Badge, cyd::protocol::kAgentSlots> badges_{};
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
