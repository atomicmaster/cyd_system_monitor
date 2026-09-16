// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32::expansion {

// Configures the expansion header's input-only GPIO (profile
// expansion.input_only, GPIO35) as a diagnostic input. This pin is
// unconnected on the board by default -- exercising it requires an
// operator to jumper it to GND or 3V3 externally; there is nothing to
// observe on a floating pin otherwise. GPIO35 is one of the ESP32's
// input-only pins with no internal pull-up/pull-down hardware, so a
// floating reading is not meaningful and is not treated as a pass/fail
// result.
bool Init();

// Returns true if the pin currently reads high.
bool IsHigh();

}  // namespace firmware::platform::esp32::expansion
