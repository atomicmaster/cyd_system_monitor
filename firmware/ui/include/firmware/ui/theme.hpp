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
// Quieter than kMuted: a switched-off thing (a disabled radio), always with a
// slash across it.
inline constexpr uint32_t kDim = 0x3A4266;
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
// monochrome Codex, Grok and OpenCode marks. Gemini is its own blue (not one
// of the reserved Alert colours).
inline constexpr uint32_t kMascotCodex = 0xF2F4FA;
inline constexpr uint32_t kMascotClaude = 0xD77757;
inline constexpr uint32_t kMascotGrok = 0xE8EBF7;
inline constexpr uint32_t kMascotGemini = 0x8AB4F8;
inline constexpr uint32_t kMascotOpenCode = 0xD8DCE8;

inline lv_color_t Color(uint32_t hex) { return lv_color_hex(hex); }

// The accent of the Overview card at `index` in registry order.
lv_color_t CardAccent(size_t index);
// The accent of an agent by wire slot (codex, claude, grok, gemini, opencode).
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
// A header or sheet action: a rounded pill with a 1 px outline on a recessed
// fill, so it reads as a button on the dark sheet. `accent` is the cyan
// expected action; otherwise it is quiet (muted).
void StyleActionButton(lv_obj_t* button, bool accent);
// The shared pressed-state fill, for a tap target that is not a button.
void StylePressed(lv_obj_t* object);
// A transparent, clickable, full-screen layer on `parent`, created on top of
// everything: a sheet puts its scrim up just before itself and deletes it on
// close, so a closed sheet holds no pool memory for it.
lv_obj_t* CreateScrim(lv_obj_t* parent);
// A switch with the cyan "on" track.
void StyleSwitch(lv_obj_t* control);
// A hairline under a row.
void StyleRowRule(lv_obj_t* row);

}  // namespace firmware::ui::theme
