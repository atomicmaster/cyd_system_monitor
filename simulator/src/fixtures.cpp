// SPDX-License-Identifier: GPL-3.0-only
#include "fixtures.hpp"

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

}  // namespace simulator::fixtures
