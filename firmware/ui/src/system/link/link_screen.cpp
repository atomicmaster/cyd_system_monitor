// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/link/link_screen.hpp"

#include "firmware/ui/theme.hpp"

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

LinkScreen::LinkScreen(lv_obj_t* parent, DecisionCallback on_decision,
                       SettingsPanel::ChangeCallback on_settings,
                       const firmware::domain::DisplaySettings& initial,
                       SettingsPanel::Actions settings_actions)
    : system_(parent), settings_(initial), on_decision_(std::move(on_decision)) {
  // Host link details: an overlay, so versions never cover the Overview cards.
  about_panel_ = lv_obj_create(parent);
  lv_obj_set_size(about_panel_, 308, 232);
  theme::StylePanel(about_panel_);
  lv_obj_center(about_panel_);
  lv_obj_set_flex_flow(about_panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(about_panel_, 8, 0);
  lv_obj_set_style_pad_row(about_panel_, 6, 0);
  lv_obj_t* about_title = lv_label_create(about_panel_);
  theme::StyleCaption(about_title);
  lv_label_set_text(about_title, "Host link");
  about_label_ = lv_label_create(about_panel_);
  lv_obj_set_width(about_label_, LV_PCT(100));
  fault_label_ = lv_label_create(about_panel_);
  lv_obj_set_width(fault_label_, LV_PCT(100));
  about_done_ = lv_button_create(about_panel_);
  lv_obj_set_size(about_done_, 96, 40);
  lv_obj_t* done_label = lv_label_create(about_done_);
  lv_label_set_text(done_label, "Done");
  lv_obj_center(done_label);
  lv_obj_add_event_cb(
      about_done_,
      [](lv_event_t* event) {
        lv_obj_add_flag(static_cast<lv_obj_t*>(lv_event_get_user_data(event)), LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, about_panel_);
  lv_obj_add_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);

  prompt_panel_ = lv_obj_create(parent);
  lv_obj_set_size(prompt_panel_, 296, 150);
  theme::StylePanel(prompt_panel_);
  lv_obj_center(prompt_panel_);
  lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  prompt_label_ = lv_label_create(prompt_panel_);
  lv_obj_align(prompt_label_, LV_ALIGN_TOP_MID, 0, 4);
  confirm_target_ = {this, true};
  decline_target_ = {this, false};
  MakeButton("Confirm", LV_ALIGN_BOTTOM_LEFT, &confirm_target_);
  MakeButton("Decline", LV_ALIGN_BOTTOM_RIGHT, &decline_target_);

  settings_panel_ = std::make_unique<SettingsPanel>(
      parent,
      [this, on_settings = std::move(on_settings)](const firmware::domain::DisplaySettings& s) {
        settings_ = s;
        if (on_settings) on_settings(s);
      },
      std::move(settings_actions));
  // The pairing prompt has priority: it must never be hidden behind either panel.
  system_.SetStatusBarTapCallback([this]() {
    if (!lv_obj_has_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);
    settings_panel_->Show(settings_);
  });
  system_.SetConnectionTapCallback([this]() {
    if (!lv_obj_has_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    settings_panel_->Hide();
    lv_obj_remove_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(about_panel_);
  });
}

LinkScreen::~LinkScreen() {
  // Delete our own objects; the SystemScreen member removes its own tree.
  if (about_panel_ != nullptr) lv_obj_delete(about_panel_);
  if (prompt_panel_ != nullptr) lv_obj_delete(prompt_panel_);
  settings_panel_.reset();
}

void LinkScreen::Update(const firmware::domain::CpuView& status,
                        std::span<const firmware::domain::MetricCardView> cards,
                        const firmware::domain::LinkView& link,
                        const firmware::domain::AgentsBarView& agents,
                        const std::string& coverage) {
  system_.Update(status, cards, agents, coverage, link.fault);
  lv_label_set_text(about_label_, link.about.c_str());
  lv_label_set_text(fault_label_, link.fault.c_str());
  if (link.prompt_visible) {
    settings_panel_->Hide();
    lv_obj_add_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(prompt_label_, link.prompt.c_str());
    lv_obj_remove_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace firmware::ui::system
