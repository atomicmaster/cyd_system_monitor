// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>

namespace firmware::ui::system::mascots {

// Each agent's mascot as an A8 (alpha-only) image, drawn in a colour through
// image recolor, by wire slot (codex, claude, grok):
//   codex   Codex's ">_" prompt (Codex has no mascot);
//   claude  Claude Code's banner crab;
//   grok    the Grok CLI's mark (braille art in its binary), strokes doubled.
const lv_image_dsc_t& Image(size_t slot);
// The second frame of the busy animation: Codex's cursor off, the crab
// dancing (claws up, legs apart). Grok rocks instead, so its frame is the same.
const lv_image_dsc_t& BusyFrame(size_t slot);
// The mascot's own colour (theme::kMascot*).
uint32_t Color(size_t slot);

}  // namespace firmware::ui::system::mascots
