// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/link/link_screen.hpp"

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {

bool SameDetails(const std::vector<firmware::domain::LinkDetail>& a,
                 const std::vector<firmware::domain::LinkDetail>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].label != b[i].label || a[i].value != b[i].value) return false;
  }
  return true;
}

}  // namespace

void LinkScreen::MakeButton(const char* text, lv_align_t align, Target* target) {
  lv_obj_t* button = lv_button_create(prompt_panel_);
  lv_obj_set_size(button, 110, 36);
  lv_obj_align(button, align, 0, 0);
  // Text-only: Confirm in cyan, Decline muted (the Quiet style).
  theme::StyleActionButton(button, target->accepted);
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
  // Host link details: a sheet over the Overview, so versions never cover the
  // cards for long. A title and Done on top, then one row per detail. While
  // it is open a transparent scrim under it closes it on a tap anywhere outside.
  about_panel_ = lv_obj_create(parent);
  theme::StyleSheet(about_panel_, 166);
  lv_obj_set_flex_flow(about_panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(about_panel_, 0, 0);
  lv_obj_t* header = lv_obj_create(about_panel_);
  lv_obj_remove_style_all(header);
  lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(header, LV_PCT(100), 30);
  lv_obj_t* about_title = lv_label_create(header);
  theme::StyleHeading(about_title);
  lv_label_set_text(about_title, "Host link");
  lv_obj_align(about_title, LV_ALIGN_LEFT_MID, 0, 0);
  about_done_ = lv_button_create(header);
  lv_obj_set_size(about_done_, LV_SIZE_CONTENT, 26);
  theme::StyleActionButton(about_done_, true);
  lv_obj_t* done_label = lv_label_create(about_done_);
  lv_label_set_text(done_label, "Done");
  lv_obj_center(done_label);
  lv_obj_align(about_done_, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(
      about_done_,
      [](lv_event_t* event) {
        static_cast<LinkScreen*>(lv_event_get_user_data(event))->SetAboutVisible(false);
      },
      LV_EVENT_CLICKED, this);
  details_ = lv_obj_create(about_panel_);
  lv_obj_remove_style_all(details_);
  lv_obj_remove_flag(details_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(details_, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(details_, LV_FLEX_FLOW_COLUMN);
  fault_label_ = lv_label_create(about_panel_);
  lv_obj_set_width(fault_label_, LV_PCT(100));
  lv_obj_set_style_pad_top(fault_label_, 4, 0);
  lv_obj_add_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);

  // The pairing prompt: a centred card with the code large enough to compare
  // across the room.
  prompt_panel_ = lv_obj_create(parent);
  lv_obj_set_size(prompt_panel_, 296, 150);
  theme::StylePanel(prompt_panel_);
  lv_obj_set_style_radius(prompt_panel_, 10, 0);
  lv_obj_center(prompt_panel_);
  lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* prompt_title = lv_label_create(prompt_panel_);
  theme::StyleHeading(prompt_title);
  lv_label_set_text(prompt_title, "Pair with this Mac?");
  lv_obj_align(prompt_title, LV_ALIGN_TOP_MID, 0, 0);
  code_label_ = lv_label_create(prompt_panel_);
  lv_obj_set_style_text_font(code_label_, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(code_label_, theme::Color(theme::kCyan), 0);
  lv_obj_set_style_text_letter_space(code_label_, 8, 0);
  lv_obj_align(code_label_, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_t* hint = lv_label_create(prompt_panel_);
  theme::StyleCaption(hint);
  lv_label_set_text(hint, "The Mac shows the same code");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 58);
  confirm_target_ = {this, true};
  decline_target_ = {this, false};
  MakeButton("Decline", LV_ALIGN_BOTTOM_LEFT, &decline_target_);
  MakeButton("Confirm", LV_ALIGN_BOTTOM_RIGHT, &confirm_target_);

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
    SetAboutVisible(false);
    settings_panel_->Show(settings_, SettingsPanel::Section::kClock);
  });
  system_.SetRadioTapCallback([this]() {
    if (!lv_obj_has_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    SetAboutVisible(false);
    settings_panel_->Show(settings_, SettingsPanel::Section::kRadios);
  });
  system_.SetConnectionTapCallback([this]() {
    if (!lv_obj_has_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    settings_panel_->Hide();
    SetAboutVisible(true);
    ShowDetails();
  });
}

void LinkScreen::SetAboutVisible(bool visible) {
  if (visible) {
    if (about_scrim_ == nullptr) {
      about_scrim_ = theme::CreateScrim(lv_obj_get_parent(about_panel_));
      lv_obj_add_event_cb(
          about_scrim_,
          [](lv_event_t* event) {
            static_cast<LinkScreen*>(lv_event_get_user_data(event))->SetAboutVisible(false);
          },
          LV_EVENT_CLICKED, this);
    }
    lv_obj_move_foreground(about_scrim_);
    lv_obj_remove_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(about_panel_);
  } else {
    if (about_scrim_ != nullptr) {
      lv_obj_delete(about_scrim_);
      about_scrim_ = nullptr;
    }
    lv_obj_add_flag(about_panel_, LV_OBJ_FLAG_HIDDEN);
  }
}

// The rows exist only while the sheet is open: about 3.5 KB of the LVGL pool
// otherwise held for a panel that is usually closed.
void LinkScreen::ShowDetails() {
  if (!about_visible()) {
    if (!shown_details_.empty()) {
      lv_obj_clean(details_);
      shown_details_.clear();
    }
    return;
  }
  if (SameDetails(latest_details_, shown_details_)) return;
  shown_details_ = latest_details_;
  lv_obj_clean(details_);
  for (const auto& detail : shown_details_) {
    lv_obj_t* row = lv_obj_create(details_);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), 24);
    theme::StyleRowRule(row);
    lv_obj_t* label = lv_label_create(row);
    theme::StyleCaption(label);
    lv_label_set_text(label, detail.label.c_str());
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, detail.value.c_str());
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

LinkScreen::~LinkScreen() {
  // Delete our own objects; the SystemScreen member removes its own tree.
  if (about_panel_ != nullptr) lv_obj_delete(about_panel_);
  if (about_scrim_ != nullptr) lv_obj_delete(about_scrim_);
  if (prompt_panel_ != nullptr) lv_obj_delete(prompt_panel_);
  settings_panel_.reset();
}

void LinkScreen::Update(const firmware::domain::CpuView& status,
                        std::span<const firmware::domain::MetricCardView> cards,
                        const firmware::domain::LinkView& link,
                        const firmware::domain::AgentsBarView& agents,
                        const firmware::domain::CoverageStatus& coverage) {
  system_.Update(status, cards, agents, coverage, link.fault);
  latest_details_ = link.details;
  ShowDetails();
  settings_panel_->SyncRadios();
  lv_label_set_text(fault_label_, link.fault.c_str());
  if (link.prompt_visible) {
    settings_panel_->Hide();
    SetAboutVisible(false);
    lv_label_set_text(code_label_, link.code.c_str());
    lv_obj_remove_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(prompt_panel_, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace firmware::ui::system
