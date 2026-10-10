// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/settings/settings_panel.hpp"

#include <cstdio>

#include "firmware/ui/theme.hpp"

namespace firmware::ui::system {

namespace domain = firmware::domain;

std::string OffsetText(int minutes_east) {
  if (minutes_east == 0) return "UTC";
  const int magnitude = minutes_east < 0 ? -minutes_east : minutes_east;
  char text[16];
  if (magnitude % 60 == 0) {
    std::snprintf(text, sizeof text, "UTC%c%d", minutes_east < 0 ? '-' : '+', magnitude / 60);
  } else {
    std::snprintf(text, sizeof text, "UTC%c%d:%02d", minutes_east < 0 ? '-' : '+', magnitude / 60,
                  magnitude % 60);
  }
  return text;
}

// Event thunks live in a friend struct so the panel's members stay private.
struct SettingsPanelEvents {
  static SettingsPanel* Panel(lv_event_t* e) {
    return static_cast<SettingsPanel*>(lv_event_get_user_data(e));
  }
  static void Use24h(lv_event_t* e) {
    auto* p = Panel(e);
    auto next = p->settings_;
    next.use_24h = lv_obj_has_state(p->use_24h_switch_, LV_STATE_CHECKED);
    p->Changed(next);
  }
  static void ManualZone(lv_event_t* e) {
    auto* p = Panel(e);
    auto next = p->settings_;
    next.manual_zone = lv_obj_has_state(p->manual_zone_switch_, LV_STATE_CHECKED);
    p->Changed(next);
  }
  static void OffsetDown(lv_event_t* e) {
    auto* p = Panel(e);
    if (p->settings_.manual_zone) p->Changed(domain::StepOffset(p->settings_, -1));
  }
  static void OffsetUp(lv_event_t* e) {
    auto* p = Panel(e);
    if (p->settings_.manual_zone) p->Changed(domain::StepOffset(p->settings_, +1));
  }
  static void Done(lv_event_t* e) { Panel(e)->Hide(); }
  static void Recalibrate(lv_event_t* e) {
    auto* p = Panel(e);
    if (p->actions_.on_recalibrate) p->actions_.on_recalibrate();
  }
  static void Debug(lv_event_t* e) {
    auto* p = Panel(e);
    if (p->actions_.on_debug) p->actions_.on_debug();
  }
};

lv_obj_t* SettingsPanel::AddRow(const char* text, lv_obj_t** control, bool is_switch) {
  lv_obj_t* row = lv_obj_create(panel_);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 34);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  theme::StyleRowRule(row);
  lv_obj_t* label = lv_label_create(row);
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  if (is_switch) {
    *control = lv_switch_create(row);
    lv_obj_set_size(*control, 44, 22);
    lv_obj_align(*control, LV_ALIGN_RIGHT_MID, 0, 0);
    theme::StyleSwitch(*control);
  }
  return row;
}

namespace {

// A text-only button sized to its label.
lv_obj_t* TextButton(lv_obj_t* parent, const char* text, uint32_t color) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, LV_SIZE_CONTENT, 28);
  theme::StyleTextButton(button, color);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

}  // namespace

SettingsPanel::SettingsPanel(lv_obj_t* parent, ChangeCallback on_change, Actions actions)
    : on_change_(std::move(on_change)), actions_(std::move(actions)) {
  // A sheet rising from the bottom edge over the Overview, its actions as
  // text in the header (the Quiet style).
  panel_ = lv_obj_create(parent);
  theme::StyleSheet(panel_, 186);
  lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel_, 2, 0);
  lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* header = lv_obj_create(panel_);
  lv_obj_remove_style_all(header);
  lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(header, LV_PCT(100), 30);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* title = lv_label_create(header);
  theme::StyleHeading(title);
  lv_label_set_text(title, "Clock");
  lv_obj_set_flex_grow(title, 1);
  recalibrate_ = TextButton(header, "Recalibrate", theme::kMuted);
  debug_ = TextButton(header, "Debug", theme::kMuted);
  done_ = TextButton(header, "Done", theme::kCyan);

  AddRow("24-hour clock", &use_24h_switch_, true);
  AddRow("Set zone manually", &manual_zone_switch_, true);

  lv_obj_t* offset_row = AddRow("", nullptr, false);
  offset_down_ = TextButton(offset_row, "-15m", theme::kCyan);
  lv_obj_align(offset_down_, LV_ALIGN_LEFT_MID, 0, 0);
  offset_label_ = lv_label_create(offset_row);
  lv_obj_align(offset_label_, LV_ALIGN_CENTER, 0, 0);
  offset_up_ = TextButton(offset_row, "+15m", theme::kCyan);
  lv_obj_align(offset_up_, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_add_event_cb(use_24h_switch_, SettingsPanelEvents::Use24h, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(manual_zone_switch_, SettingsPanelEvents::ManualZone, LV_EVENT_VALUE_CHANGED,
                      this);
  lv_obj_add_event_cb(offset_down_, SettingsPanelEvents::OffsetDown, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(offset_up_, SettingsPanelEvents::OffsetUp, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(done_, SettingsPanelEvents::Done, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(recalibrate_, SettingsPanelEvents::Recalibrate, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(debug_, SettingsPanelEvents::Debug, LV_EVENT_CLICKED, this);
  Refresh();
}

SettingsPanel::~SettingsPanel() {
  if (panel_ != nullptr) lv_obj_delete(panel_);
}

void SettingsPanel::Show(const domain::DisplaySettings& current) {
  settings_ = domain::Sanitize(current);
  Refresh();
  lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel_);
}

void SettingsPanel::Hide() { lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN); }

bool SettingsPanel::visible() const { return !lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN); }

void SettingsPanel::Changed(const domain::DisplaySettings& next) {
  settings_ = domain::Sanitize(next);
  Refresh();
  if (on_change_) on_change_(settings_);
}

void SettingsPanel::Refresh() {
  const auto set = [](lv_obj_t* sw, bool on) {
    if (on) {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
  };
  set(use_24h_switch_, settings_.use_24h);
  set(manual_zone_switch_, settings_.manual_zone);
  // The offset only applies to a manual zone; otherwise it reads "follows the Mac".
  lv_label_set_text(offset_label_, settings_.manual_zone
                                       ? OffsetText(settings_.manual_offset_minutes).c_str()
                                       : "Follows the Mac");
  for (lv_obj_t* button : {offset_down_, offset_up_}) {
    if (settings_.manual_zone) {
      lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(button, LV_STATE_DISABLED);
    }
  }
}

}  // namespace firmware::ui::system
