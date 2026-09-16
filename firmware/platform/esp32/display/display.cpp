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

// Bridges LVGL's flush request to esp_lcd_panel_draw_bitmap. The SPI panel
// IO queues the pixel-data transaction asynchronously (io_cfg's
// trans_queue_depth > 1 -- esp_lcd's SPI backend uses
// spi_device_queue_trans, not a blocking polling transmit, for color
// data), so draw_bitmap returns as soon as the transaction is queued, well
// before the bytes are actually on the wire. Calling lv_display_flush_ready
// here unconditionally would tell LVGL the single (non-double-buffered)
// draw buffer is free while the DMA is still reading the old contents out
// of it, racing the next partial render's overwrite against the in-flight
// transfer -- this produced visibly sheared/garbled text on the real
// board. flush_ready is instead called from OnColorTransDone, the SPI
// driver's actual completion callback, registered below.
void FlushCallback(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

bool OnColorTransDone(esp_lcd_panel_io_handle_t /*io*/, esp_lcd_panel_io_event_data_t* /*edata*/,
                      void* user_ctx) {
  lv_display_flush_ready(static_cast<lv_display_t*>(user_ctx));
  return false;  // no higher-priority task woken by this ISR-context callback
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
  bus_cfg.max_transfer_sz =
      firmware::domain::profile::kDisplayWidth * firmware::domain::profile::kDisplayHeight * 2;
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
  // esp_lcd_spi_bus_handle_t is `typedef int`, not a pointer, so this is a
  // plain enum-to-int conversion (static_cast), not a bit-reinterpretation.
  err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &io_cfg,
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
  // This exact ILI9341 module wires its subpixels BGR, not RGB: a solid
  // red fill rendered as solid blue on the real board is the signature of
  // an R/B channel-order mismatch (distinct from the RGB565 byte-
  // endianness LV_COLOR_16_SWAP fixes above -- a byte swap can't produce a
  // clean primary-color swap like this, since R/G/B don't align to byte
  // boundaries in RGB565). Black text and near-white backgrounds are
  // invariant to an R/B swap, which is why this was invisible until a
  // saturated color (the calibration target marker) was rendered.
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
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
  // esp_lcd_ili9341 maps mirror_x/mirror_y directly to the MADCTL MX/MY
  // bits (independent of swap_xy/MV) -- see esp_lcd_ili9341.c's
  // panel_ili9341_mirror(). mirror(true, true) is the combination verified
  // correct (non-mirrored, USB connector on the operator's preferred side)
  // on the physical E32R28T; mirror(false, true) and mirror(true, false)
  // (MX XOR MY) were both observed mirrored/backwards. mirror(false, false)
  // is this orientation's 180-degree-rotated counterpart (also
  // non-mirrored) -- see docs/roadmap.md's V1 Settings-configurable flip,
  // which will toggle between this pair.
  esp_lcd_panel_swap_xy(g_panel, true);
  esp_lcd_panel_mirror(g_panel, true, true);
  esp_lcd_panel_disp_on_off(g_panel, true);

  if (InitBacklight() != ESP_OK) {
    ESP_LOGW(kTag,
             "backlight PWM init failed; panel remains initialized without backlight control");
  }

  lv_display_t* lv_disp = lv_display_create(firmware::domain::profile::kLogicalWidth,
                                            firmware::domain::profile::kLogicalHeight);
  lv_display_set_flush_cb(lv_disp, FlushCallback);
  static uint8_t
      draw_buf[firmware::domain::profile::kLogicalWidth * 40 * 2];  // 40-row partial buffer
  lv_display_set_buffers(lv_disp, draw_buf, nullptr, sizeof(draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  esp_lcd_panel_io_callbacks_t io_cbs = {};
  io_cbs.on_color_trans_done = OnColorTransDone;
  err = esp_lcd_panel_io_register_event_callbacks(g_panel_io, &io_cbs, lv_disp);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_lcd_panel_io_register_event_callbacks failed: %s", esp_err_to_name(err));
    return nullptr;
  }

  return lv_disp;
}

}  // namespace firmware::platform::esp32::display
