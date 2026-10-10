// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>
#include <optional>

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

// The clock settings panel and how it is reached: tapping the status bar.
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

  // --- reached through the status bar of the link screen ---
  std::optional<domain::DisplaySettings> saved;
  ui::LinkScreen link(
      screen, [](bool) {}, [&](const domain::DisplaySettings& s) { saved = s; },
      domain::DisplaySettings{.use_24h = false});
  link.Update(domain::CpuView{.connection = "Connected", .clock = "17:13"}, {}, domain::LinkView{});
  RenderOnce();
  result.Expect(!link.settings_panel().visible(), "settings are closed until the bar is tapped");
  // Fixed layout: none of these may scroll, so a drag on the bar moves nothing.
  for (lv_obj_t* fixed : {link.system().status_bar(), lv_obj_get_parent(link.system().status_bar()),
                          link.settings_panel().panel()}) {
    result.Expect(!lv_obj_has_flag(fixed, LV_OBJ_FLAG_SCROLLABLE),
                  "the status bar, its container, and the settings panel are not scrollable");
  }
  Click(link.system().status_bar());
  result.Expect(link.settings_panel().visible() && !link.settings_panel().settings().use_24h,
                "tapping the status bar opens settings with the current values");
  Toggle(link.settings_panel().use_24h_switch());
  result.Expect(saved.has_value() && saved->use_24h && link.settings().use_24h,
                "a change reaches the persistence callback and the screen's own settings");
  Click(link.settings_panel().done_button());

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

  // A pending pairing prompt has priority over settings.
  domain::LinkView prompt;
  prompt.prompt_visible = true;
  prompt.code = "123456";
  link.Update(domain::CpuView{}, {}, prompt);
  result.Expect(!lv_obj_has_flag(link.prompt_panel(), LV_OBJ_FLAG_HIDDEN) &&
                    std::strcmp(lv_label_get_text(link.code_label()), "123456") == 0,
                "a pending pairing shows its code");
  Click(link.system().status_bar());
  result.Expect(!link.settings_panel().visible(), "settings do not open over a pairing prompt");
  domain::LinkView none;
  link.Update(domain::CpuView{}, {}, none);
  Click(link.system().status_bar());
  result.Expect(link.settings_panel().visible(), "settings open again once the prompt is gone");
  link.Update(domain::CpuView{}, {}, prompt);
  result.Expect(!link.settings_panel().visible(),
                "a prompt arriving closes an open settings panel");
  Click(link.system().connection_label());
  result.Expect(!link.about_visible(), "the Host link panel does not open over a pairing prompt");
  return result.Finish("simulator settings");
}

}  // namespace simulator::scenarios
