#pragma once

// orbit-fighter sprite atlas
// ----------------------------------------------------------------------------
// Tiny palette-indexed pixel-art atlas for the R-Type sub-app. All bitmap
// data lives in flash (static constexpr arrays). Each byte in a bitmap is a
// palette index into the table defined in orbit_sprites.cpp:
//
//   0  transparent (pixel not drawn)
//   1  cream / DARK     (0xF0EEE6)
//   2  orange           (0xCC785C)
//   3  blue / cyan      (0x00FFFF)
//   4  green            (0x00FF00)
//   5  yellow           (0xFFD43B)
//   6  mid-gray         (0x777777)
//   7  light-gray       (0x333333)
//   8  coral / danger   (0x9D4534)  — darker orange for enemy fire / errors
//
// Top-left anchored. Render with `draw_sprite(x, y, SpriteId::X)`.
// Width/height are queryable via sprite_w / sprite_h so the caller can do
// collision boxes and erase-rects without hard-coding sizes.

#include <stdint.h>

namespace apps {
namespace orbit {

enum class SpriteId : uint8_t {
    // Player ship (16x8)
    PLAYER = 0,
    PLAYER_CHARGE_L1,
    PLAYER_CHARGE_L2,
    PLAYER_CHARGE_L3,

    // Enemies (16x16 unless noted)
    BRACE_SWARM,
    DRONE_404,
    DRONE_500,
    LAMBDA,
    NULL_STALKER,
    TAG_SCOUT,
    FOR_LOOP,
    MISSILE,            // 8x4

    // Pods (8x8)
    POD_LASER,
    POD_SPREAD,
    POD_PIERCE,

    // Bullets
    BULLET_PLAYER,      // 4x2
    BULLET_CHARGED,     // 8x4
    BULLET_ENEMY,       // 4x2
    BULLET_BOSS,        // 6x4

    // Power-ups (12x12)
    POWERUP_HP,
    POWERUP_CLEAR,
    POWERUP_CACHE,
    POWERUP_TOKENS,
    POWERUP_SUBAGENT,
    POWERUP_SKIP_PERMS,

    // Bosses
    BOSS_REGEX_TYRANT,        // 48x40
    BOSS_CONTEXT_OVERFLOW,    // 64x56
    BOSS_HALLUCINATION,       // 48x16 (three 16x16 ghost outlines in a triangle)
    BOSS_RATE_LIMITER,        // 48x40
    BOSS_ROGUE_TOOL_CALL,     // 48x40

    SPRITE_COUNT
};

// Draw sprite at (x, y), top-left anchored. Skips palette-index-0 pixels so
// non-rectangular silhouettes look right against any background.
void draw_sprite(int16_t x, int16_t y, SpriteId id);

int sprite_w(SpriteId id);
int sprite_h(SpriteId id);

}  // namespace orbit
}  // namespace apps
