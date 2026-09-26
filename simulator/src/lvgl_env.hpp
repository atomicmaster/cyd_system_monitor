// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <lvgl.h>

#include <string>

namespace simulator {

// Initializes LVGL headless: software renderer, one in-memory draw buffer,
// and a flush callback that only marks the buffer rendered (there is no
// real window -- this must run in CI and in sandboxes with no display).
// Call once per process; returns the active screen object screens build
// under.
lv_obj_t* InitLvglHeadless();

// Forces one render pass (so the flush callback runs) and returns.
void RenderOnce();

// Redraws the whole screen and writes it to `path` as a 24-bit BMP, each
// pixel enlarged to scale x scale. For looking at layouts; scenarios never
// compare images.
bool SaveScreenshot(const std::string& path, int scale = 1);

}  // namespace simulator
