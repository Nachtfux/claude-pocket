#pragma once

#include <stdint.h>

namespace theme {

// Palette ported 1:1 from buddy/device/buddy_ui_cp.py — same colors the
// MicroPython Claude Buddy bundle uses, so Pocket feels visually continuous
// with it. Semantic names keep IVORY = "background" and DARK = "primary
// text" for call-site stability; values are the buddy DARK / CREAM.
constexpr uint32_t IVORY      = 0x1F1F1F;  // buddy DARK     — background
constexpr uint32_t DARK       = 0xF0EEE6;  // buddy CREAM    — primary text
constexpr uint32_t ORANGE     = 0xCC785C;  // buddy ORANGE   — accent + hairline
constexpr uint32_t BLUE       = 0x00FFFF;  // buddy CYAN     — info / listening toast
constexpr uint32_t GREEN      = 0x00FF00;  // buddy GREEN    — approve / success
constexpr uint32_t MID_GRAY   = 0x777777;  // buddy GRAY_MID — secondary text
constexpr uint32_t LIGHT_GRAY = 0x333333;  // buddy GRAY_DIM — selected-row backdrop
constexpr uint32_t YELLOW     = 0xFFD43B;  // transient value-changed indicator

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

constexpr int STATUS_BAR_H = 14;
constexpr int CONTENT_TOP = STATUS_BAR_H;
constexpr int CONTENT_H = SCREEN_H - STATUS_BAR_H;

// One spot for typography choices so the rest of the code stays font-agnostic.
// Sizes are M5GFX textSize() multipliers on the chosen font.
constexpr int FONT_BODY_SIZE = 1;
constexpr int FONT_HEADING_SIZE = 2;

}  // namespace theme
