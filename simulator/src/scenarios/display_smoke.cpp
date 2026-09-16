// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>

#include "../expect.hpp"
#include "../fixtures.hpp"
#include "../lvgl_env.hpp"
#include "firmware/ui/diagnostic/diagnostic_screen.hpp"
#include "scenarios.hpp"

namespace simulator::scenarios {

int RunDisplaySmoke() {
  ScenarioResult result;

  lv_obj_t* screen = InitLvglHeadless();
  const auto state = fixtures::MakeDiagnosticStateFixture();
  const auto handles = firmware::ui::diagnostic::BuildDiagnosticScreen(screen, state);
  RenderOnce();

  result.Expect(handles.root != nullptr, "diagnostic screen root object is created");

  const char* profile_text = lv_label_get_text(handles.profile_label);
  result.Expect(std::strstr(profile_text, state.profile_id.c_str()) != nullptr,
                "profile label contains the active profile id");

  const char* build_text = lv_label_get_text(handles.build_identity_label);
  result.Expect(std::strstr(build_text, "simulator-dev-build") != nullptr,
                "build identity label contains the fixture build identity");

  const char* reset_text = lv_label_get_text(handles.reset_reason_label);
  result.Expect(std::strstr(reset_text, "power-on") != nullptr,
                "reset reason label reports the fixture power-on reason");

  const uint32_t child_count = lv_obj_get_child_count(handles.peripheral_list);
  result.Expect(child_count == state.peripherals.size(),
                "peripheral list has one row per fixture peripheral");

  result.Expect(handles.recalibrate_button != nullptr,
                "recalibrate button is present when the fixture reports touch present");
  result.Expect(handles.capacity_slice_button != nullptr,
                "capacity slice button is present when the fixture reports touch present");

  // F06's recalibration entrypoint is meaningless without a working touch
  // panel; a fixture with touch absent must not offer it. The capacity
  // slice button shares that condition, since reaching it also requires
  // touch.
  auto state_no_touch = state;
  state_no_touch.peripherals[0] = firmware::domain::PeripheralStatus{"touch", false, "init failed"};
  const auto handles_no_touch =
      firmware::ui::diagnostic::BuildDiagnosticScreen(screen, state_no_touch);
  result.Expect(handles_no_touch.recalibrate_button == nullptr,
                "no recalibrate button is offered when touch is absent");
  result.Expect(handles_no_touch.capacity_slice_button == nullptr,
                "no capacity slice button is offered when touch is absent");

  return result.Finish("simulator display-smoke");
}

}  // namespace simulator::scenarios
