// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>
#include <optional>
#include <vector>

#include "../expect.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/link_view.hpp"
#include "firmware/ui/system/link/link_screen.hpp"
#include "firmware/ui/system/settings/settings_panel.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {
namespace {

// Whether any label under `object` contains `text`.
bool ContainsText(lv_obj_t* object, const char* text) {
  if (lv_obj_check_type(object, &lv_label_class) &&
      std::strstr(lv_label_get_text(object), text) != nullptr) {
    return true;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
    if (ContainsText(lv_obj_get_child(object, static_cast<int32_t>(i)), text)) return true;
  }
  return false;
}

namespace domain = firmware::domain;
namespace ui = firmware::ui::system;

bool Checked(lv_obj_t* sw) { return lv_obj_has_state(sw, LV_STATE_CHECKED); }
bool Disabled(lv_obj_t* button) { return lv_obj_has_state(button, LV_STATE_DISABLED); }

void Toggle(lv_obj_t* sw) {
  if (Checked(sw)) {
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
  } else {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

void Click(lv_obj_t* object) { lv_obj_send_event(object, LV_EVENT_CLICKED, nullptr); }

}  // namespace

// The settings sheet (Clock and Radios sections) and how each is reached from the status bar.
int RunSettings() {
  ScenarioResult result;
  lv_obj_t* screen = InitLvglHeadless();

  // --- the panel on its own ---
  int changes = 0;
  domain::DisplaySettings last;
  ui::SettingsPanel panel(screen, [&](const domain::DisplaySettings& s) {
    ++changes;
    last = s;
  });
  result.Expect(!panel.visible(), "the panel starts hidden");
  panel.Show({});
  RenderOnce();
  result.Expect(panel.visible(), "Show makes the panel visible");
  result.Expect(Checked(panel.use_24h_switch()) && !Checked(panel.manual_zone_switch()),
                "defaults: 24-hour on, following the Mac");
  result.Expect(std::strstr(lv_label_get_text(panel.offset_label()), "Follows the Mac") != nullptr,
                "with no manual zone the offset row says it follows the Mac");
  result.Expect(Disabled(panel.offset_up_button()) && Disabled(panel.offset_down_button()),
                "offset buttons are disabled while following the Mac");

  Click(panel.offset_up_button());
  result.Expect(changes == 0, "a disabled offset button changes nothing");

  Toggle(panel.use_24h_switch());
  result.Expect(changes == 1 && !last.use_24h, "turning 24-hour off reports 12-hour");
  Toggle(panel.manual_zone_switch());
  result.Expect(changes == 2 && last.manual_zone && last.manual_offset_minutes == 0,
                "turning the manual zone on starts at UTC");
  result.Expect(std::strcmp(lv_label_get_text(panel.offset_label()), "UTC") == 0,
                "the offset row reads UTC");
  result.Expect(!Disabled(panel.offset_up_button()) && !Disabled(panel.offset_down_button()),
                "offset buttons are enabled for a manual zone");

  for (int i = 0; i < 3; ++i) Click(panel.offset_up_button());
  result.Expect(last.manual_offset_minutes == 45, "three +15m taps give +45 minutes");
  result.Expect(std::strcmp(lv_label_get_text(panel.offset_label()), "UTC+0:45") == 0,
                "the offset row reads UTC+0:45");
  for (int i = 0; i < 24; ++i) Click(panel.offset_down_button());
  result.Expect(last.manual_offset_minutes == -315 &&
                    std::strcmp(lv_label_get_text(panel.offset_label()), "UTC-5:15") == 0,
                "stepping down crosses zero to UTC-5:15");
  for (int i = 0; i < 100; ++i) Click(panel.offset_down_button());
  result.Expect(last.manual_offset_minutes == domain::kMinOffsetMinutes,
                "the offset stops at UTC-12");

  Click(panel.done_button());
  result.Expect(!panel.visible(), "Done hides the panel");

  // --- the Radios section on its own, against a stub radio task ---
  {
    using Section = ui::SettingsPanel::Section;
    struct Request {
      bool wifi;
      bool on;
    };
    std::vector<Request> requests;
    bool wifi_on = true, ble_on = true, queue_ok = true;
    ui::SettingsPanel::Actions actions;
    actions.read_radios = [&](bool& wifi, bool& ble) {
      wifi = wifi_on;
      ble = ble_on;
    };
    actions.set_radio = [&](bool wifi, bool on) {
      requests.push_back({wifi, on});
      return queue_ok;
    };
    ui::SettingsPanel radios(screen, [](const domain::DisplaySettings&) {}, actions);
    radios.Show({}, Section::kRadios);
    RenderOnce();
    result.Expect(radios.section() == Section::kRadios && radios.wifi_switch() != nullptr &&
                      radios.ble_switch() != nullptr && radios.use_24h_switch() == nullptr,
                  "Radios builds its two switches and none of the Clock rows");
    result.Expect(ContainsText(radios.panel(), "WiFi observation") &&
                      ContainsText(radios.panel(), "BLE observation") &&
                      ContainsText(radios.panel(), "Operator disabled") &&
                      ContainsText(radios.panel(), "Not saved across restarts"),
                  "Radios names both observations and explains Operator disabled and no saving");
    result.Expect(Checked(radios.wifi_switch()) && Checked(radios.ble_switch()),
                  "both switches start on when both radios observe");
    for (lv_obj_t* kept :
         {radios.recalibrate_button(), radios.debug_button(), radios.done_button()}) {
      lv_area_t box;
      lv_obj_get_coords(kept, &box);
      result.Expect(box.y1 >= 0 && box.y2 <= 239 && box.x1 >= 0 && box.x2 <= 319 &&
                        !lv_obj_has_flag(kept, LV_OBJ_FLAG_HIDDEN),
                    "Recalibrate, Debug and Done stay on screen in Radios");
    }
    Toggle(radios.wifi_switch());
    result.Expect(requests.size() == 1 && requests[0].wifi && !requests[0].on,
                  "switching WiFi off sends a WiFi-disable request");
    Toggle(radios.ble_switch());
    result.Expect(requests.size() == 2 && !requests[1].wifi && !requests[1].on,
                  "switching BLE off sends a BLE-disable request");
    Toggle(radios.wifi_switch());
    result.Expect(requests.size() == 3 && requests[2].wifi && requests[2].on,
                  "switching WiFi back on sends a WiFi-enable request");
    // The radio task applies them later; a sync while it has not caught up
    // must not undo the finger, and a DEV command that changes the config must show.
    radios.SyncRadios();
    result.Expect(Checked(radios.wifi_switch()) && !Checked(radios.ble_switch()),
                  "a sync before the config changes leaves the switches as touched");
    ble_on = false;  // the radio task applies the BLE request
    radios.SyncRadios();
    result.Expect(Checked(radios.wifi_switch()) && !Checked(radios.ble_switch()),
                  "a sync once the config catches up leaves them as touched");
    wifi_on = false;
    ble_on = true;
    radios.SyncRadios();
    result.Expect(!Checked(radios.wifi_switch()) && Checked(radios.ble_switch()),
                  "a config change from elsewhere moves the switches while shown");
    queue_ok = false;
    Toggle(radios.wifi_switch());
    result.Expect(!Checked(radios.wifi_switch()) && requests.size() == 4,
                  "a request the radio task did not queue puts the switch back");
    // Reopening reads the config afresh.
    radios.Hide();
    wifi_on = true;
    ble_on = false;
    radios.Show({}, Section::kRadios);
    result.Expect(Checked(radios.wifi_switch()) && !Checked(radios.ble_switch()),
                  "showing the section reads the current radio config");
    // The selector moves between sections and builds only the shown rows.
    Click(radios.clock_tab());
    result.Expect(radios.section() == Section::kClock && radios.wifi_switch() == nullptr &&
                      radios.use_24h_switch() != nullptr,
                  "the Clock tab swaps the Radios rows for the Clock rows");
    Click(radios.radios_tab());
    result.Expect(radios.wifi_switch() != nullptr && radios.use_24h_switch() == nullptr,
                  "the Radios tab swaps them back");
    radios.Hide();
    result.Expect(lv_obj_get_child_count(lv_obj_get_child(radios.panel(), 2)) == 0,
                  "a closed sheet holds no rows");
  }

  // --- reached through the status bar of the link screen ---
  std::optional<domain::DisplaySettings> saved;
  ui::LinkScreen link(
      screen, [](bool) {}, [&](const domain::DisplaySettings& s) { saved = s; },
      domain::DisplaySettings{.use_24h = false});
  link.Update(domain::CpuView{.connection = "Connected", .clock = "17:13"}, {}, domain::LinkView{});
  RenderOnce();
  result.Expect(!link.settings_panel().visible(), "settings are closed until the gear is tapped");
  // Fixed layout: none of these may scroll, so a drag on the bar moves nothing.
  for (lv_obj_t* fixed : {link.system().status_bar(), lv_obj_get_parent(link.system().status_bar()),
                          link.settings_panel().panel()}) {
    result.Expect(!lv_obj_has_flag(fixed, LV_OBJ_FLAG_SCROLLABLE),
                  "the status bar, its container, and the settings panel are not scrollable");
  }
  Click(link.system().gear());
  result.Expect(link.settings_panel().visible() && !link.settings_panel().settings().use_24h &&
                    link.settings_panel().section() == ui::SettingsPanel::Section::kClock,
                "tapping the gear opens the clock settings with the current values");
  Toggle(link.settings_panel().use_24h_switch());
  result.Expect(saved.has_value() && saved->use_24h && link.settings().use_24h,
                "a change reaches the persistence callback and the screen's own settings");
  Click(link.settings_panel().done_button());
  result.Expect(link.settings_panel().scrim() == nullptr, "Done removes the scrim with the sheet");
  // The bar itself opens nothing; the clock is a second path to settings.
  Click(link.system().status_bar());
  result.Expect(!link.settings_panel().visible(), "the bar's empty space opens nothing");
  Click(link.system().clock_label());
  result.Expect(link.settings_panel().visible() &&
                    link.settings_panel().section() == ui::SettingsPanel::Section::kClock,
                "tapping the clock also opens the clock settings");
  RenderOnce();
  // A tap outside the sheet closes it: the scrim sits above the bar, so the
  // bar underneath never sees that tap.
  {
    lv_obj_t* scrim = link.settings_panel().scrim();
    result.Expect(scrim != nullptr &&
                      lv_obj_get_index(scrim) >
                          lv_obj_get_index(lv_obj_get_parent(link.system().status_bar())) &&
                      lv_obj_get_index(link.settings_panel().panel()) > lv_obj_get_index(scrim),
                  "the open sheet has a scrim above the Overview and below the sheet");
    lv_area_t coords;
    lv_obj_get_coords(scrim, &coords);
    result.Expect(coords.x1 == 0 && coords.y1 == 0 && coords.x2 == 319 && coords.y2 == 239 &&
                      lv_obj_has_flag(scrim, LV_OBJ_FLAG_CLICKABLE),
                  "the scrim is a clickable full-screen layer, covering the status bar");
    // Where a finger would land: the connection text and the gear both sit
    // under the scrim, which takes the touch instead.
    for (lv_obj_t* under : {link.system().connection_label(), link.system().gear()}) {
      lv_area_t box;
      lv_obj_get_coords(under, &box);
      lv_point_t point{(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
      result.Expect(lv_indev_search_obj(lv_screen_active(), &point) == scrim,
                    "a touch on the bar while the sheet is open lands on the scrim");
    }
    Click(scrim);
    result.Expect(!link.settings_panel().visible() && link.settings_panel().scrim() == nullptr,
                  "tapping outside the sheet closes it and removes the scrim");
  }

  // --- which part of the bar opens which section, by where a finger lands ---
  {
    using Section = ui::SettingsPanel::Section;
    // Taps the bar at (x, y) as a finger would: whatever clickable object is
    // there gets the click. Returns that object.
    const auto tap = [&](int32_t x, int32_t y) {
      lv_point_t point{x, y};
      lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &point);
      if (hit != nullptr) Click(hit);
      return hit;
    };
    const auto centre_of = [](lv_obj_t* object) {
      lv_area_t box;
      lv_obj_get_coords(object, &box);
      return lv_point_t{(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    };
    const auto close = [&] { link.settings_panel().Hide(); };
    auto& sheet = link.settings_panel();
    auto& bar = link.system();

    for (lv_obj_t* slot : {bar.wifi_group(), bar.ble_group()}) {
      lv_area_t box;
      lv_obj_get_coords(slot, &box);
      // Its centre, both corners of its own box, and the 4 px margin round it.
      const lv_point_t points[] = {centre_of(slot),
                                   {box.x1, box.y1},
                                   {box.x2, box.y2},
                                   {box.x1 - 3, box.y1 - 3},
                                   {box.x2 + 3, box.y2 + 3}};
      bool all = true;
      for (const lv_point_t& at : points) {
        close();
        tap(at.x, at.y);
        all = all && sheet.visible() && sheet.section() == Section::kRadios;
      }
      result.Expect(all, "a tap anywhere on a WiFi or BLE slot (margin included) opens Radios");
    }
    // The icon, flag and channel inside a slot are not buttons of their own.
    for (lv_obj_t* part : {bar.wifi_icon(), bar.wifi_flag_label(), bar.wifi_channel_label(),
                           bar.ble_icon(), bar.ble_flag_label()}) {
      close();
      tap(centre_of(part).x, centre_of(part).y);
      result.Expect(sheet.visible() && sheet.section() == Section::kRadios,
                    "a tap on a slot's icon, flag or channel opens Radios");
    }
    for (lv_obj_t* target : {bar.clock_label(), bar.gear()}) {
      close();
      tap(centre_of(target).x, centre_of(target).y);
      result.Expect(sheet.visible() && sheet.section() == Section::kClock,
                    "a tap on the clock or the gear opens Clock");
    }
    // The flexible centre, between the connection text and the tray, opens nothing.
    close();
    lv_area_t connection_box, tray_box;
    lv_obj_get_coords(bar.connection_label(), &connection_box);
    lv_obj_get_coords(bar.radio_tray(), &tray_box);
    const int32_t middle = (connection_box.x2 + 8 + tray_box.x1 - 4) / 2;
    tap(middle, centre_of(bar.status_bar()).y);
    result.Expect(!sheet.visible() && !link.about_visible(),
                  "a tap in the bar's flexible centre opens nothing");
    // The connection text still opens the Host link sheet.
    tap(centre_of(bar.connection_label()).x, centre_of(bar.connection_label()).y);
    result.Expect(link.about_visible() && !sheet.visible(),
                  "the connection text still opens the Host link sheet");
    Click(link.about_done_button());
  }

  // Tapping the connection text opens the Host link panel instead: versions,
  // pairing and any fault, which no longer sit over the Overview's cards.
  domain::LinkView details;
  details.details = {
      {"Supported", "1.0"}, {"Host", "1.0"}, {"Negotiated", "1.0"}, {"Pairing", "Paired"}};
  link.Update(domain::CpuView{.connection = "Connected", .clock = "17:13"}, {}, details);
  result.Expect(!link.about_visible(), "the Host link panel is closed until tapped");
  Click(link.system().connection_label());
  result.Expect(link.about_visible() && !link.settings_panel().visible() &&
                    ContainsText(link.details_list(), "Negotiated") &&
                    ContainsText(link.details_list(), "Paired"),
                "tapping the connection text opens the Host link panel, not settings");
  Click(link.about_done_button());
  result.Expect(!link.about_visible(), "Done closes the Host link panel");
  Click(link.system().connection_label());
  result.Expect(link.about_visible(), "the Host link panel opens again");
  lv_obj_t* about_scrim = link.about_scrim();
  result.Expect(about_scrim != nullptr &&
                    lv_obj_get_index(about_scrim) < lv_obj_get_index(link.about_panel()),
                "the open Host link sheet has a scrim under it");
  Click(about_scrim);
  result.Expect(!link.about_visible() && link.about_scrim() == nullptr,
                "tapping outside the Host link sheet closes it and removes its scrim");

  // A pending pairing prompt has priority over settings.
  domain::LinkView prompt;
  prompt.prompt_visible = true;
  prompt.code = "123456";
  link.Update(domain::CpuView{}, {}, prompt);
  result.Expect(!lv_obj_has_flag(link.prompt_panel(), LV_OBJ_FLAG_HIDDEN) &&
                    std::strcmp(lv_label_get_text(link.code_label()), "123456") == 0,
                "a pending pairing shows its code");
  Click(link.system().gear());
  result.Expect(!link.settings_panel().visible(), "settings do not open over a pairing prompt");
  domain::LinkView none;
  link.Update(domain::CpuView{}, {}, none);
  Click(link.system().gear());
  result.Expect(link.settings_panel().visible(), "settings open again once the prompt is gone");
  link.Update(domain::CpuView{}, {}, prompt);
  result.Expect(!link.settings_panel().visible(),
                "a prompt arriving closes an open settings panel");
  Click(link.system().connection_label());
  result.Expect(!link.about_visible(), "the Host link panel does not open over a pairing prompt");
  return result.Finish("simulator settings");
}

}  // namespace simulator::scenarios
