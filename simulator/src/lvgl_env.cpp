// SPDX-License-Identifier: GPL-3.0-only
#include "lvgl_env.hpp"

namespace simulator {

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
// LV_COLOR_DEPTH is 16 (firmware/ui/lv_conf.h); one full-screen buffer is
// enough here since nothing partially redraws in a headless scenario run.
uint8_t g_draw_buf[kWidth * kHeight * 2];

lv_display_t* g_display = nullptr;

void FlushCallback(lv_display_t* display, const lv_area_t* /*area*/, uint8_t* /*pixels*/) {
  // No real window to present to: mark the buffer as rendered and move on.
  lv_display_flush_ready(display);
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

}  // namespace simulator
