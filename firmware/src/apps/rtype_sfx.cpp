#include "rtype_sfx.h"

#include <M5Unified.h>
#include <stdlib.h>

namespace apps {
namespace rtype {
namespace sfx {

namespace {

// Channel allocation — see header for the full table.
constexpr uint8_t CH_FIRE        = 0;
constexpr uint8_t CH_POD         = 1;
constexpr uint8_t CH_CHARGE      = 2;
constexpr uint8_t CH_HIT         = 3;
constexpr uint8_t CH_EXPLODE     = 4;
constexpr uint8_t CH_BOSS        = 5;
constexpr uint8_t CH_PICKUP      = 6;
constexpr uint8_t CH_BIOME       = 7;

// ---- Charge-build state machine -----------------------------------------
//
// start_charge_build() captures the start time and arms the state machine.
// Each tick() call computes the elapsed time, maps it to a frequency in the
// 200 → 1500 Hz ramp over 1500 ms, and (every CHARGE_STEP_MS) emits a short
// tone on CH_CHARGE so the speaker mixer hears a continuous rising whine
// rather than one long tone (M5.Speaker.tone is fire-and-forget per call).
//
// The "advance" cadence is the only thing tick() touches; when
// g_charge_active is false the function returns after a single bool +
// timestamp compare, which is what makes tick() cheap on the hot path.

constexpr uint32_t CHARGE_DURATION_MS = 1500;
constexpr uint32_t CHARGE_STEP_MS     = 30;   // emit a fresh tone every 30 ms
constexpr uint32_t CHARGE_TONE_MS     = 45;   // small overlap → continuous feel
constexpr float    CHARGE_F0          = 200.0f;
constexpr float    CHARGE_F1          = 1500.0f;

bool     g_charge_active   = false;
uint32_t g_charge_start_ms = 0;
uint32_t g_charge_last_emit_ms = 0;

// Small helper — M5.Speaker.tone takes (freq_hz, duration_ms, channel).
inline void t(float freq_hz, uint32_t dur_ms, uint8_t ch) {
    M5.Speaker.tone(freq_hz, dur_ms, ch);
}

// Quick pseudo-noise burst: a tight sequence of randomized low-freq tones
// queued on `ch`. Each tone is `slice_ms` long; `total_ms` controls the
// burst length. The speaker mixer queues these per-channel, so the call
// itself returns immediately.
void noise_burst(uint32_t total_ms, uint32_t slice_ms,
                 uint16_t fmin, uint16_t fmax, uint8_t ch) {
    if (slice_ms == 0) slice_ms = 5;
    uint32_t n = total_ms / slice_ms;
    if (n == 0) n = 1;
    if (n > 32) n = 32;  // cap queue depth — keeps the mixer responsive
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t f = (uint16_t)(fmin + (uint32_t)random((int)(fmax - fmin)));
        t((float)f, slice_ms, ch);
    }
}

}  // namespace

// ---- Player + pod fire --------------------------------------------------

void play_fire() {
    // ~50 ms blip, 1200 → 800 Hz sweep in 5 steps × 10 ms.
    t(1200.0f, 10, CH_FIRE);
    t(1100.0f, 10, CH_FIRE);
    t(1000.0f, 10, CH_FIRE);
    t( 900.0f, 10, CH_FIRE);
    t( 800.0f, 10, CH_FIRE);
}

void play_pod_fire() {
    // Softer single 900 Hz blip, 30 ms.
    t(900.0f, 30, CH_POD);
}

// ---- Non-blocking charge build ------------------------------------------

void start_charge_build() {
    g_charge_active       = true;
    g_charge_start_ms     = millis();
    g_charge_last_emit_ms = 0;  // force emission on the very next tick()
}

void cancel_charge_build() {
    g_charge_active = false;
    M5.Speaker.stop(CH_CHARGE);
}

void play_charge_release() {
    // The release cancels the build implicitly.
    g_charge_active = false;
    M5.Speaker.stop(CH_CHARGE);

    // Descending punch: 400 → 100 Hz across 60 ms (4 steps × 15 ms).
    t(400.0f, 15, CH_CHARGE);
    t(300.0f, 15, CH_CHARGE);
    t(200.0f, 15, CH_CHARGE);
    t(100.0f, 15, CH_CHARGE);
    // ~20 ms noise tail to round out the 80 ms total.
    noise_burst(20, 5, 80, 220, CH_CHARGE);
}

// ---- Hits / explosions / glitches --------------------------------------

void play_hit() {
    // Short noise burst (~15 ms) + a 2 kHz ping (~15 ms).
    noise_burst(15, 5, 80, 200, CH_HIT);
    t(2000.0f, 15, CH_HIT);
}

void play_explode() {
    // 3-tone descending burst with decay. Each step lengthens slightly to
    // suggest decay; total ~200 ms.
    t(800.0f, 50, CH_EXPLODE);
    t(400.0f, 70, CH_EXPLODE);
    t(150.0f, 80, CH_EXPLODE);
}

void play_player_hit() {
    // Descending glitch: random freq jumps falling from 1200 → 200 Hz over
    // ~100 ms. 10 slices × 10 ms; each slice's freq is randomized around a
    // linearly descending centre.
    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        int centre = 1200 - (1000 * i) / (N - 1);  // 1200 .. 200
        int jitter = (int)random(-150, 150);
        int f = centre + jitter;
        if (f < 80) f = 80;
        t((float)f, 10, CH_HIT);
    }
}

void play_pickup() {
    // Rising arpeggio C4-E4-G4, each 50 ms.
    t(262.0f, 50, CH_PICKUP);
    t(330.0f, 50, CH_PICKUP);
    t(392.0f, 50, CH_PICKUP);
}

void play_bomb() {
    // Bass drop 200 → 40 Hz over ~250 ms (5 × 50 ms) layered with a
    // ~250 ms screen-wash white-noise tail. Two channels keep the drop
    // and the wash from cutting each other off.
    t(200.0f, 50, CH_EXPLODE);
    t(150.0f, 50, CH_EXPLODE);
    t(110.0f, 50, CH_EXPLODE);
    t( 75.0f, 50, CH_EXPLODE);
    t( 40.0f, 50, CH_EXPLODE);
    noise_burst(250, 10, 60, 300, CH_HIT);
}

// ---- Boss --------------------------------------------------------------

void play_boss_intro() {
    // Dramatic descending tones, 200 ms each.
    t(200.0f, 200, CH_BOSS);
    t(150.0f, 200, CH_BOSS);
    t(110.0f, 200, CH_BOSS);
    t( 82.0f, 200, CH_BOSS);
}

void play_boss_hit() {
    // Thicker bass thud — 80 Hz for ~60 ms.
    t(80.0f, 60, CH_BOSS);
}

void play_boss_death() {
    // Explosion layer on CH_EXPLODE + ascending triumph on CH_BIOME.
    // Total ~1200 ms (600 ms explosion run, 600 ms arpeggio with overlap).
    t(800.0f, 120, CH_EXPLODE);
    t(500.0f, 140, CH_EXPLODE);
    t(250.0f, 160, CH_EXPLODE);
    t(120.0f, 180, CH_EXPLODE);
    noise_burst(200, 10, 70, 260, CH_HIT);
    // Triumph: C4 → C5 → E5 → G5, 150 ms each.
    t(262.0f, 150, CH_BIOME);
    t(523.0f, 150, CH_BIOME);
    t(659.0f, 150, CH_BIOME);
    t(784.0f, 150, CH_BIOME);
}

// ---- Biome / death ------------------------------------------------------

void play_biome_transition() {
    // Sci-fi sweep 300 → 2000 Hz across ~500 ms (10 × 50 ms), then a
    // final chime at 1500 Hz for ~100 ms.
    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        float f = 300.0f + (1700.0f * i) / (N - 1);
        t(f, 50, CH_BIOME);
    }
    t(1500.0f, 100, CH_BIOME);
}

