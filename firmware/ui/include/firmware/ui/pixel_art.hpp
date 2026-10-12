// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <array>
#include <cstdint>

namespace firmware::ui::pixel_art {

// Pixel art at native size, '#' opaque, expanded at compile time to A8 with
// `SX` x `SY` pixels per art pixel, for an image drawn through image recolor.
template <int W, int H, int SX, int SY>
constexpr std::array<uint8_t, W * SX * H * SY> Pixels(const char* const (&rows)[H]) {
  std::array<uint8_t, W * SX * H * SY> out{};
  for (int y = 0; y < H * SY; ++y) {
    for (int x = 0; x < W * SX; ++x) out[y * W * SX + x] = rows[y / SY][x / SX] == '#' ? 255 : 0;
  }
  return out;
}

// An A8 image descriptor over `data` (`w` x `h`, one byte per pixel).
inline lv_image_dsc_t Image(const uint8_t* data, int32_t w, int32_t h) {
  lv_image_dsc_t dsc{};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_A8;
  dsc.header.w = w;
  dsc.header.h = h;
  dsc.header.stride = w;
  dsc.data_size = static_cast<uint32_t>(w * h);
  dsc.data = data;
  return dsc;
}

}  // namespace firmware::ui::pixel_art
