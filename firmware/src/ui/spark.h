#pragma once

#include <M5GFX.h>

namespace ui {

// Renders the Anthropic burst animation — the same 16-frame loop used by the
// MicroPython Claude Buddy bundle. Caller owns a fixed-size sprite created
// via Spark::create(); each tick, pick a frame index and call draw_frame().
class Spark {
public:
    static constexpr int SIZE = 72;

    static void create(M5Canvas& sprite);
    static void draw_frame(M5Canvas& sprite, int frame_index, uint32_t bg_rgb888);
};

}  // namespace ui
