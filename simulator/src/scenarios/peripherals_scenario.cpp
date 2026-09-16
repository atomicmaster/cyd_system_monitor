// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>

#include "firmware/ui/diagnostic/diagnostic_screen.hpp"
#include "scenarios.hpp"

#include "../expect.hpp"
#include "../fixtures.hpp"
#include "../lvgl_env.hpp"

namespace simulator::scenarios {

int RunPeripherals() {
  ScenarioResult result;

  lv_obj_t* screen = InitLvglHeadless();
  const auto state = fixtures::MakeDiagnosticStateFixture();
  const auto handles = firmware::ui::diagnostic::BuildDiagnosticScreen(screen, state);
  RenderOnce();

  const uint32_t child_count = lv_obj_get_child_count(handles.peripheral_list);
  result.Expect(child_count == state.peripherals.size(),
                "one rendered row exists per fixture peripheral");

  for (uint32_t i = 0; i < child_count && i < state.peripherals.size(); ++i) {
    const auto& peripheral = state.peripherals[i];
    lv_obj_t* row = lv_obj_get_child(handles.peripheral_list, static_cast<int32_t>(i));
    const char* text = lv_label_get_text(row);

    char what[192];
    std::snprintf(what, sizeof(what), "row %u contains peripheral name '%s'", i, peripheral.name.c_str());
    result.Expect(std::strstr(text, peripheral.name.c_str()) != nullptr, what);

    const char* expect_presence = peripheral.present ? "PRESENT" : "ABSENT";
    std::snprintf(what, sizeof(what), "row %u reports %s for '%s'", i, expect_presence,
                  peripheral.name.c_str());
    result.Expect(std::strstr(text, expect_presence) != nullptr, what);

    std::snprintf(what, sizeof(what), "row %u contains detail text for '%s'", i, peripheral.name.c_str());
    result.Expect(std::strstr(text, peripheral.detail.c_str()) != nullptr, what);
  }

  // F07's acceptance line: missing SD is an ordinary idle status, not a
  // failure -- confirm the fixture's absent MicroSD row still renders
  // cleanly as ABSENT rather than being omitted or flagged specially.
  bool found_microsd_absent = false;
  for (uint32_t i = 0; i < child_count; ++i) {
    lv_obj_t* row = lv_obj_get_child(handles.peripheral_list, static_cast<int32_t>(i));
    const char* text = lv_label_get_text(row);
    if (std::strstr(text, "microsd") != nullptr && std::strstr(text, "ABSENT") != nullptr) {
      found_microsd_absent = true;
    }
  }
  result.Expect(found_microsd_absent, "missing MicroSD renders as an ordinary ABSENT row");

  return result.Finish("simulator peripherals");
}

}  // namespace simulator::scenarios
