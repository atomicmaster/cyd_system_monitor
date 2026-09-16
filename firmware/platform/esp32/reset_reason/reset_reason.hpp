// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/reset_reason.hpp"

namespace firmware::platform::esp32::reset_reason {

// Adapts esp_reset_reason() (esp_system.h) to
// firmware::domain::ResetReason, so application/UI code depends only on
// the platform-neutral enum.
firmware::domain::ResetReason ReadResetReason();

}  // namespace firmware::platform::esp32::reset_reason
