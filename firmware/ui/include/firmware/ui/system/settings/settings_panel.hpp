// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <functional>
#include <string>

#include "firmware/domain/display_settings.hpp"

namespace firmware::ui::system {

// The settings sheet, in two sections chosen by a "Clock" | "Radios" selector in
// the header, of which only the shown one has rows (the LVGL pool is tight):
//   Clock: 24-hour or 12-hour, and follow the Mac's zone or use a fixed UTC offset.
//     It renders and edits a DisplaySettings value; persistence is the caller's,
//     through `on_change`.
//   Radios: independent WiFi observation and BLE observation switches, read from
//     and written to the radio task through `Actions`. Not persisted yet (TB16).
// Recalibrate, Debug and Done stay in the header in both sections.
class SettingsPanel {
 public:
  using ChangeCallback = std::function<void(const firmware::domain::DisplaySettings&)>;
  // Optional entries to screens outside the panel (touch recalibration, the debug landing screen).
  struct Actions {
    std::function<void()> on_recalibrate;
    std::function<void()> on_debug;
    // The radio config in force now: whether WiFi and BLE observation are enabled.
    std::function<void(bool& wifi, bool& ble)> read_radios;
    // Asks the radio task to enable or disable one radio; false when it was not queued.
    std::function<bool(bool wifi, bool on)> set_radio;
  };
  enum class Section { kClock, kRadios };
  SettingsPanel(lv_obj_t* parent, ChangeCallback on_change, Actions actions = {});
  ~SettingsPanel();
  SettingsPanel(const SettingsPanel&) = delete;
  SettingsPanel& operator=(const SettingsPanel&) = delete;

  // Opens the sheet at `section`.
  void Show(const firmware::domain::DisplaySettings& current, Section section = Section::kClock);
  // Re-reads the radio config into the Radios switches when that section is
  // shown and the config changed since the last read (a DEV command, say).
  void SyncRadios();
  Section section() const { return section_; }
  void Hide();
  bool visible() const;
  const firmware::domain::DisplaySettings& settings() const { return settings_; }

  lv_obj_t* panel() const { return panel_; }
  lv_obj_t* scrim() const { return scrim_; }
  lv_obj_t* use_24h_switch() const { return use_24h_switch_; }
  lv_obj_t* manual_zone_switch() const { return manual_zone_switch_; }
  lv_obj_t* offset_label() const { return offset_label_; }
  lv_obj_t* offset_down_button() const { return offset_down_; }
  lv_obj_t* offset_up_button() const { return offset_up_; }
  lv_obj_t* done_button() const { return done_; }
  lv_obj_t* recalibrate_button() const { return recalibrate_; }
  lv_obj_t* debug_button() const { return debug_; }
  lv_obj_t* clock_tab() const { return clock_tab_; }
  lv_obj_t* radios_tab() const { return radios_tab_; }
  // Null unless the Radios section is shown.
  lv_obj_t* wifi_switch() const { return wifi_switch_; }
  lv_obj_t* ble_switch() const { return ble_switch_; }
  lv_obj_t* radios_caption() const { return radios_caption_; }

 private:
  friend struct SettingsPanelEvents;
  void Refresh();
  void SelectSection(Section section);
  void BuildClockRows();
  void BuildRadioRows();
  void RadioToggled(bool wifi);
  void Changed(const firmware::domain::DisplaySettings& next);
  lv_obj_t* AddRow(const char* text, lv_obj_t** control, bool is_switch);

  ChangeCallback on_change_;
  Actions actions_;
  firmware::domain::DisplaySettings settings_;
  lv_obj_t* scrim_ = nullptr;  // full-screen tap-outside layer under the sheet
  lv_obj_t* panel_ = nullptr;
  Section section_ = Section::kClock;
  lv_obj_t* clock_tab_ = nullptr;
  lv_obj_t* radios_tab_ = nullptr;
  lv_obj_t* body_ = nullptr;  // holds only the shown section's rows
  lv_obj_t* wifi_switch_ = nullptr;
  lv_obj_t* ble_switch_ = nullptr;
  lv_obj_t* radios_caption_ = nullptr;
  bool read_wifi_ = false;  // the last config read (not the last touch), to spot a change
  bool read_ble_ = false;
  lv_obj_t* use_24h_switch_ = nullptr;
  lv_obj_t* manual_zone_switch_ = nullptr;
  lv_obj_t* offset_label_ = nullptr;
  lv_obj_t* offset_down_ = nullptr;
  lv_obj_t* offset_up_ = nullptr;
  lv_obj_t* done_ = nullptr;
  lv_obj_t* recalibrate_ = nullptr;
  lv_obj_t* debug_ = nullptr;
};

// "UTC-5", "UTC+5:45", "UTC" for the offset label.
std::string OffsetText(int minutes_east);

}  // namespace firmware::ui::system
