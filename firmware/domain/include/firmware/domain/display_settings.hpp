// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace firmware::domain {

// How the board presents time. Local to the board (NVS); TB16 later brings
// these under the shared configuration-revision and Host-command path.
struct DisplaySettings {
  bool use_24h = true;
  bool show_zone = false;  // "UTC-5" after the time; never hidden when the zone is unknown
  // Off: follow the Host's own local offset. On: a fixed offset chosen here.
  bool manual_zone = false;
  int16_t manual_offset_minutes = 0;  // only meaningful when manual_zone

  bool operator==(const DisplaySettings&) const = default;
};

constexpr int16_t kMinOffsetMinutes = -12 * 60;
constexpr int16_t kMaxOffsetMinutes = 14 * 60;
constexpr int16_t kOffsetStepMinutes = 15;  // Nepal is UTC+5:45

// Clamps the manual offset into the real-world range and onto the 15-minute
// grid, so a damaged or hand-edited record can never render nonsense.
DisplaySettings Sanitize(DisplaySettings settings);

// Steps the manual offset by `direction` quarter-hours, staying in range.
DisplaySettings StepOffset(DisplaySettings settings, int direction);

// Fixed-size storage form: version, flags, offset (LE), check byte.
using SettingsRecord = std::array<uint8_t, 5>;
SettingsRecord Serialize(const DisplaySettings& settings);
// Absent for a wrong version, a bad check byte, or an out-of-range offset:
// the caller then uses the defaults rather than a half-trusted record.
std::optional<DisplaySettings> Deserialize(const SettingsRecord& record);

}  // namespace firmware::domain
