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
  lv_obj_t* row = lv_obj_create(body_);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 32);
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

// An outlined pill sized to its label.
lv_obj_t* ActionButton(lv_obj_t* parent, const char* text, uint32_t color) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, LV_SIZE_CONTENT, 26);
  theme::StyleActionButton(button, color == theme::kCyan);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

// A section selector pill: cyan when it is the shown section, muted otherwise.
void StyleTab(lv_obj_t* tab, bool selected) {
  lv_obj_remove_style_all(tab);
  theme::StyleActionButton(tab, selected);
}

}  // namespace

SettingsPanel::SettingsPanel(lv_obj_t* parent, ChangeCallback on_change, Actions actions)
    : on_change_(std::move(on_change)), actions_(std::move(actions)) {
  // A sheet rising from the bottom edge over the Overview. The header holds the
  // Clock | Radios selector and Done; the shown section's rows sit under it and
  // Recalibrate and Debug along the bottom. While it is open a transparent scrim
  // under it takes any tap outside the sheet (the status bar included) and
  // closes it.
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
  lv_obj_set_style_pad_column(header, 6, 0);
  clock_tab_ = ActionButton(header, "Clock", theme::kCyan);
  radios_tab_ = ActionButton(header, "Radios", theme::kMuted);
  lv_obj_t* spacer = lv_obj_create(header);
  lv_obj_remove_style_all(spacer);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(spacer, 0, 1);
  lv_obj_set_flex_grow(spacer, 1);
  done_ = ActionButton(header, "Done", theme::kCyan);

  body_ = lv_obj_create(panel_);
  lv_obj_remove_style_all(body_);
  lv_obj_remove_flag(body_,
                     static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_width(body_, LV_PCT(100));
  lv_obj_set_flex_grow(body_, 1);
  lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* footer = lv_obj_create(panel_);
  lv_obj_remove_style_all(footer);
  lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(footer, LV_PCT(100), 28);
  lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(footer, 6, 0);
  recalibrate_ = ActionButton(footer, "Recalibrate", theme::kMuted);
  debug_ = ActionButton(footer, "Debug", theme::kMuted);

  lv_obj_add_event_cb(
      clock_tab_,
      [](lv_event_t* e) { SettingsPanelEvents::Panel(e)->SelectSection(Section::kClock); },
      LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(
      radios_tab_,
      [](lv_event_t* e) { SettingsPanelEvents::Panel(e)->SelectSection(Section::kRadios); },
      LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(done_, SettingsPanelEvents::Done, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(recalibrate_, SettingsPanelEvents::Recalibrate, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(debug_, SettingsPanelEvents::Debug, LV_EVENT_CLICKED, this);
  // No section is built while hidden; Show builds the one asked for.
}

void SettingsPanel::BuildClockRows() {
  AddRow("24-hour clock", &use_24h_switch_, true);
  AddRow("Set zone manually", &manual_zone_switch_, true);

  lv_obj_t* offset_row = AddRow("", nullptr, false);
  offset_down_ = ActionButton(offset_row, "-15m", theme::kCyan);
  lv_obj_align(offset_down_, LV_ALIGN_LEFT_MID, 0, 0);
  offset_label_ = lv_label_create(offset_row);
  lv_obj_align(offset_label_, LV_ALIGN_CENTER, 0, 0);
  offset_up_ = ActionButton(offset_row, "+15m", theme::kCyan);
  lv_obj_align(offset_up_, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_add_event_cb(use_24h_switch_, SettingsPanelEvents::Use24h, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(manual_zone_switch_, SettingsPanelEvents::ManualZone, LV_EVENT_VALUE_CHANGED,
                      this);
  lv_obj_add_event_cb(offset_down_, SettingsPanelEvents::OffsetDown, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(offset_up_, SettingsPanelEvents::OffsetUp, LV_EVENT_CLICKED, this);
  Refresh();
}

void SettingsPanel::BuildRadioRows() {
  AddRow("WiFi observation", &wifi_switch_, true);
  AddRow("BLE observation", &ble_switch_, true);
  lv_obj_add_event_cb(
      wifi_switch_, [](lv_event_t* e) { SettingsPanelEvents::Panel(e)->RadioToggled(true); },
      LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(
      ble_switch_, [](lv_event_t* e) { SettingsPanelEvents::Panel(e)->RadioToggled(false); },
      LV_EVENT_VALUE_CHANGED, this);
  radios_caption_ = lv_label_create(body_);
  theme::StyleCaption(radios_caption_);
  lv_obj_set_width(radios_caption_, LV_PCT(100));
  lv_obj_set_style_pad_top(radios_caption_, 4, 0);
  lv_label_set_text(radios_caption_,
                    "An off radio reads Operator disabled, not zero. Not saved across restarts.");
  read_wifi_ = read_ble_ = true;
  if (actions_.read_radios) actions_.read_radios(read_wifi_, read_ble_);
  const auto set = [](lv_obj_t* sw, bool on) {
    if (on) {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
  };
  set(wifi_switch_, read_wifi_);
  set(ble_switch_, read_ble_);
}

void SettingsPanel::SelectSection(Section section) {
  // Only the shown section has rows: the LVGL pool cannot hold both.
  lv_obj_clean(body_);
  use_24h_switch_ = manual_zone_switch_ = offset_label_ = offset_down_ = offset_up_ = nullptr;
  wifi_switch_ = ble_switch_ = radios_caption_ = nullptr;
  section_ = section;
  StyleTab(clock_tab_, section == Section::kClock);
  StyleTab(radios_tab_, section == Section::kRadios);
  if (section == Section::kClock) {
    BuildClockRows();
  } else {
    BuildRadioRows();
  }
}

void SettingsPanel::RadioToggled(bool wifi) {
  lv_obj_t* sw = wifi ? wifi_switch_ : ble_switch_;
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  // The same ControlRequest path as DEV:RADIO_WIFI_ON/OFF and DEV:RADIO_BLE_ON/OFF.
  if (actions_.set_radio && !actions_.set_radio(wifi, on)) {
    // Not queued (radio stopped or queue full): put the switch back.
    if (on) {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return;
  }
}

void SettingsPanel::SyncRadios() {
  if (section_ != Section::kRadios || !visible() || wifi_switch_ == nullptr ||
      !actions_.read_radios) {
    return;
  }
  bool wifi = read_wifi_, ble = read_ble_;
  actions_.read_radios(wifi, ble);
  const auto apply = [](lv_obj_t* sw, bool& last, bool now) {
    if (now == last) return;
    last = now;
    if (now) {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
  };
  apply(wifi_switch_, read_wifi_, wifi);
  apply(ble_switch_, read_ble_, ble);
}

SettingsPanel::~SettingsPanel() {
  if (panel_ != nullptr) lv_obj_delete(panel_);
  if (scrim_ != nullptr) lv_obj_delete(scrim_);
}

void SettingsPanel::Show(const domain::DisplaySettings& current, Section section) {
  settings_ = domain::Sanitize(current);
  SelectSection(section);
  if (scrim_ == nullptr) {
    scrim_ = theme::CreateScrim(lv_obj_get_parent(panel_));
    lv_obj_add_event_cb(scrim_, SettingsPanelEvents::Done, LV_EVENT_CLICKED, this);
  }
  lv_obj_move_foreground(scrim_);
  lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel_);
}

void SettingsPanel::Hide() {
  if (scrim_ != nullptr) {
    lv_obj_delete(scrim_);
    scrim_ = nullptr;
  }
  // A closed sheet holds no rows.
  lv_obj_clean(body_);
  use_24h_switch_ = manual_zone_switch_ = offset_label_ = offset_down_ = offset_up_ = nullptr;
  wifi_switch_ = ble_switch_ = radios_caption_ = nullptr;
  lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

bool SettingsPanel::visible() const { return !lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN); }

void SettingsPanel::Changed(const domain::DisplaySettings& next) {
  settings_ = domain::Sanitize(next);
  Refresh();
  if (on_change_) on_change_(settings_);
}

void SettingsPanel::Refresh() {
  if (use_24h_switch_ == nullptr) return;  // the Clock rows are not built
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
