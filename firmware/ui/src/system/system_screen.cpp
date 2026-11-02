// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/system_screen.hpp"

#include <string>

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
// The radio tray: every box has a fixed width so no text change moves anything.
// The channel box fits "ch11" and "ch14" in montserrat 12; a flag is one
// character ("~", "?", "!").
constexpr int32_t kSlotGap = 2;  // within a radio's slot: icon, flag, channel
constexpr int32_t kTrayGap = 8;  // between the two radios, so each reads as a unit
constexpr int32_t kChannelWidth = 27;
constexpr int32_t kFlagWidth = 7;  // "~" and "?" are 7 px wide in montserrat 12
constexpr int32_t kSeparatorHeight = 12;
constexpr int32_t kSeparatorMargin = 6;
constexpr int32_t kSlashHaloWidth = 5;
constexpr int32_t kSlashWidth = 2;
constexpr int32_t kRuneWidth = 2;
constexpr int32_t kRuneBoxWidth = 10;  // the rune's 8 px plus its stroke
constexpr int32_t kGearWidth = 26;  // the Settings touch target: 26x20, 24 px tall with its margin
constexpr int32_t kSlashSpan = 11;  // vertical extent: just the glyph, not the font line

// A non-interactive plain object: the bar underneath owns every tap.
lv_obj_t* PlainObject(lv_obj_t* parent) {
  lv_obj_t* object = lv_obj_create(parent);
  lv_obj_remove_style_all(object);
  lv_obj_remove_flag(object,
                     static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return object;
}

// A fixed-width single-line label in the tray.
lv_obj_t* TrayLabel(lv_obj_t* parent, int32_t width, lv_text_align_t align) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(label, align, 0);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

// A working radio is one steady colour whether or not its window is open:
// windows alternate about five times a second, and following them read as
// pulsing on the board.
uint32_t SlotColor(firmware::domain::RadioSlotState state, uint32_t accent) {
  using firmware::domain::RadioSlotState;
  switch (state) {
    case RadioSlotState::kScanning:
    case RadioSlotState::kIdle:
      return accent;
    case RadioSlotState::kOff:
      return theme::kDim;
    case RadioSlotState::kFault:
      return theme::kText;
    case RadioSlotState::kGap:
    case RadioSlotState::kNoData:
      return theme::kMuted;
  }
  return theme::kMuted;
}

// A mark beside the icon: colour never travels alone.
const char* SlotFlag(firmware::domain::RadioSlotState state) {
  using firmware::domain::RadioSlotState;
  switch (state) {
    case RadioSlotState::kGap:
      return "~";
    case RadioSlotState::kNoData:
      return "?";
    case RadioSlotState::kFault:
      return "!";
    case RadioSlotState::kScanning:
    case RadioSlotState::kIdle:
    case RadioSlotState::kOff:
      return "";
  }
  return "";
}
void SetHidden(lv_obj_t* object, bool hidden) {
  if (hidden) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  }
}
}  // namespace

