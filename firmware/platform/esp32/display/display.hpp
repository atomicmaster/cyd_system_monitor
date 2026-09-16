// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

namespace firmware::platform::esp32::display {

// Initializes the ILI9341V panel over the profile's SPI pins
// (cs/dc/sclk/mosi/miso), sets landscape orientation via
// esp_lcd_panel_swap_xy/mirror, drives the backlight pin full-on through
// ledc PWM (dimming is future scope; the PWM channel is already in place
// for it), and registers an lv_display_t whose flush callback bridges to
// esp_lcd_panel_draw_bitmap. Returns nullptr if panel or SPI bus init
// fails; callers must not treat a null display as fatal on its own --
// board_init.cpp folds this into the diagnostic peripheral list instead of
// aborting boot.
lv_display_t* InitDisplay();

}  // namespace firmware::platform::esp32::display
