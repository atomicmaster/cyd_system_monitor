// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32::button {

// Configures the BOOT/download button (profile buttons.download, GPIO0)
// as a diagnostic input -- it is an input-only strap pin at boot, so this
// driver only ever reads it, never drives it.
bool Init();

// Returns true if BOOT is currently held (active-low, per ESP32's strap
// wiring: pressed reads 0).
bool IsPressed();

}  // namespace firmware::platform::esp32::button
