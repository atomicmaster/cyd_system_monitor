// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace firmware::platform::esp32::led {

// Initializes the RGB status LED's three ledc PWM channels (profile
// rgb_led.red/green/blue), respecting rgb_led.active_low. Returns false if
// any channel fails to configure.
bool Init();

// Sets each channel's brightness 0..255. Duty inversion for the
// active-low wiring is handled internally so callers always think in
// "0 = off, 255 = full brightness" terms regardless of polarity.
void SetColor(uint8_t red, uint8_t green, uint8_t blue);

}  // namespace firmware::platform::esp32::led
