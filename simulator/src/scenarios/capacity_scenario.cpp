// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>

#include "../expect.hpp"
#include "../fixtures.hpp"
#include "../lvgl_env.hpp"
#include "firmware/domain/capacity.hpp"
#include "firmware/ui/capacity/capacity_slice.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {

int RunCapacity() {
  ScenarioResult result;
  namespace bounds = firmware::domain::capacity;
  using firmware::ui::capacity::CapacityScreen;
  using firmware::ui::capacity::CapacitySliceController;

  lv_obj_t* screen = InitLvglHeadless();
  const auto state = fixtures::MakeCapacitySliceStateFixture();

  // The fixture supplies one more alert/setting row than the declared
  // maximum -- confirm the source data really is oversized before relying
  // on the controller to have clamped it.
  result.Expect(state.alerts.size() == bounds::kMaxAlertListRows + 1,
                "fixture supplies one more alert row than the declared maximum");
  result.Expect(state.settings.size() == bounds::kMaxSettingsRows + 1,
                "fixture supplies one more settings row than the declared maximum");

  CapacitySliceController controller(screen, state);
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kLiveStatus,
                "the controller starts on the live status screen");
  result.Expect(controller.root() != nullptr, "the live status screen has a root object");
  result.Expect(controller.live_status_label() != nullptr,
                "the live status screen exposes its label through a named handle");

  // Work item 3's declared refresh-cadence bound: a second update inside
  // the minimum interval must not redraw the label, but the very first
  // update after ShowLiveStatus() always applies.
  uint32_t sim_now_ms = 1000;
  const bool first_update = controller.UpdateLiveStatus("first reading", sim_now_ms);
  result.Expect(first_update,
                "the first live status update after showing the screen always redraws");
  result.Expect(
      std::strcmp(lv_label_get_text(controller.live_status_label()), "first reading") == 0,
      "the first live status update's text reaches the label");

  sim_now_ms += bounds::kMinLiveStatusRefreshIntervalMs / 2;
  const bool rate_limited_update = controller.UpdateLiveStatus("too soon", sim_now_ms);
  result.Expect(!rate_limited_update,
                "an update inside the declared minimum refresh interval is rate limited");
  result.Expect(
      std::strcmp(lv_label_get_text(controller.live_status_label()), "first reading") == 0,
      "a rate-limited update leaves the on-screen label unchanged");

  sim_now_ms += bounds::kMinLiveStatusRefreshIntervalMs;
  const bool second_update = controller.UpdateLiveStatus("second reading", sim_now_ms);
  result.Expect(second_update,
                "an update at or past the declared minimum refresh interval redraws the label");
  result.Expect(
      std::strcmp(lv_label_get_text(controller.live_status_label()), "second reading") == 0,
      "the applied update's text reaches the label");

  controller.ShowAlertList();
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kAlertList,
                "ShowAlertList switches to the alert list screen");
  {
    lv_obj_t* list = controller.alert_list();
    result.Expect(list != nullptr, "the alert list screen exposes its list through a named handle");
    const uint32_t row_count = lv_obj_get_child_count(list);
    result.Expect(row_count == bounds::kMaxAlertListRows,
                  "the alert list renders at most the declared maximum row count despite the "
                  "oversized fixture");

    lv_obj_t* first_row = lv_obj_get_child(list, 0);
    const char* text = lv_label_get_text(first_row);
    result.Expect(std::strlen(text) <= bounds::kMaxAlertLabelLength + 16,
                  "an oversized alert title is truncated rather than rendered in full");
  }

  controller.ShowAlertDetail(0);
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kAlertDetail,
                "ShowAlertDetail(0) switches to the alert detail screen for an in-range index");
  {
    result.Expect(
        controller.alert_detail_title() != nullptr && controller.alert_detail_body() != nullptr,
        "the alert detail screen exposes its title and body through named handles");
    const char* text = lv_label_get_text(controller.alert_detail_body());
    result.Expect(std::strlen(text) <= bounds::kMaxAlertDetailLength,
                  "the alert detail text is truncated to the declared maximum length");
  }

  // Out-of-range detail request (the oversized fixture's extra row was
  // clamped away by the list, so its index is out of range for detail
  // too): the controller must not build a screen for a row it never
  // rendered, and must leave the previously live screen untouched.
  controller.ShowAlertDetail(bounds::kMaxAlertListRows);
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kAlertDetail,
                "an out-of-range alert detail request leaves the current screen unchanged");

  controller.ShowSettings();
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kSettings,
                "ShowSettings switches to the settings navigation screen");
  {
    lv_obj_t* list = controller.settings_list();
    result.Expect(list != nullptr, "the settings screen exposes its list through a named handle");
    const uint32_t row_count = lv_obj_get_child_count(list);
    result.Expect(row_count == bounds::kMaxSettingsRows,
                  "the settings screen renders at most the declared maximum row count despite the "
                  "oversized fixture");
  }

  // Repeated navigation across every screen must keep leaving exactly one
  // live root behind -- F08a Work item 4's "verify that the bounded UI
  // remains valid after repeated navigation."
  for (int cycle = 0; cycle < 5; ++cycle) {
    controller.ShowLiveStatus();
    controller.ShowAlertList();
    controller.ShowAlertDetail(0);
    controller.ShowSettings();
    RenderOnce();
    result.Expect(controller.root() != nullptr,
                  "repeated navigation always leaves exactly one live screen root");
    result.Expect(lv_obj_get_parent(controller.root()) == screen,
                  "the live screen root stays attached directly under the capacity-slice parent");
  }

  return result.Finish("simulator capacity");
}

}  // namespace simulator::scenarios
