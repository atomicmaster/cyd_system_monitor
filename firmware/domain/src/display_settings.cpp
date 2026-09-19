// SPDX-License-Identifier: Apache-2.0
#include "firmware/domain/display_settings.hpp"

#include <algorithm>

namespace firmware::domain {
namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kUse24h = 1;
constexpr uint8_t kShowZone = 2;
constexpr uint8_t kManualZone = 4;

uint8_t Check(const SettingsRecord& r) {
  return static_cast<uint8_t>(0xA5 ^ r[0] ^ r[1] ^ r[2] ^ r[3]);
}

}  // namespace

DisplaySettings Sanitize(DisplaySettings settings) {
  const int clamped =
      std::clamp<int>(settings.manual_offset_minutes, kMinOffsetMinutes, kMaxOffsetMinutes);
  // Round toward zero onto the grid.
  settings.manual_offset_minutes =
      static_cast<int16_t>(clamped / kOffsetStepMinutes * kOffsetStepMinutes);
  return settings;
}

DisplaySettings StepOffset(DisplaySettings settings, int direction) {
  settings.manual_offset_minutes = static_cast<int16_t>(
      settings.manual_offset_minutes + (direction < 0 ? -kOffsetStepMinutes : kOffsetStepMinutes));
  return Sanitize(settings);
}

SettingsRecord Serialize(const DisplaySettings& settings) {
  const DisplaySettings s = Sanitize(settings);
  SettingsRecord r{};
  r[0] = kVersion;
  r[1] = static_cast<uint8_t>((s.use_24h ? kUse24h : 0) | (s.show_zone ? kShowZone : 0) |
                              (s.manual_zone ? kManualZone : 0));
  const uint16_t offset = static_cast<uint16_t>(s.manual_offset_minutes);
  r[2] = static_cast<uint8_t>(offset & 0xff);
  r[3] = static_cast<uint8_t>(offset >> 8);
  r[4] = Check(r);
  return r;
}

std::optional<DisplaySettings> Deserialize(const SettingsRecord& record) {
  if (record[0] != kVersion || record[4] != Check(record)) return std::nullopt;
  if ((record[1] & ~(kUse24h | kShowZone | kManualZone)) != 0) return std::nullopt;
  const int16_t offset = static_cast<int16_t>(record[2] | (record[3] << 8));
  if (offset < kMinOffsetMinutes || offset > kMaxOffsetMinutes || offset % kOffsetStepMinutes != 0)
    return std::nullopt;
  DisplaySettings s;
  s.use_24h = (record[1] & kUse24h) != 0;
  s.show_zone = (record[1] & kShowZone) != 0;
  s.manual_zone = (record[1] & kManualZone) != 0;
  s.manual_offset_minutes = offset;
  return s;
}

}  // namespace firmware::domain
