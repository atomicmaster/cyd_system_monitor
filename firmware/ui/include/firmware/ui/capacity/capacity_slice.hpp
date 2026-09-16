// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <string>
#include <vector>

namespace firmware::ui::capacity {

// One bounded row of the capacity slice's Alert list. `detail` is only
// shown by ShowAlertDetail, not the list row itself.
struct AlertRow {
  std::string severity;
  std::string title;
  std::string detail;
};

// One bounded row of the capacity slice's Settings navigation list.
struct SettingRow {
  std::string label;
  std::string value;
};

// Fixture/real content the capacity slice renders. This stands in for the
// eventual MVP Host state and Alert/Settings sources (F08a Work item 2):
// platform/esp32 or a future real source populates it; the simulator
// supplies a maximum-size fixture. Handing in more rows than
// firmware::domain::capacity's declared bounds does not grow the rendered
// object tree -- CapacitySliceController clamps at build time.
struct CapacitySliceState {
  std::string live_status_text;
  std::vector<AlertRow> alerts;
  std::vector<SettingRow> settings;
};

enum class CapacityScreen {
  kLiveStatus,
  kAlertList,
  kAlertDetail,
  kSettings,
};

// Owns exactly one live capacity-slice screen under `parent` at a time,
// mirroring firmware/main/main.cpp's manual delete-then-rebuild navigation
// (AppState) so the bounded operational slice can be exercised
// off-target (F08a Work item 4: "navigation that creates and destroys the
// capacity-slice screens"). Every Show*() call destroys the previously
// built screen before constructing the next one -- no two capacity-slice
// screens are ever live at once, and repeated navigation never leaks
// widgets.
class CapacitySliceController {
 public:
  CapacitySliceController(lv_obj_t* parent, CapacitySliceState state);
  ~CapacitySliceController();

  CapacitySliceController(const CapacitySliceController&) = delete;
  CapacitySliceController& operator=(const CapacitySliceController&) = delete;

  void ShowLiveStatus();
  void ShowAlertList();
  // No-op (stays on whatever screen was already live) if `alert_index` is
  // out of range for the bounded, clamped alert row count -- there is no
  // detail to show past what the list itself rendered.
  void ShowAlertDetail(size_t alert_index);
  void ShowSettings();

  CapacityScreen current_screen() const { return current_screen_; }
  // The currently live screen's root object, or nullptr if none has been
  // built yet.
  lv_obj_t* root() const { return root_; }

 private:
  void DestroyCurrent();

  lv_obj_t* parent_;
  CapacitySliceState state_;
  CapacityScreen current_screen_ = CapacityScreen::kLiveStatus;
  lv_obj_t* root_ = nullptr;
};

}  // namespace firmware::ui::capacity
