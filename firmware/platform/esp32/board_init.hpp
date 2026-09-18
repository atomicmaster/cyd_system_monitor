// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include "firmware/domain/diagnostic.hpp"
#include "touch/touch.hpp"

namespace firmware::platform::esp32::board {

// Everything board_init.cpp hands back to main.cpp after orchestrating all
// of firmware/platform/esp32's drivers at boot.
struct BoardHandles {
  // Null if display init failed; main.cpp must treat that as "no screen,"
  // not abort -- the rest of the board (UART diagnostics, USB recovery)
  // still needs to work without a display.
  lv_display_t* display = nullptr;

  // Touch driver, already Init()'d. main.cpp polls ReadRaw() on it to
  // drive first-boot/recalibration (see firmware::ui::calibration) --
  // board_init owns construction/Init() so it stays the single place the
  // bit-banged SPI arrangement is wired up.
  firmware::platform::esp32::touch::Xpt2046Touch touch;
  bool touch_ready = false;
};

// Orchestrates board bring-up in this fixed order:
//   1. nvs_flash_init (required before any NVS access, including
//      calibration record load).
//   2. Display (owns the display's dedicated SPI2 bus).
//   3. Touch (bit-banged software SPI; F08-confirmed under combined load,
//      see touch/touch.hpp and board_init.cpp's arrangement comment).
//   4. MicroSD probe (shares the expansion SPI3 bus with the expansion
//      header; see the "Provisional bus arrangement" comment in
//      board_init.cpp, the single documented decision point for that
//      sharing).
//   5. RGB LED, audio enable/DAC, battery ADC, BOOT button, reset reason.
//   6. The development-only UART clear-calibration console
//      (dev_console/dev_console.hpp).
//
// Populates and returns a firmware::domain::DiagnosticState alongside the
// BoardHandles the caller needs (currently just the LVGL display, if any).
// No individual peripheral failure aborts boot; each is folded into the
// DiagnosticState's peripheral list as present/absent instead, so a board
// with (for example) no SD card inserted still boots to a readable
// diagnostic screen.
firmware::domain::DiagnosticState InitBoard(BoardHandles& out_handles);

}  // namespace firmware::platform::esp32::board
