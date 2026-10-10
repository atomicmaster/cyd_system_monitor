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
// The two rates of a pair: download and storage read first, upload and write
// second (Network rows, Storage slots).
inline constexpr uint32_t kRateFirst = kPink;
inline constexpr uint32_t kRateSecond = kCyan;
// The agent mascots keep their own colours: Claude Code's terracotta crab
// (the one deliberate orange outside Alerts, always with its shape) and the
// monochrome Codex and Grok marks.
inline constexpr uint32_t kMascotCodex = 0xF2F4FA;
inline constexpr uint32_t kMascotClaude = 0xD77757;
inline constexpr uint32_t kMascotGrok = 0xE8EBF7;

inline lv_color_t Color(uint32_t hex) { return lv_color_hex(hex); }

// The accent of the Overview card at `index` in registry order.
lv_color_t CardAccent(size_t index);
// The accent of an agent by wire slot (codex, claude, grok).
lv_color_t AgentAccent(size_t slot);

// A rounded panel on the navy screen: panel fill, 1 px border, no scrolling.
void StylePanel(lv_obj_t* object);
// A muted 12 pt caption (card titles, notes).
void StyleCaption(lv_obj_t* label);
// A 14 pt heading (sheet and prompt titles).
void StyleHeading(lv_obj_t* label);
// A sheet rising from the bottom edge: panel fill, rounded corners, a
// hairline top edge and a grab handle, `height` tall.
void StyleSheet(lv_obj_t* sheet, int32_t height);
// A text-only button in `color` (the Quiet style has no filled buttons).
void StyleTextButton(lv_obj_t* button, uint32_t color);
// A switch with the cyan "on" track.
void StyleSwitch(lv_obj_t* control);
// A hairline under a row.
void StyleRowRule(lv_obj_t* row);

}  // namespace firmware::ui::theme
