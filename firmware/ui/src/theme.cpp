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

void StyleCaption(lv_obj_t* label) {
  lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(label, Color(kMuted), 0);
}

}  // namespace firmware::ui::theme
