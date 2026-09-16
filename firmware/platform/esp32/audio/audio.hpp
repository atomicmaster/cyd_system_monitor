// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace firmware::platform::esp32::audio {

// Configures the amplifier enable GPIO (profile audio.enable, respecting
// audio.enable_active_low) and the DAC channel on audio.dac. Leaves the
// amplifier disabled and silent: audio.alert_sound_default is off per the
// profile, and this driver does not override that default.
bool Init();

// Enables or disables the amplifier. Optional-sound default stays off
// (profile audio.alert_sound_default); callers opt in deliberately.
void SetAmplifierEnabled(bool enabled);

// Emits a simple square-wave tone on the DAC channel for `duration_ms`,
// blocking the calling task. Intended for deliberate diagnostic/alert use,
// not continuous playback.
void PlayTestTone(uint32_t frequency_hz, uint32_t duration_ms);

}  // namespace firmware::platform::esp32::audio
