// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>
#include <string>

#include "firmware/domain/display_settings.hpp"

namespace firmware::ui::system {

// The clock settings overlay: 24-hour or 12-hour, show or hide the timezone,
// and follow the Mac's zone or use a fixed UTC offset. It renders and edits a
// DisplaySettings value; persistence is the caller's, through `on_change`.
class SettingsPanel {
 public:
  using ChangeCallback = std::function<void(const firmware::domain::DisplaySettings&)>;
  SettingsPanel(lv_obj_t* parent, ChangeCallback on_change);
  ~SettingsPanel();
  SettingsPanel(const SettingsPanel&) = delete;
  SettingsPanel& operator=(const SettingsPanel&) = delete;

  void Show(const firmware::domain::DisplaySettings& current);
  void Hide();
  bool visible() const;
  const firmware::domain::DisplaySettings& settings() const { return settings_; }

  lv_obj_t* panel() const { return panel_; }
  lv_obj_t* use_24h_switch() const { return use_24h_switch_; }
  lv_obj_t* show_zone_switch() const { return show_zone_switch_; }
  lv_obj_t* manual_zone_switch() const { return manual_zone_switch_; }
  lv_obj_t* offset_label() const { return offset_label_; }
  lv_obj_t* offset_down_button() const { return offset_down_; }
  lv_obj_t* offset_up_button() const { return offset_up_; }
  lv_obj_t* done_button() const { return done_; }

 private:
  friend struct SettingsPanelEvents;
  void Refresh();
  void Changed(const firmware::domain::DisplaySettings& next);
  lv_obj_t* AddRow(const char* text, lv_obj_t** control, bool is_switch);

  ChangeCallback on_change_;
  firmware::domain::DisplaySettings settings_;
  lv_obj_t* panel_ = nullptr;
  lv_obj_t* use_24h_switch_ = nullptr;
  lv_obj_t* show_zone_switch_ = nullptr;
  lv_obj_t* manual_zone_switch_ = nullptr;
  lv_obj_t* offset_label_ = nullptr;
  lv_obj_t* offset_down_ = nullptr;
  lv_obj_t* offset_up_ = nullptr;
  lv_obj_t* done_ = nullptr;
};

// "UTC-5", "UTC+5:45", "UTC" for the offset label.
std::string OffsetText(int minutes_east);

}  // namespace firmware::ui::system
