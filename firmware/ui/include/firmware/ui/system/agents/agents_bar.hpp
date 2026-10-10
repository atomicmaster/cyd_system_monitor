// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <array>
#include <string>

#include "firmware/domain/agents_bar.hpp"
#include "protocol.hpp"

namespace firmware::ui::system {

// The Overview's bottom bar: each agent's mascot in its own third of the bar,
// in a fixed place, with three states:
//   not running        a dim silhouette;
//   running or idle    the mascot in its own colour, still;
//   busy               animated (the crab dances, Codex's cursor blinks, the
//                      Grok mark rocks).
// A dimmed bar is a retained reading, labelled "Last", and every mascot stops
// (docs/ui-design.md: animations decay to neutral when stale). When there is
// no reading ("--" and its cause) no mascot is drawn, so it never reads as
// nothing running. A non-empty notice (a link fault) replaces the agents, so
// the cause stays visible on the Overview.
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
  lv_obj_t* mascot(size_t slot) const { return mascots_[slot].image; }
  bool mascot_visible(size_t slot) const;
  // Drawn in its own colour (running), not as a silhouette or hidden.
  bool lit(size_t slot) const {
    return mascots_[slot].state == State::kStill || mascots_[slot].state == State::kBusy;
  }
  bool animating(size_t slot) const;

 private:
  enum class State : uint8_t { kHidden, kSilhouette, kStill, kBusy };
  struct Mascot {
    lv_obj_t* image = nullptr;
    State state = State::kHidden;
  };
  void Set(size_t slot, State state);

  lv_obj_t* root_ = nullptr;
  lv_obj_t* message_label_ = nullptr;
  lv_obj_t* notice_label_ = nullptr;
  std::array<Mascot, cyd::protocol::kAgentSlots> mascots_{};
  bool dimmed_ = false;
};

}  // namespace firmware::ui::system
