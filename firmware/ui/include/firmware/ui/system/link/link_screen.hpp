// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>

#include "firmware/domain/cpu_view.hpp"
#include "firmware/domain/link_view.hpp"
#include "firmware/ui/system/cpu/cpu_card.hpp"

namespace firmware::ui::system {

// The CPU shell plus TB02's minimal pairing prompt and About/diagnostics
// text. Renders CpuView/LinkView only; the Confirm/Decline buttons report
// the touch decision to the caller, which owns the link.
class LinkScreen {
 public:
  using DecisionCallback = std::function<void(bool accepted)>;
  LinkScreen(lv_obj_t* parent, DecisionCallback on_decision);
  ~LinkScreen();
  LinkScreen(const LinkScreen&) = delete;
  LinkScreen& operator=(const LinkScreen&) = delete;

  void Update(const firmware::domain::CpuView& cpu, const firmware::domain::LinkView& link);

  lv_obj_t* prompt_panel() const { return prompt_panel_; }
  lv_obj_t* about_label() const { return about_label_; }
  lv_obj_t* fault_label() const { return fault_label_; }

 private:
  struct Target {
    LinkScreen* screen = nullptr;
    bool accepted = false;
  };
  void MakeButton(const char* text, lv_align_t align, Target* target);
  CpuScreen cpu_screen_;
  DecisionCallback on_decision_;
  Target confirm_target_;
  Target decline_target_;
  lv_obj_t* about_label_ = nullptr;
  lv_obj_t* fault_label_ = nullptr;
  lv_obj_t* prompt_panel_ = nullptr;
  lv_obj_t* prompt_label_ = nullptr;
};

}  // namespace firmware::ui::system
