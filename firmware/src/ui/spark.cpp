#include "spark.h"

#include <M5Unified.h>

#include "burst_frames.h"

namespace ui {

void Spark::create(M5Canvas& sprite) {
    sprite.setColorDepth(16);
    sprite.createSprite(SIZE, SIZE);
}

void Spark::draw_frame(M5Canvas& sprite, int frame_index, uint32_t bg_rgb888) {
    const auto bg = M5.Display.color888(
        (bg_rgb888 >> 16) & 0xff,
        (bg_rgb888 >> 8) & 0xff,
        bg_rgb888 & 0xff);
    const auto fg = M5.Display.color888(
        (burst::COLOR_RGB >> 16) & 0xff,
        (burst::COLOR_RGB >> 8) & 0xff,
        burst::COLOR_RGB & 0xff);

    sprite.fillSprite(bg);

    if (frame_index < 0) frame_index = 0;
    if (frame_index >= burst::N_FRAMES) frame_index %= burst::N_FRAMES;
    const auto& frame = burst::FRAMES[frame_index];

    for (uint16_t i = 0; i + 3 <= frame.length; i += 3) {
        uint8_t y = frame.data[i];
        uint8_t x = frame.data[i + 1];
        uint8_t w = frame.data[i + 2];
        sprite.drawFastHLine(x, y, w, fg);
    }
}

}  // namespace ui
