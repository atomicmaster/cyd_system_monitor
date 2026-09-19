// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/link/link_screen.hpp"

namespace firmware::ui::system {

void LinkScreen::MakeButton(const char* text, lv_align_t align, Target* target) {
  lv_obj_t* button = lv_button_create(prompt_panel_);
  lv_obj_set_size(button, 120, 44);
  lv_obj_align(button, align, 0, -8);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  // The target lives in this LinkScreen, which also owns (and deletes) the
  // button, so the pointer never dangles.
  lv_obj_add_event_cb(
      button,
      [](lv_event_t* event) {
        auto* t = static_cast<Target*>(lv_event_get_user_data(event));
        if (t->screen->on_decision_) t->screen->on_decision_(t->accepted);
      },
      LV_EVENT_CLICKED, target);
}

LinkScreen::LinkScreen(lv_obj_t* parent, DecisionCallback on_decision)
    : system_(parent), on_decision_(std::move(on_decision)) {
  about_label_ = lv_label_create(parent);
  lv_obj_set_pos(about_label_, 8, 136);
  lv_obj_set_width(about_label_, 304);

  fault_label_ = lv_label_create(parent);
  lv_obj_set_pos(fault_label_, 8, 218);
  lv_obj_set_width(fault_label_, 304);
  lv_obj_set_style_text_color(fault_label_, lv_palette_main(LV_PALETTE_RED), 0);

  prompt_panel_ = lv_obj_create(parent);
  lv_obj_set_size(prompt_panel_, 296, 150);
  lv_obj_center(prompt_panel_);
  lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  prompt_label_ = lv_label_create(prompt_panel_);
  lv_obj_align(prompt_label_, LV_ALIGN_TOP_MID, 0, 4);
  confirm_target_ = {this, true};
  decline_target_ = {this, false};
  MakeButton("Confirm", LV_ALIGN_BOTTOM_LEFT, &confirm_target_);
  MakeButton("Decline", LV_ALIGN_BOTTOM_RIGHT, &decline_target_);
}

LinkScreen::~LinkScreen() {
  // Delete our own objects; the SystemScreen member removes its own tree.
  if (about_label_ != nullptr) lv_obj_delete(about_label_);
  if (fault_label_ != nullptr) lv_obj_delete(fault_label_);
  if (prompt_panel_ != nullptr) lv_obj_delete(prompt_panel_);
}

void LinkScreen::Update(const firmware::domain::CpuView& status,
                        std::span<const firmware::domain::MetricCardView> cards,
                        const firmware::domain::LinkView& link) {
  system_.Update(status, cards);
  lv_label_set_text(about_label_, link.about.c_str());
  lv_label_set_text(fault_label_, link.fault.c_str());
  if (link.prompt_visible) {
    lv_label_set_text(prompt_label_, link.prompt.c_str());
    lv_obj_remove_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace firmware::ui::system
