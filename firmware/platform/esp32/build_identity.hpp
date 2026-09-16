// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32 {

// Not a measured build-provenance string yet; C01/TB02 own a real build
// identity format. This placeholder just distinguishes an ESP32 image from
// the simulator's fixture "simulator-dev-build" string. Shared by
// board_init.cpp's DiagnosticState and dev_console.cpp's
// DEV:CAPACITY_STATUS so both report the same identity for one flashed
// image.
inline constexpr const char* kBuildIdentity = "esp32-dev-build";

}  // namespace firmware::platform::esp32
