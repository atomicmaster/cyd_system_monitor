// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "firmware/domain/display_settings.hpp"

using namespace firmware::domain;

namespace {
int g_failures = 0;
#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
      ++g_failures;                                       \
    }                                                     \
  } while (0)
}  // namespace

int main() {
  {  // Defaults: 24-hour, follow the Host.
    const DisplaySettings s;
    CHECK(s.use_24h && !s.manual_zone && s.manual_offset_minutes == 0);
  }
  {  // A record from before the "show timezone" flag was retired still loads.
    SettingsRecord old = Serialize({.manual_zone = true, .manual_offset_minutes = 60});
    old[1] |= 2;
    old[4] = static_cast<uint8_t>(0xA5 ^ old[0] ^ old[1] ^ old[2] ^ old[3]);
    const auto back = Deserialize(old);
    CHECK(back.has_value() && back->manual_zone && back->manual_offset_minutes == 60);
    CHECK(Serialize(*back)[1] == old[1] - 2);
  }
  {  // Every combination survives storage.
    for (int bits = 0; bits < 4; ++bits) {
      for (int offset : {-720, -570, -15, 0, 15, 345, 840}) {
        DisplaySettings s{.use_24h = (bits & 1) != 0,
                          .manual_zone = (bits & 2) != 0,
                          .manual_offset_minutes = static_cast<int16_t>(offset)};
        const auto back = Deserialize(Serialize(s));
        CHECK(back.has_value() && *back == s);
      }
    }
  }
  {  // Damaged records fall back to the defaults instead of being half-trusted.
    const SettingsRecord good = Serialize({.use_24h = false});
    CHECK(Deserialize(good).has_value());
    for (size_t i = 0; i < good.size(); ++i) {
      SettingsRecord bad = good;
      bad[i] ^= 0x10;
      CHECK(!Deserialize(bad).has_value());
    }
    SettingsRecord wrong_version = good;
    wrong_version[0] = 2;
    wrong_version[4] = static_cast<uint8_t>(0xA5 ^ wrong_version[0] ^ wrong_version[1] ^
                                            wrong_version[2] ^ wrong_version[3]);
    CHECK(!Deserialize(wrong_version).has_value());
    // Valid check byte but an offset off the grid or out of range.
    SettingsRecord off_grid = good;
    off_grid[2] = 7;
    off_grid[3] = 0;
    off_grid[4] =
        static_cast<uint8_t>(0xA5 ^ off_grid[0] ^ off_grid[1] ^ off_grid[2] ^ off_grid[3]);
    CHECK(!Deserialize(off_grid).has_value());
    SettingsRecord too_far = good;
    too_far[2] = 0xD0;  // 2000 minutes
    too_far[3] = 0x07;
    too_far[4] = static_cast<uint8_t>(0xA5 ^ too_far[0] ^ too_far[1] ^ too_far[2] ^ too_far[3]);
    CHECK(!Deserialize(too_far).has_value());
  }
  {  // Stepping stays on the grid and inside the real-world range.
    DisplaySettings s{.manual_zone = true};
    s = StepOffset(s, +1);
    CHECK(s.manual_offset_minutes == 15);
    s = StepOffset(s, -1);
    s = StepOffset(s, -1);
    CHECK(s.manual_offset_minutes == -15);
    for (int i = 0; i < 200; ++i) s = StepOffset(s, +1);
    CHECK(s.manual_offset_minutes == kMaxOffsetMinutes);
    for (int i = 0; i < 400; ++i) s = StepOffset(s, -1);
    CHECK(s.manual_offset_minutes == kMinOffsetMinutes);
    CHECK(Sanitize({.manual_offset_minutes = 5000}).manual_offset_minutes == kMaxOffsetMinutes);
    CHECK(Sanitize({.manual_offset_minutes = 20}).manual_offset_minutes == 15);
  }
  if (g_failures != 0) {
    std::printf("firmware_domain_display_settings_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("firmware_domain_display_settings_tests: ok");
  return 0;
}
