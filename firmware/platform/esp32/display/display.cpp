// SPDX-License-Identifier: Apache-2.0
#include "display/display.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "firmware/domain/generated/profile.hpp"

namespace firmware::platform::esp32::display {

namespace {

constexpr const char* kTag = "fw.display";

// The display's own dedicated SPI2 (HSPI) host. Touch and MicroSD use a
// separate, provisional bus arrangement documented centrally in
// board_init.cpp -- this module only owns the display's bus.
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;

constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_bit_t kBacklightDutyResolution = LEDC_TIMER_10_BIT;  // 0..1023

esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_panel_io_handle_t g_panel_io = nullptr;

// Full-on for diagnostics; a future dimming feature reuses this same ledc
// channel and only changes the requested duty cycle.
esp_err_t InitBacklight() {
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = kBacklightSpeedMode;
  timer_cfg.timer_num = kBacklightTimer;
  timer_cfg.duty_resolution = kBacklightDutyResolution;
  timer_cfg.freq_hz = 5000;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t err = ledc_timer_config(&timer_cfg);
  if (err != ESP_OK) return err;

  ledc_channel_config_t channel_cfg = {};
  channel_cfg.gpio_num = firmware::domain::profile::kDisplayBacklight;
  channel_cfg.speed_mode = kBacklightSpeedMode;
  channel_cfg.channel = kBacklightChannel;
  channel_cfg.timer_sel = kBacklightTimer;
  channel_cfg.duty = (1 << kBacklightDutyResolution) - 1;  // full-on
  channel_cfg.hpoint = 0;
  return ledc_channel_config(&channel_cfg);
}

// Bridges LVGL's flush request to esp_lcd_panel_draw_bitmap. This panel
// draws over SPI in polling (blocking) mode with no color-trans-done
// callback wired up, so the bitmap is fully on the wire by the time
// draw_bitmap returns and flush_ready can be called immediately.
void FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
  lv_display_flush_ready(disp);
}

}  // namespace

lv_display_t* InitDisplay() {
  spi_bus_config_t bus_cfg = {};
  bus_cfg.sclk_io_num = firmware::domain::profile::kDisplaySclk;
  bus_cfg.mosi_io_num = firmware::domain::profile::kDisplayMosi;
  bus_cfg.miso_io_num = firmware::domain::profile::kDisplayMiso;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  // One full 320x240x16bpp frame's worth of transfer headroom; LVGL flushes
  // partial areas, so this is comfortably above any single flush's size.
  bus_cfg.max_transfer_sz = firmware::domain::profile::kDisplayWidth *
                             firmware::domain::profile::kDisplayHeight * 2;
  esp_err_t err = spi_bus_initialize(kDisplaySpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return nullptr;
  }

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = firmware::domain::profile::kDisplayCs;
  io_cfg.dc_gpio_num = firmware::domain::profile::kDisplayDc;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = 40 * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  err = esp_lcd_new_panel_io_spi(reinterpret_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &io_cfg,
                                  &g_panel_io);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
    return nullptr;
  }

  esp_lcd_panel_dev_config_t panel_cfg = {};
  // display.reset is the chip's EN pin in profile.toml, not a GPIO the
  // application drives; there is no dedicated per-panel reset line to
  // assert, so this stays -1 (no reset pin) rather than a fabricated GPIO.
  panel_cfg.reset_gpio_num = -1;
  panel_cfg.color_space = ESP_LCD_COLOR_SPACE_RGB;
  panel_cfg.bits_per_pixel = 16;

  err = esp_lcd_new_panel_ili9341(g_panel_io, &panel_cfg, &g_panel);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(err));
    return nullptr;
  }

  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);
  // Landscape 320x240: the panel is physically 240 wide x 320 tall
  // (profile.toml display.width/height), so swap_xy rotates into the
  // logical landscape layout firmware/domain/geometry.hpp validates.
  esp_lcd_panel_swap_xy(g_panel, true);
  esp_lcd_panel_mirror(g_panel, false, true);
  esp_lcd_panel_disp_on_off(g_panel, true);

  if (InitBacklight() != ESP_OK) {
    ESP_LOGW(kTag, "backlight PWM init failed; panel remains initialized without backlight control");
  }

  lv_display_t* lv_disp = lv_display_create(firmware::domain::profile::kLogicalWidth,
                                             firmware::domain::profile::kLogicalHeight);
  lv_display_set_flush_cb(lv_disp, FlushCallback);
  static uint8_t draw_buf[firmware::domain::profile::kLogicalWidth * 40 * 2];  // 40-row partial buffer
  lv_display_set_buffers(lv_disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  return lv_disp;
}

}  // namespace firmware::platform::esp32::display
