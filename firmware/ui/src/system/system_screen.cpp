// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/system_screen.hpp"

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {
namespace {
// 4 px margins and gaps on the 320x240 display: a 20 px status bar, 174 px of
// cards (a 96 px dial row and a 74 px panel row), and a 30 px Agents bar.
constexpr int32_t kGap = 4;
constexpr int32_t kContentWidth = 320 - 2 * kGap;
constexpr int32_t kStatusHeight = 20;
constexpr int32_t kAgentsHeight = 30;
constexpr int32_t kCardAreaHeight = 240 - 2 * kGap - kStatusHeight - kAgentsHeight - 2 * kGap;
constexpr int32_t kDialWidth = (kContentWidth - 2 * kGap) / 3;
constexpr int32_t kDialHeight = 96;
constexpr int32_t kPanelWidth = (kContentWidth - kGap) / 2;
constexpr int32_t kPanelHeight = kCardAreaHeight - kDialHeight - kGap;
}  // namespace

SystemScreen::SystemScreen(lv_obj_t* parent) {
  root_ = lv_obj_create(parent);
  lv_obj_set_size(root_, 320, 240);
  // Fixed layout: nothing here scrolls, so a drag on the bar never moves content.
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(root_, kGap, 0);
  lv_obj_set_style_pad_row(root_, kGap, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_radius(root_, 0, 0);
  lv_obj_set_style_bg_color(root_, theme::Color(theme::kScreen), 0);
  lv_obj_set_style_text_color(root_, theme::Color(theme::kText), 0);

  bar_row_ = lv_obj_create(root_);
  lv_obj_t* bar_row = bar_row_;
  lv_obj_remove_style_all(bar_row);
  lv_obj_set_size(bar_row, LV_PCT(100), kStatusHeight);
  // No side padding: the screen's own 4 px margin is enough, and the worst
  // case ("Disconnected", "WiFi gap BLE scan", "Time unavailable") needs the
  // full 312 px.
  lv_obj_set_style_pad_column(bar_row, 8, 0);
  lv_obj_set_style_text_font(bar_row, &lv_font_montserrat_12, 0);
  lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  // A hairline under the bar instead of a panel (the Quiet style).
  theme::StyleRowRule(bar_row);
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
  // Its own touch target: the bar is only 20 px tall, so reach past it.
  lv_obj_add_flag(connection_label_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(connection_label_, 8);
  lv_obj_add_event_cb(
      connection_label_,
      [](lv_event_t* e) {
        auto* self = static_cast<SystemScreen*>(lv_event_get_user_data(e));
        if (self->on_connection_tap_) self->on_connection_tap_();
      },
      LV_EVENT_CLICKED, this);
  coverage_label_ = lv_label_create(bar_row);
  lv_obj_set_style_text_color(coverage_label_, theme::Color(theme::kMuted), 0);
  clock_label_ = lv_label_create(bar_row);

  card_area_ = lv_obj_create(root_);
  lv_obj_remove_style_all(card_area_);
  lv_obj_set_size(card_area_, LV_PCT(100), kCardAreaHeight);
  lv_obj_remove_flag(card_area_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card_area_, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(card_area_, kGap, 0);
  lv_obj_set_style_pad_row(card_area_, kGap, 0);

  agents_bar_ = std::make_unique<AgentsBar>(root_, LV_PCT(100), kAgentsHeight);
}

SystemScreen::~SystemScreen() {
  cards_.clear();
  agents_bar_.reset();
  if (root_ != nullptr) lv_obj_delete(root_);
}

void SystemScreen::Update(const firmware::domain::CpuView& status,
                          std::span<const firmware::domain::MetricCardView> cards,
                          const firmware::domain::AgentsBarView& agents,
                          const std::string& coverage, const std::string& notice) {
  // Live Host data needs no word: a cyan USB glyph. Every other state is spelled out.
  lv_label_set_text(connection_label_,
                    status.connected ? LV_SYMBOL_USB : status.connection.c_str());
  lv_obj_set_style_text_color(connection_label_,
                              theme::Color(status.connected ? theme::kCyan : theme::kText), 0);
  lv_label_set_text(coverage_label_, coverage.c_str());
  lv_label_set_text(clock_label_, status.clock.c_str());
  while (cards_.size() < cards.size()) {
    const size_t index = cards_.size();
    const auto layout = cards[index].layout;
    const bool dial = layout == firmware::domain::CardLayout::kDial;
    cards_.push_back(std::make_unique<MetricCard>(card_area_, dial ? kDialWidth : kPanelWidth,
                                                  dial ? kDialHeight : kPanelHeight, layout,
                                                  theme::CardAccent(index)));
  }
  for (size_t i = 0; i < cards.size(); ++i) cards_[i]->Update(cards[i]);
  agents_bar_->Update(agents, notice);
}

}  // namespace firmware::ui::system
