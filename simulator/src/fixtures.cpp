// SPDX-License-Identifier: GPL-3.0-only
#include "fixtures.hpp"

#include <string>

#include "firmware/domain/capacity.hpp"
#include "firmware/domain/generated/profile.hpp"

namespace simulator::fixtures {

firmware::domain::DiagnosticState MakeDiagnosticStateFixture() {
  using firmware::domain::PeripheralStatus;
  using firmware::domain::ResetReason;

  firmware::domain::DiagnosticState state;
  state.profile_id = firmware::domain::profile::kProfileId;
  state.build_identity = "simulator-dev-build";
  state.reset_reason = ResetReason::PowerOn;
  state.peripherals = {
      PeripheralStatus{"touch", true, "calibrated"},
      PeripheralStatus{"rgb_led", true, "off"},
      // Missing SD is an ordinary idle status per F07's acceptance line,
      // not a failure -- this fixture deliberately models that case.
      PeripheralStatus{"microsd", false, "not present (idle)"},
      PeripheralStatus{"audio", true, "enabled, alert sound off by default"},
      PeripheralStatus{"battery", true, "3.9V (diagnostic only, no state of charge)"},
      PeripheralStatus{"boot_button", true, "released"},
      PeripheralStatus{"expansion", false, "not probed"},
  };
  return state;
}

firmware::ui::capacity::CapacitySliceState MakeCapacitySliceStateFixture() {
  namespace bounds = firmware::domain::capacity;
  using firmware::ui::capacity::AlertRow;
  using firmware::ui::capacity::CapacitySliceState;
  using firmware::ui::capacity::SettingRow;

  CapacitySliceState state;
  state.live_status_text = std::string(bounds::kMaxLiveStatusTextLength + 20, 'S');

  // One row more than the declared maximum, so the scenario can confirm
  // the extra row is clamped away rather than rendered.
  for (uint32_t i = 0; i < bounds::kMaxAlertListRows + 1; ++i) {
    AlertRow row;
    row.severity = (i % 2 == 0) ? "High" : "Medium";
    row.title = std::string(bounds::kMaxAlertLabelLength + 20, 'A') + std::to_string(i);
    row.detail = std::string(bounds::kMaxAlertDetailLength + 40, 'D');
    state.alerts.push_back(std::move(row));
  }

  for (uint32_t i = 0; i < bounds::kMaxSettingsRows + 1; ++i) {
    SettingRow row;
    row.label = std::string(bounds::kMaxSettingLabelLength + 10, 'L') + std::to_string(i);
    row.value = std::string(bounds::kMaxSettingValueLength + 10, 'V');
    state.settings.push_back(std::move(row));
  }

  return state;
}

}  // namespace simulator::fixtures
