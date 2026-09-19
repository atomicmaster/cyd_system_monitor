// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/display_settings.hpp"

namespace firmware::platform::esp32::nvs {

// Clock presentation settings persisted in NVS. A missing, torn, or damaged
// record loads as the defaults (24-hour, zone hidden, follow the Mac); it is
// never half-trusted (see firmware::domain::Deserialize).
firmware::domain::DisplaySettings LoadDisplaySettings();

// Returns false on any NVS error; the caller keeps using the in-memory value.
bool SaveDisplaySettings(const firmware::domain::DisplaySettings& settings);

}  // namespace firmware::platform::esp32::nvs
