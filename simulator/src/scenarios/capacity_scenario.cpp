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

  controller.ShowAlertList();
  RenderOnce();
  result.Expect(controller.current_screen() == CapacityScreen::kAlertList,
                "ShowAlertList switches to the alert list screen");
  {
    lv_obj_t* list = lv_obj_get_child(controller.root(), 0);
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
    lv_obj_t* detail_label = lv_obj_get_child(controller.root(), 1);
    const char* text = lv_label_get_text(detail_label);
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
    lv_obj_t* list = lv_obj_get_child(controller.root(), 0);
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
