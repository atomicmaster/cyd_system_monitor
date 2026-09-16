// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/diagnostic/diagnostic_screen.hpp"

#include <cstdio>

#include "firmware/domain/reset_reason.hpp"

namespace firmware::ui::diagnostic {

namespace {

lv_obj_t* MakeInfoLabel(lv_obj_t* parent) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  return label;
}

}  // namespace

DiagnosticScreenHandles BuildDiagnosticScreen(lv_obj_t* parent,
                                              const firmware::domain::DiagnosticState& state) {
  DiagnosticScreenHandles handles;

  handles.root = lv_obj_create(parent);
  lv_obj_set_size(handles.root, 320, 240);
  lv_obj_set_flex_flow(handles.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(handles.root, 4, 0);
  lv_obj_set_style_pad_row(handles.root, 2, 0);

  handles.profile_label = MakeInfoLabel(handles.root);
  lv_label_set_text_fmt(handles.profile_label, "profile: %s", state.profile_id.c_str());

  handles.build_identity_label = MakeInfoLabel(handles.root);
  lv_label_set_text_fmt(handles.build_identity_label, "build: %s", state.build_identity.c_str());

  handles.reset_reason_label = MakeInfoLabel(handles.root);
  lv_label_set_text_fmt(handles.reset_reason_label, "reset: %s",
                        firmware::domain::ToString(state.reset_reason));

  handles.peripheral_list = lv_obj_create(handles.root);
  lv_obj_set_width(handles.peripheral_list, LV_PCT(100));
  lv_obj_set_flex_grow(handles.peripheral_list, 1);
  lv_obj_set_flex_flow(handles.peripheral_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(handles.peripheral_list, LV_OBJ_FLAG_SCROLLABLE);

  bool touch_present = false;
  for (const auto& peripheral : state.peripherals) {
    lv_obj_t* row = MakeInfoLabel(handles.peripheral_list);
    lv_label_set_text_fmt(row, "%s: %s (%s)", peripheral.name.c_str(),
                          peripheral.present ? "PRESENT" : "ABSENT", peripheral.detail.c_str());
    if (peripheral.name == "touch" && peripheral.present) {
      touch_present = true;
    }
  }

  if (touch_present) {
    handles.recalibrate_button = lv_button_create(handles.root);
    lv_obj_t* label = lv_label_create(handles.recalibrate_button);
    lv_label_set_text(label, "Recalibrate touch");
  }

  return handles;
}

}  // namespace firmware::ui::diagnostic
