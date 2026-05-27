#pragma once

// Pure-synth sound effects for claude-orbit. All entry points are
// non-blocking: they enqueue tones into M5.Speaker's channel mixer and
// return immediately. The non-blocking charge-build sweep is advanced
// by repeated calls to tick() from the game's main loop — no delay()
// calls anywhere in the implementation, so SFX never stalls rendering.
//
// Channel layout (M5.Speaker.tone has 8 mixer channels, 0..7):
//   0 — player fire blips
//   1 — pod fire blips
//   2 — charge build (continuous sweep) + charge release
//   3 — hits / player-hit glitches
//   4 — explosions / boss death / bombs / death
//   5 — boss intro / boss hit thuds
//   6 — pickups / menu ticks / menu confirm / powerup spawn
//   7 — biome transition / chimes
//
// Static state stays well under 1 KB (a handful of u32s + bool).

namespace apps {
namespace orbit {
namespace sfx {

void play_fire();
void play_pod_fire();

void start_charge_build();
void cancel_charge_build();
void play_charge_release();

void play_hit();
void play_explode();
void play_player_hit();
void play_pickup();
void play_bomb();

void play_boss_intro();
void play_boss_hit();
void play_boss_death();

void play_biome_transition();
void play_level_complete();  // celebratory fanfare on biome boss kill
void play_death();

void play_menu_tick();
void play_menu_confirm();
void play_powerup_spawn();

// Call from the game's main tick(). Cheap (single timestamp check) when
// no sound is currently being advanced.
void tick();

// Hard-stop every channel + clear the in-flight charge sweep. Use this on
// game reset so the death-sound queue isn't still draining into the new run.
void stop_all();

}  // namespace sfx
}  // namespace orbit
}  // namespace apps