void play_level_complete() {
    // Triumphant 6-note fanfare: C5-E5-G5-C6-E6-G6 (523, 659, 784, 1047, 1319,
    // 1568 Hz). Played on CH_BIOME so it layers above the biome-transition
    // sweep without cutting it off. ~480 ms total.
    t( 523.0f, 80, CH_BIOME);
    t( 659.0f, 80, CH_BIOME);
    t( 784.0f, 80, CH_BIOME);
    t(1047.0f, 80, CH_BIOME);
    t(1319.0f, 80, CH_BIOME);
    t(1568.0f, 80, CH_BIOME);
}

void play_death() {
    // Short descending glitch ending in C2 (65 Hz). ~270 ms total:
    // 6 slices × 40 ms glitching from 1500 → 65 Hz with random jitter.
    // The previous 12-slice / 700 ms version queued so deep on CH_HIT
    // that any subsequent SFX (including the next run's hits) sat behind
    // a half-second of death-noise drain.
    constexpr int N = 6;
    for (int i = 0; i < N; ++i) {
        int centre = 1500 - ((1500 - 65) * i) / (N - 1);
        int jitter = (int)random(-200, 200);
        int f = centre + jitter;
        if (f < 60) f = 60;
        t((float)f, 40, CH_HIT);
    }
    t(65.0f, 30, CH_HIT);
}

void stop_all() {
    // Cancel the non-blocking sweep first so tick() doesn't immediately
    // re-emit a CH_CHARGE tone after we clear the mixer.
    g_charge_active = false;
    // Stop every channel — the death-sound queue is the main offender on
    // CH_HIT, but explosions / boss thuds / biome chimes could all still
    // be draining when the user hits R to retry. Clear the slate.
    M5.Speaker.stop();
}

// ---- Menu / misc -------------------------------------------------------

void play_menu_tick() {
    t(1500.0f, 10, CH_PICKUP);
}

void play_menu_confirm() {
    // Rising chord C5-G5-C6 (523, 784, 1047 Hz), each 50 ms.
    t( 523.0f, 50, CH_PICKUP);
    t( 784.0f, 50, CH_PICKUP);
    t(1047.0f, 50, CH_PICKUP);
}

void play_powerup_spawn() {
    t(1500.0f, 40, CH_PICKUP);
}

// ---- Main-loop hook ----------------------------------------------------

void tick() {
    // Hot path: not building a charge → single bool check and return.
    if (!g_charge_active) return;

    uint32_t now = millis();
    uint32_t elapsed = now - g_charge_start_ms;
    if (elapsed >= CHARGE_DURATION_MS) {
        // Reached the top of the sweep — hold the build at peak until the
        // caller fires play_charge_release() or cancel_charge_build().
        if (now - g_charge_last_emit_ms >= CHARGE_STEP_MS) {
            t(CHARGE_F1, CHARGE_TONE_MS, CH_CHARGE);
            g_charge_last_emit_ms = now;
        }
        return;
    }

    if (now - g_charge_last_emit_ms < CHARGE_STEP_MS) return;
    g_charge_last_emit_ms = now;

    float frac = (float)elapsed / (float)CHARGE_DURATION_MS;
    float freq = CHARGE_F0 + (CHARGE_F1 - CHARGE_F0) * frac;
    t(freq, CHARGE_TONE_MS, CH_CHARGE);
}

}  // namespace sfx
}  // namespace rtype
}  // namespace apps
