// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/theme.hpp"

namespace firmware::ui::theme {

lv_color_t CardAccent(size_t index) {
  static constexpr uint32_t kAccents[] = {kCyan, kViolet, kBlue, kTeal, kPink, kMagenta};
  return Color(kAccents[index % (sizeof kAccents / sizeof kAccents[0])]);
}

lv_color_t AgentAccent(size_t slot) {
  static constexpr uint32_t kAccents[] = {kCyan, kMagenta, kViolet};
  return Color(kAccents[slot % (sizeof kAccents / sizeof kAccents[0])]);
}

void StylePanel(lv_obj_t* object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(object, Color(kPanel), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(object, Color(kPanelBorder), 0);
  lv_obj_set_style_border_width(object, 1, 0);
  lv_obj_set_style_radius(object, 6, 0);
  lv_obj_set_style_text_color(object, Color(kText), 0);
}

namespace {

// Shared styles: one copy in the LVGL pool however many objects use them.
struct Styles {
  lv_style_t caption, heading, sheet, text_button, pressed, disabled, rule;
  Styles() {
    lv_style_init(&caption);
    lv_style_set_text_font(&caption, &lv_font_montserrat_12);
    lv_style_set_text_color(&caption, Color(kMuted));
    lv_style_init(&heading);
    lv_style_set_text_font(&heading, &lv_font_montserrat_14);
    lv_style_set_text_color(&heading, Color(kText));
    lv_style_init(&sheet);
    lv_style_set_bg_color(&sheet, Color(kPanel));
    lv_style_set_bg_opa(&sheet, LV_OPA_COVER);
    lv_style_set_border_side(&sheet, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&sheet, Color(kPanelBorder));
    lv_style_set_border_width(&sheet, 1);
    lv_style_set_radius(&sheet, 14);
    lv_style_set_shadow_width(&sheet, 0);
    lv_style_set_pad_all(&sheet, 10);
    lv_style_set_pad_top(&sheet, 14);
    lv_style_set_text_color(&sheet, Color(kText));
    lv_style_init(&text_button);
    lv_style_set_bg_opa(&text_button, LV_OPA_TRANSP);
    lv_style_set_border_width(&text_button, 0);
    lv_style_set_shadow_width(&text_button, 0);
    lv_style_set_pad_hor(&text_button, 6);
    lv_style_init(&pressed);
    lv_style_set_bg_color(&pressed, Color(kPanelBorder));
    lv_style_set_bg_opa(&pressed, LV_OPA_COVER);
    lv_style_init(&disabled);
    lv_style_set_opa(&disabled, LV_OPA_40);
    lv_style_init(&rule);
    lv_style_set_border_side(&rule, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&rule, Color(kPanelBorder));
    lv_style_set_border_width(&rule, 1);
  }
};

Styles& Shared() {
  static Styles styles;
  return styles;
}

}  // namespace

void StyleCaption(lv_obj_t* label) { lv_obj_add_style(label, &Shared().caption, 0); }

void StyleHeading(lv_obj_t* label) { lv_obj_add_style(label, &Shared().heading, 0); }

void StyleSheet(lv_obj_t* sheet, int32_t height) {
  lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_style(sheet, &Shared().sheet, 0);
  // Flush with the bottom edge: anything below it would make the screen scroll.
  lv_obj_set_size(sheet, 320, height);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_t* handle = lv_obj_create(sheet);
  lv_obj_remove_style_all(handle);
  lv_obj_add_flag(handle, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(handle, 36, 4);
  lv_obj_set_style_radius(handle, 2, 0);
  lv_obj_set_style_bg_color(handle, Color(kPanelBorder), 0);
  lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
  lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -9);
}

void StyleTextButton(lv_obj_t* button, uint32_t color) {
  lv_obj_add_style(button, &Shared().text_button, 0);
  lv_obj_add_style(button, &Shared().pressed, LV_STATE_PRESSED);
  lv_obj_add_style(button, &Shared().disabled, LV_STATE_DISABLED);
  lv_obj_set_style_text_color(button, Color(color), 0);
}

void StyleSwitch(lv_obj_t* control) {
  lv_obj_set_style_bg_color(control, Color(kTrack), LV_PART_MAIN);
  lv_obj_set_style_bg_color(control, Color(kCyan),
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(control, Color(kText), LV_PART_KNOB);
}

void StyleRowRule(lv_obj_t* row) { lv_obj_add_style(row, &Shared().rule, 0); }

}  // namespace firmware::ui::theme
