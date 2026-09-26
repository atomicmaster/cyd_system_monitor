// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lvgl.h>

#include <cstddef>

namespace firmware::ui::theme {

// The dark palette: deep navy surfaces with cool neon accents for data. Red,
// orange, amber and yellow are absent on purpose: they are reserved for Alert
// Severity (docs/ui-design.md), so a data accent never reads as an Alert.
// Colour always travels with text or shape, never alone.
inline constexpr uint32_t kScreen = 0x0B0F1E;
inline constexpr uint32_t kPanel = 0x141A2E;
inline constexpr uint32_t kPanelBorder = 0x252D4A;
inline constexpr uint32_t kTrack = 0x252D4A;
inline constexpr uint32_t kText = 0xE8EBF7;
inline constexpr uint32_t kMuted = 0x8A91B4;
inline constexpr uint32_t kCyan = 0x22D3EE;
inline constexpr uint32_t kViolet = 0xA78BFA;
inline constexpr uint32_t kBlue = 0x60A5FA;
inline constexpr uint32_t kTeal = 0x2DD4BF;
inline constexpr uint32_t kPink = 0xF472B6;
inline constexpr uint32_t kMagenta = 0xE879F9;

inline lv_color_t Color(uint32_t hex) { return lv_color_hex(hex); }

// The accent of the Overview card at `index` in registry order.
lv_color_t CardAccent(size_t index);
// The accent of an agent by wire slot (codex, claude, grok).
lv_color_t AgentAccent(size_t slot);

// A rounded panel on the navy screen: panel fill, 1 px border, no scrolling.
void StylePanel(lv_obj_t* object);
// A muted 12 pt caption (card titles, notes).
void StyleCaption(lv_obj_t* label);

}  // namespace firmware::ui::theme
