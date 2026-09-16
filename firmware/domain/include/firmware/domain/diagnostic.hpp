// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "firmware/domain/reset_reason.hpp"

namespace firmware::domain {

// One row of the diagnostic screen's peripheral list. `detail` is a short
// human-readable status (e.g. "3.9V", "not present", "CH340 bridge") -- the
// platform layer decides its exact wording from real hardware; the
// simulator supplies fixture text.
struct PeripheralStatus {
  std::string name;
  bool present;
  std::string detail;
};

// Platform-neutral data the diagnostic screen renders. platform/esp32
// populates this from real hardware at boot; the simulator populates it
// with synthetic/fixture values so firmware/ui can be exercised without a
// board.
struct DiagnosticState {
  std::string profile_id;
  std::string build_identity;
  ResetReason reset_reason;
  std::vector<PeripheralStatus> peripherals;
};

// Looks up a peripheral row by name. Returns nullptr if `state` has no row
// with that name -- distinct from a row that is present==false, which means
// the peripheral was probed and found absent.
const PeripheralStatus* FindPeripheral(const DiagnosticState& state, std::string_view name);

}  // namespace firmware::domain
