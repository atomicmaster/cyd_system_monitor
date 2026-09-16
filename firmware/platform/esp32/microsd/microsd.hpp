// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace firmware::platform::esp32::microsd {

// Result of one MicroSD presence probe.
struct ProbeResult {
  bool present = false;
  std::string detail;  // human-readable status for the diagnostic screen
};

// Probes for a MicroSD card over the shared expansion SPI bus (see
// board_init.cpp's centrally documented provisional bus arrangement) via
// esp_vfs_fat_sdspi_mount. Absence is a normal, non-fatal result -- F07's
// acceptance line treats missing SD as an ordinary idle status, not a
// failure -- so this never aborts boot; it only reports what it found.
ProbeResult Probe();

}  // namespace firmware::platform::esp32::microsd
