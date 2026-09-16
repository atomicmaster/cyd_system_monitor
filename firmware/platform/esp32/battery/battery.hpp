// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::platform::esp32::battery {

// Initializes an ADC oneshot unit/channel on the profile's battery.adc
// pin. Returns false on ADC init failure.
bool Init();

// Reads the raw battery/supply voltage in millivolts via the ADC's
// calibrated line-fitting scheme. This is diagnostic-only, per the
// profile README's stated MVP scope: no calibrated state of charge,
// battery health, or runtime estimate.
int ReadMillivolts();

}  // namespace firmware::platform::esp32::battery
