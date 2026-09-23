// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/system_screen.hpp"

namespace firmware::ui::system {
namespace {
constexpr int32_t kCardWidth = 152;
constexpr int32_t kCardHeight = 84;
}  // namespace

SystemScreen::SystemScreen(lv_obj_t* parent) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 320, 128);
  // Fixed layout: nothing here scrolls, so a drag on the bar never moves content.
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root_, 4, 0);
  lv_obj_set_style_pad_row(root_, 4, 0);

  bar_row_ = lv_obj_create(root_);
  lv_obj_t* bar_row = bar_row_;
  lv_obj_set_size(bar_row, LV_PCT(100), 28);
  lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(bar_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(bar_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      bar_row,
      [](lv_event_t* e) {
        auto* self = static_cast<SystemScreen*>(lv_event_get_user_data(e));
        if (self->on_status_tap_) self->on_status_tap_();
      },
      LV_EVENT_CLICKED, this);
  connection_label_ = lv_label_create(bar_row);
  coverage_label_ = lv_label_create(bar_row);
  clock_label_ = lv_label_create(bar_row);

  card_area_ = lv_obj_create(root_);
  lv_obj_set_size(card_area_, LV_PCT(100), kCardHeight);
  lv_obj_remove_flag(card_area_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card_area_, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_all(card_area_, 0, 0);
  lv_obj_set_style_pad_column(card_area_, 4, 0);
  lv_obj_set_style_border_width(card_area_, 0, 0);
}

SystemScreen::~SystemScreen() {
  cards_.clear();
  if (root_ != nullptr) lv_obj_delete(root_);
}

void SystemScreen::Update(const firmware::domain::CpuView& status,
                          std::span<const firmware::domain::MetricCardView> cards,
                          const std::string& coverage) {
  lv_label_set_text(connection_label_, status.connection.c_str());
  lv_label_set_text(coverage_label_, coverage.c_str());
  lv_label_set_text(clock_label_, status.clock.c_str());
  while (cards_.size() < cards.size()) {
    cards_.push_back(std::make_unique<MetricCard>(card_area_, kCardWidth, kCardHeight));
  }
  for (size_t i = 0; i < cards.size(); ++i) cards_[i]->Update(cards[i]);
}

}  // namespace firmware::ui::system
