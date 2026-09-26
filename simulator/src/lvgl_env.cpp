// SPDX-License-Identifier: GPL-3.0-only
#include "lvgl_env.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace simulator {

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
// LV_COLOR_DEPTH is 16 (firmware/ui/lv_conf.h); one full-screen buffer is
// enough here since nothing partially redraws in a headless scenario run.
uint8_t g_draw_buf[kWidth * kHeight * 2];

// The whole screen as the panel would show it, kept for SaveScreenshot.
uint16_t g_frame[kWidth * kHeight];

lv_display_t* g_display = nullptr;

void FlushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  // No real window to present to: copy the area into the frame and move on.
  const int32_t width = lv_area_get_width(area);
  for (int32_t y = area->y1; y <= area->y2; ++y) {
    std::memcpy(&g_frame[y * kWidth + area->x1], pixels + (y - area->y1) * width * 2, width * 2);
  }
  lv_display_flush_ready(display);
}

void PutLe(std::ofstream& out, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
}

}  // namespace

lv_obj_t* InitLvglHeadless() {
  lv_init();
  g_display = lv_display_create(kWidth, kHeight);
  lv_display_set_flush_cb(g_display, FlushCallback);
  lv_display_set_buffers(g_display, g_draw_buf, nullptr, sizeof(g_draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  return lv_display_get_screen_active(g_display);
}

void RenderOnce() { lv_refr_now(g_display); }

bool SaveScreenshot(const std::string& path, int scale) {
  // Force a full redraw so the frame holds every pixel, not just the last
  // invalidated areas.
  lv_obj_invalidate(lv_display_get_screen_active(g_display));
  lv_refr_now(g_display);
  const uint32_t width = kWidth * scale;
  const uint32_t height = kHeight * scale;
  const uint32_t row_bytes = (width * 3 + 3) & ~3u;
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  // 24-bit BMP: 14-byte file header, 40-byte info header, bottom-up rows.
  out.put('B');
  out.put('M');
  PutLe(out, 54 + row_bytes * height, 4);
  PutLe(out, 0, 4);
  PutLe(out, 54, 4);
  PutLe(out, 40, 4);
  PutLe(out, width, 4);
  PutLe(out, height, 4);
  PutLe(out, 1, 2);
  PutLe(out, 24, 2);
  PutLe(out, 0, 4);
  PutLe(out, row_bytes * height, 4);
  PutLe(out, 2835, 4);
  PutLe(out, 2835, 4);
  PutLe(out, 0, 4);
  PutLe(out, 0, 4);
  std::string row(row_bytes, '\0');
  for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0; --y) {
    for (uint32_t x = 0; x < width; ++x) {
      // LV_COLOR_16_SWAP: the flushed pixels are big-endian, as the panel wants.
      const uint16_t raw = g_frame[(y / scale) * kWidth + x / scale];
      const uint16_t pixel = static_cast<uint16_t>((raw >> 8) | (raw << 8));
      const uint8_t r = (pixel >> 11) & 0x1F;
      const uint8_t g = (pixel >> 5) & 0x3F;
      const uint8_t b = pixel & 0x1F;
      row[x * 3] = static_cast<char>((b << 3) | (b >> 2));
      row[x * 3 + 1] = static_cast<char>((g << 2) | (g >> 4));
      row[x * 3 + 2] = static_cast<char>((r << 3) | (r >> 2));
    }
    out.write(row.data(), row.size());
  }
  return static_cast<bool>(out);
}

}  // namespace simulator
