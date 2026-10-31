// SPDX-License-Identifier: Apache-2.0
#include "firmware/ui/system/agents/mascots.hpp"

#include "firmware/ui/pixel_art.hpp"
#include "firmware/ui/theme.hpp"

namespace firmware::ui::system::mascots {
namespace {

using pixel_art::Pixels;

// clang-format off: one row of pixels per line.
constexpr const char* kCodexArt[] = {
    "#........",
    ".#.......",
    "..#......",
    ".#.......",
    "#...#####",
};
constexpr const char* kCodexOffArt[] = {
    "#........",
    ".#.......",
    "..#......",
    ".#.......",
    "#........",
};
// The banner crab is drawn with quadrant blocks, whose pixels are twice as
// tall as wide in a terminal: 2 across, 4 down keeps its shape.
constexpr const char* kClaudeArt[] = {
    "..############..",
    "..##.######.##..",
    "################",
    "..############..",
    "...#.#....#.#...",
};
constexpr const char* kClaudeDanceArt[] = {
    "#.############.#",
    "#.##.######.##.#",
    "..############..",
    "..############..",
    "..#.#......#.#..",
};
constexpr const char* kGrokArt[] = {
    ".......#####......##",
    ".....#########...###",
    "....##########..###.",
    "...######..##.####..",
    "...####......####...",
    "..####......#####...",
    "..###......#######..",
    "..###.....###..###..",
    "..###.....##...###..",
    "..###.........####..",
    "..###.........####..",
    "..####.......####...",
    "...###......#####...",
    "..###.##########....",
    ".###..#########.....",
    "###...########......",
    "##..................",
};
// Gemini's four-point sparkle; busy twinkles between this and a small star.
constexpr const char* kGeminiArt[] = {
    "........#........",
    "........#........",
    "........#........",
    ".......###.......",
    ".......###.......",
    "......#####......",
    ".....#######.....",
    "...###########...",
    "#################",
    "...###########...",
    ".....#######.....",
    "......#####......",
    ".......###.......",
    ".......###.......",
    "........#........",
    "........#........",
    "........#........",
};
constexpr const char* kGeminiSmallArt[] = {
    ".................",
    ".................",
    ".................",
    ".................",
    "........#........",
    "........#........",
    ".......###.......",
    "......#####......",
    "....#########....",
    "......#####......",
    ".......###.......",
    "........#........",
    "........#........",
    ".................",
    ".................",
    ".................",
    ".................",
};
// OpenCode: a rounded terminal window with a small block inside; busy blinks
// the block off.
constexpr const char* kOpenCodeArt[] = {
    ".#########.",
    "#.........#",
    "#.........#",
    "#...###...#",
    "#...###...#",
    "#...###...#",
    "#.........#",
    "#.........#",
    ".#########.",
};
constexpr const char* kOpenCodeOffArt[] = {
    ".#########.",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    ".#########.",
};
// clang-format on

constexpr auto kCodex = Pixels<9, 5, 3, 3>(kCodexArt);
constexpr auto kCodexOff = Pixels<9, 5, 3, 3>(kCodexOffArt);
constexpr auto kClaude = Pixels<16, 5, 2, 4>(kClaudeArt);
constexpr auto kClaudeDance = Pixels<16, 5, 2, 4>(kClaudeDanceArt);
constexpr auto kGrok = Pixels<20, 17, 1, 1>(kGrokArt);
constexpr auto kGemini = Pixels<17, 17, 1, 1>(kGeminiArt);
constexpr auto kGeminiSmall = Pixels<17, 17, 1, 1>(kGeminiSmallArt);
constexpr auto kOpenCode = Pixels<11, 9, 2, 2>(kOpenCodeArt);
constexpr auto kOpenCodeOff = Pixels<11, 9, 2, 2>(kOpenCodeOffArt);

constexpr auto Descriptor = pixel_art::Image;

const lv_image_dsc_t kImages[] = {
    Descriptor(kCodex.data(), 27, 15), Descriptor(kClaude.data(), 32, 20),
    Descriptor(kGrok.data(), 20, 17), Descriptor(kGemini.data(), 17, 17),
    Descriptor(kOpenCode.data(), 22, 18)};
const lv_image_dsc_t kBusyFrames[] = {
    Descriptor(kCodexOff.data(), 27, 15), Descriptor(kClaudeDance.data(), 32, 20),
    Descriptor(kGrok.data(), 20, 17), Descriptor(kGeminiSmall.data(), 17, 17),
    Descriptor(kOpenCodeOff.data(), 22, 18)};
constexpr uint32_t kColors[] = {theme::kMascotCodex, theme::kMascotClaude, theme::kMascotGrok,
                                theme::kMascotGemini, theme::kMascotOpenCode};
constexpr size_t kSlots = sizeof kColors / sizeof kColors[0];
static_assert(sizeof kImages / sizeof kImages[0] == kSlots);
static_assert(sizeof kBusyFrames / sizeof kBusyFrames[0] == kSlots);

}  // namespace

const lv_image_dsc_t& Image(size_t slot) { return kImages[slot % kSlots]; }
const lv_image_dsc_t& BusyFrame(size_t slot) { return kBusyFrames[slot % kSlots]; }
uint32_t Color(size_t slot) { return kColors[slot % kSlots]; }

}  // namespace firmware::ui::system::mascots
