// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::domain {

// Platform-neutral classification of why the board (re)started. The ESP32
// platform layer adapts esp_reset_reason() (esp_system.h) into this enum;
// the simulator populates it with fixture values. Keep this enum's meaning
// stable across platforms so the diagnostic screen never branches on which
// platform produced it.
enum class ResetReason {
  PowerOn,
  ExternalReset,
  Watchdog,
  Brownout,
  DeepSleepWake,
  Software,
  Unknown,
};

const char* ToString(ResetReason reason);

}  // namespace firmware::domain
