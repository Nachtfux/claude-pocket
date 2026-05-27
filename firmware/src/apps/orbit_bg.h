#pragma once

#include <stdint.h>

namespace apps {
namespace orbit {
namespace bg {

// Biome IDs (palette + density tweaks):
//   0 STDOUT   — calm, sparse mid-gray
//   1 STDIN    — denser bracket silhouettes (layer 2)
//   2 STAGING  — green-dominant debris (layer 3), optimistic
//   3 SANDBOX  — chaotic angled layer-2 shapes, irregular timing
//   4 PROD     — high density, dim ORANGE / CORAL accents, tense

// Initialize background state for the given biome. Seeds layer positions,
// resets the particle pool, and fills the playfield rect with IVORY.
void enter(int biome);

// Advance scroll positions for all three parallax layers and redraw them
// inside the playfield rect (x=2..238, y=30..121). The game loop should
// call this once per frame BEFORE drawing the player/enemies/bullets.
void tick();

// Erase the playfield rect to IVORY without touching layer state. Useful
// when the game wants a clean slate (stage transition, death flash).
void clear_field();

// Switch palette/style without resetting layer positions or particles.
void set_biome(int biome);

// --- Particle FX --------------------------------------------------------
//
// Spawn 6-12 short-lived sparks radiating from (x, y). `kind` selects the
// palette:
//   0 enemy_pop           → ORANGE + YELLOW
//   1 boss_hit            → BLUE + ORANGE
//   2 charged_shot_impact → YELLOW + DARK (brightest)
void fx_spawn_explosion(int16_t x, int16_t y, int kind);

// Spawn 3-4 small fast ORANGE sparkles (power-up pickups).
void fx_spawn_sparkle(int16_t x, int16_t y);

// Advance + render all live particles. Erases previous-frame positions so
// no streaking. Particles auto-expire after ~200-400 ms.
void fx_tick();

}  // namespace bg
}  // namespace orbit
}  // namespace apps
