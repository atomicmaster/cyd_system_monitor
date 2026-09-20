// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/diagnostic.hpp"

namespace firmware::ui::diagnostic {

// Handles to the widgets a test (or the navigation entrypoints) needs to
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
  // "Capacity slice" button, letting an operator reach the bounded F08a
  // capacity-slice screens for physical exercise. The caller
  // wires the click handler (this screen stays pure LVGL + firmware::domain);
  // null if `state.peripherals` reports touch absent/failed.
  lv_obj_t* capacity_slice_button = nullptr;
  // "Back to monitor" button, always present. Same caller-wires-the-handler
  // convention; returns to the monitoring landing screen.
  lv_obj_t* back_button = nullptr;
};

// Builds the 320x240 landscape diagnostic screen under `parent`: profile
// id, build identity, reset reason, a scrollable list of peripheral status
// rows, and (if touch is present) a capacity-slice button, and a back button. Pure LVGL +
// firmware::domain -- no ESP-IDF headers, so the simulator can build and
// inspect this with a fixture DiagnosticState and no hardware.
DiagnosticScreenHandles BuildDiagnosticScreen(lv_obj_t* parent,
                                              const firmware::domain::DiagnosticState& state);

}  // namespace firmware::ui::diagnostic
