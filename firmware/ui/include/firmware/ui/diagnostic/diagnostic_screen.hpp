// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/diagnostic.hpp"

namespace firmware::ui::diagnostic {

// Handles to the widgets a test (or the recalibration entrypoint) needs to
// reach directly, without depending on LVGL child-index order. Every handle
// is a plain LVGL object pointer; inspect widget contents via LVGL's own
// object-tree APIs (e.g. lv_label_get_text on the labels).
struct DiagnosticScreenHandles {
  lv_obj_t* root = nullptr;
  lv_obj_t* profile_label = nullptr;
  lv_obj_t* build_identity_label = nullptr;
  lv_obj_t* reset_reason_label = nullptr;
  // Scrollable container; one child label per peripheral row, in the same
  // order as `state.peripherals`, formatted "<name>: PRESENT|ABSENT (<detail>)".
  lv_obj_t* peripheral_list = nullptr;
  // "Recalibrate" button. This screen stays pure LVGL + firmware::domain
  // (no dependency on firmware::ui::calibration), so it does not wire a
  // click handler itself -- the caller (main.cpp) attaches one via
  // lv_obj_add_event_cb(handles.recalibrate_button, ..., LV_EVENT_CLICKED, ...)
  // to start firmware::ui::calibration::CalibrationFlow. Null if
  // `state.peripherals` reports touch absent/failed, since recalibration is
  // meaningless without a working touch panel.
  lv_obj_t* recalibrate_button = nullptr;
};

// Builds the 320x240 landscape diagnostic screen under `parent`: profile
// id, build identity, reset reason, a scrollable list of peripheral status
// rows, and (if touch is present) a recalibrate button. Pure LVGL +
// firmware::domain -- no ESP-IDF headers, so the simulator can build and
// inspect this with a fixture DiagnosticState and no hardware.
DiagnosticScreenHandles BuildDiagnosticScreen(lv_obj_t* parent,
                                               const firmware::domain::DiagnosticState& state);

}  // namespace firmware::ui::diagnostic
