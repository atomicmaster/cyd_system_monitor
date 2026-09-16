// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>
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
//
// Exposes the current screen's widgets through named accessors (mirroring
// firmware::ui::diagnostic::DiagnosticScreenHandles), not positional
// lv_obj_get_child() indexing -- each accessor is only valid while
// current_screen() reports the matching screen, and returns nullptr
// otherwise.
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

  // Updates the live status text without rebuilding the screen, rate
  // limited to at most once per firmware::domain::capacity's declared
  // kMinLiveStatusRefreshIntervalMs (Work item 3's "upper bound for ...
  // refresh cadence"): a call inside that window updates the stored state
  // (so the next allowed refresh reflects the latest value) but leaves the
  // on-screen label untouched. Returns true if the label was actually
  // redrawn. A no-op (but still rate-tracked) unless current_screen() is
  // kLiveStatus.
  bool UpdateLiveStatus(std::string live_status_text, uint32_t now_ms);

  // Cycles kLiveStatus -> kAlertList -> kAlertDetail(0) -> kSettings ->
  // kLiveStatus -> ... one step per call. This is the capacity slice's own
  // exercise order (Work item 5: "exercise ... the capacity slice with
  // active touch"); firmware/main/main.cpp wires one tap-to-advance
  // handler to it rather than each screen owning separate navigation
  // widgets, since TB17 (not this ticket) owns the MVP's real navigation.
  void AdvanceScreen();

  CapacityScreen current_screen() const { return current_screen_; }
  // The currently live screen's root object, or nullptr if none has been
  // built yet.
  lv_obj_t* root() const { return root_; }

  // Valid only while current_screen() == kLiveStatus.
  lv_obj_t* live_status_label() const { return live_status_label_; }
  // Valid only while current_screen() == kAlertList. One child label per
  // rendered (bounded) alert row, in the same order as the clamped prefix
  // of state.alerts.
  lv_obj_t* alert_list() const { return alert_list_; }
  // Valid only while current_screen() == kAlertDetail.
  lv_obj_t* alert_detail_title() const { return alert_detail_title_; }
  lv_obj_t* alert_detail_body() const { return alert_detail_body_; }
  // Valid only while current_screen() == kSettings. One child label per
  // rendered (bounded) settings row.
  lv_obj_t* settings_list() const { return settings_list_; }

  // A dedicated "Next" button present on every screen, positioned outside
  // and after the scrollable list containers (alert_list_/settings_list_).
  // Physical testing found that a whole-root LV_EVENT_CLICKED handler
  // never fires on kAlertList/kSettings: those screens' scrollable list
  // covers nearly the whole root, and LVGL delivers the press/release to
  // the list (which consumes it for its own scroll-gesture detection)
  // rather than bubbling it up to root. A real, always-on-top button
  // avoids that ambiguity entirely. Never null once a screen is built.
  lv_obj_t* next_button() const { return next_button_; }

 private:
  void DestroyCurrent();
  void AddNextButton();

  lv_obj_t* parent_;
  CapacitySliceState state_;
  CapacityScreen current_screen_ = CapacityScreen::kLiveStatus;
  lv_obj_t* root_ = nullptr;

  lv_obj_t* live_status_label_ = nullptr;
  lv_obj_t* alert_list_ = nullptr;
  lv_obj_t* alert_detail_title_ = nullptr;
  lv_obj_t* alert_detail_body_ = nullptr;
  lv_obj_t* settings_list_ = nullptr;
  lv_obj_t* next_button_ = nullptr;

  // 0 means "never refreshed yet"; the first UpdateLiveStatus() call after
  // construction always redraws regardless of now_ms.
  uint32_t last_live_status_refresh_ms_ = 0;
  bool live_status_refreshed_once_ = false;
};

// A bounded, representative maximum-size CapacitySliceState: exactly
// firmware::domain::capacity's declared kMaxAlertListRows/kMaxSettingsRows
// rows, each at its declared maximum label/detail length (F08a Work item
// 2's "placeholders are permitted only when their object count, text
// length, refresh behavior, and memory reservation are explicit"). Used by
// firmware/main/main.cpp to drive the physical capacity slice exercise
// until a real Alert/Settings source exists; the simulator's own fixture
// (deliberately oversized, to test clamping) stays separate from this one.
CapacitySliceState MakeMaxRepresentativeState();

}  // namespace firmware::ui::capacity