void SystemScreen::BuildRadioIcon(RadioWidgets& widgets, lv_obj_t* group, const char* symbol) {
  lv_point_t size{kRuneBoxWidth, 13};
  if (symbol != nullptr) {
    lv_text_get_size(&size, symbol, &lv_font_montserrat_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  }
  lv_obj_t* slot = PlainObject(group);
  lv_obj_set_size(slot, size.x, kStatusHeight);
  if (symbol != nullptr) {
    widgets.icon = lv_label_create(slot);
    lv_label_set_text_static(widgets.icon, symbol);
    lv_obj_center(widgets.icon);
  } else {
    // The rune, a thin stroke polyline 13 px tall (the prototype's path):
    // up the stem, out to the top arrow, across to the bottom-left, and so on.
    const int32_t x = 1, y = (kStatusHeight - 13) / 2;
    widgets.rune_points = {lv_point_precise_t{x, y + 4},      lv_point_precise_t{x + 8, y + 9},
                           lv_point_precise_t{x + 4, y + 13}, lv_point_precise_t{x + 4, y},
                           lv_point_precise_t{x + 8, y + 4},  lv_point_precise_t{x, y + 9}};
    widgets.icon = lv_line_create(slot);
    lv_line_set_points(widgets.icon, widgets.rune_points.data(), widgets.rune_points.size());
    lv_obj_set_style_line_width(widgets.icon, kRuneWidth, 0);
    lv_obj_set_style_line_rounded(widgets.icon, true, 0);
  }
  lv_obj_remove_flag(widgets.icon, LV_OBJ_FLAG_CLICKABLE);
  // The slash runs corner to corner of the icon, over a screen-coloured halo
  // that keeps the strokes beneath it from muddying it.
  const int32_t top = (kStatusHeight - kSlashSpan) / 2;
  widgets.slash_points = {lv_point_precise_t{1, top},
                          lv_point_precise_t{size.x - 1, top + kSlashSpan}};
  const auto make_line = [&](uint32_t color, int32_t width) {
    lv_obj_t* line = lv_line_create(slot);
    lv_line_set_points(line, widgets.slash_points.data(), widgets.slash_points.size());
    lv_obj_set_style_line_color(line, theme::Color(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return line;
  };
  widgets.slash_halo = make_line(theme::kScreen, kSlashHaloWidth);
  widgets.slash = make_line(theme::kDim, kSlashWidth);
}

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
  // No side padding: the screen's own 4 px margin is enough.
  lv_obj_set_style_pad_column(bar_row, 0, 0);
  lv_obj_set_style_text_font(bar_row, &lv_font_montserrat_12, 0);
  lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  // A hairline under the bar instead of a panel (the Quiet style).
  theme::StyleRowRule(bar_row);
  lv_obj_remove_flag(bar_row, LV_OBJ_FLAG_SCROLLABLE);
  // The bar itself is not a button: only the connection text, the clock and
  // the gear are, so a stray tap on the tray or the gaps opens nothing.
  lv_obj_remove_flag(bar_row, LV_OBJ_FLAG_CLICKABLE);
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
  // The centre takes whatever the ends leave; it is reserved for the future
  // Alert, so it shows nothing for now.
  lv_obj_t* centre = PlainObject(bar_row);
  lv_obj_set_size(centre, 0, kStatusHeight);
  lv_obj_set_flex_grow(centre, 1);
  // The radio tray beside the clock: WiFi (icon, flag, channel), BLE (icon,
  // flag), then a hairline. A flag sits right after its icon, so it belongs to
  // that radio. BLE shows no channel: its controller cycles the
  // three advertising channels itself.
  tray_ = PlainObject(bar_row);
  lv_obj_set_size(tray_, LV_SIZE_CONTENT, kStatusHeight);
  lv_obj_set_flex_flow(tray_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tray_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tray_, kTrayGap, 0);
  const auto make_group = [&] {
    lv_obj_t* group = PlainObject(tray_);
    lv_obj_set_size(group, LV_SIZE_CONTENT, kStatusHeight);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(group, kSlotGap, 0);
    return group;
  };
  lv_obj_t* wifi_group = make_group();
  BuildRadioIcon(wifi_, wifi_group, LV_SYMBOL_WIFI);
  wifi_.flag = TrayLabel(wifi_group, kFlagWidth, LV_TEXT_ALIGN_CENTER);
  wifi_channel_ = TrayLabel(wifi_group, kChannelWidth, LV_TEXT_ALIGN_LEFT);
  lv_obj_t* ble_group = make_group();
  BuildRadioIcon(ble_, ble_group, nullptr);
  ble_.flag = TrayLabel(ble_group, kFlagWidth, LV_TEXT_ALIGN_CENTER);
  separator_ = PlainObject(tray_);
  lv_obj_set_size(separator_, 1, kSeparatorHeight);
  lv_obj_set_style_bg_opa(separator_, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(separator_, theme::Color(theme::kPanelBorder), 0);
  lv_obj_set_style_margin_right(separator_, kSeparatorMargin, 0);
  clock_label_ = lv_label_create(bar_row);
  // The clock opens Settings too, as a second path; the gear is the obvious one.
  lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      clock_label_,
      [](lv_event_t* e) {
        auto* self = static_cast<SystemScreen*>(lv_event_get_user_data(e));
        if (self->on_status_tap_) self->on_status_tap_();
      },
      LV_EVENT_CLICKED, this);
  // The gear: one fixed-width label at the bar's right end, so no text
  // change moves it.
  gear_ = lv_label_create(bar_row);
  lv_label_set_text_static(gear_, LV_SYMBOL_SETTINGS);
  lv_obj_set_size(gear_, kGearWidth, kStatusHeight);
  lv_obj_set_style_text_align(gear_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(gear_, theme::Color(theme::kText), 0);
  lv_obj_set_style_pad_top(gear_, 3, 0);
  lv_obj_add_flag(gear_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gear_, 2);
  theme::StylePressed(gear_);
  lv_obj_add_event_cb(
      gear_,
      [](lv_event_t* e) {
        auto* self = static_cast<SystemScreen*>(lv_event_get_user_data(e));
        if (self->on_status_tap_) self->on_status_tap_();
      },
      LV_EVENT_CLICKED, this);

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

void SystemScreen::ApplySlot(const RadioWidgets& widgets,
                             const firmware::domain::RadioSlotView& slot, uint32_t accent) {
  const lv_color_t color = theme::Color(SlotColor(slot.state, accent));
  if (lv_obj_check_type(widgets.icon, &lv_line_class)) {
    lv_obj_set_style_line_color(widgets.icon, color, 0);
  } else {
    lv_obj_set_style_text_color(widgets.icon, color, 0);
  }
  lv_obj_set_style_text_color(widgets.flag, color, 0);
  lv_label_set_text_static(widgets.flag, SlotFlag(slot.state));
  const bool off = slot.state == firmware::domain::RadioSlotState::kOff;
  for (lv_obj_t* line : {widgets.slash_halo, widgets.slash}) {
    SetHidden(line, !off);
  }
  // An off radio has no flag to show: its empty box collapses so the slashed
  // icon sits beside its neighbour instead of before a blank gap.
  SetHidden(widgets.flag, off);
}

void SystemScreen::Update(const firmware::domain::CpuView& status,
                          std::span<const firmware::domain::MetricCardView> cards,
                          const firmware::domain::AgentsBarView& agents,
                          const firmware::domain::CoverageStatus& coverage,
                          const std::string& notice) {
  // Live Host data needs no word: a cyan USB glyph. Every other state is spelled out.
  lv_label_set_text(connection_label_,
                    status.connected ? LV_SYMBOL_USB : status.connection.c_str());
  lv_obj_set_style_text_color(connection_label_,
                              theme::Color(status.connected ? theme::kCyan : theme::kText), 0);
  if (!shown_coverage_ || shown_coverage_->wifi.state != coverage.wifi.state ||
      shown_coverage_->wifi.channel != coverage.wifi.channel) {
    ApplySlot(wifi_, coverage.wifi, theme::kCyan);
    const uint8_t channel = coverage.wifi.channel;
    lv_label_set_text(wifi_channel_, channel == 0 ? "" : ("ch" + std::to_string(channel)).c_str());
    lv_obj_set_style_text_color(wifi_channel_,
                                theme::Color(SlotColor(coverage.wifi.state, theme::kCyan)), 0);
    // WiFi off has no channel: collapse the box with the flag (see ApplySlot).
    SetHidden(wifi_channel_, coverage.wifi.state == firmware::domain::RadioSlotState::kOff);
  }
  if (!shown_coverage_ || shown_coverage_->ble.state != coverage.ble.state) {
    ApplySlot(ble_, coverage.ble, theme::kBlue);
  }
  shown_coverage_ = coverage;
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
