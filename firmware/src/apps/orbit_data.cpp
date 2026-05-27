#include "orbit_data.h"

#include <stdlib.h>

// orbit-fighter: data tables (definitions).
//
// All large arrays are `static constexpr` so they end up in flash on the
// ESP32-S3 (the Cardputer ADV's MCU) without needing PSRAM. The lookup
// functions are thin index-bounded accessors; they never allocate.

namespace apps {
namespace orbit {
namespace data {

namespace {

// ---------------------------------------------------------------------------
// Playfield reminders (sourced from orbit.cpp; kept here as comments so
// the spawn y values below are easy to sanity-check):
//   FIELD_X0 =   2   FIELD_X1 = 238
//   FIELD_Y0 =  30   FIELD_Y1 = 121     usable height = 91 px
// Enemies are ~12 px tall, so spawn y in [32 .. 108] is a safe band.
// ---------------------------------------------------------------------------

// ---------- Enemy stats ----------------------------------------------------
// Indexed by EnemyId. Order MUST match the EnemyId enum.
static constexpr EnemyStats kEnemyTable[] = {
    // BRACE_SWARM — tiny `{ }` glyphs, die in one hit, arrive in wedges.
    { /*hp*/ 1, /*sx*/ -2, /*sy*/ 0, /*score*/ 10,
      /*pattern*/ 3 /*swarm_v*/, /*shoot*/ false, /*ivl*/ 0, /*bspd*/ 0 },

    // DRONE_404 — standard cannon-fodder, shoots slow plasma.
    { 4, -1, 0, 50, 0 /*linear*/, true, 1500, 3 },

    // DRONE_500 — heavier 5xx variant, more HP, faster fire.
    { 8, -1, 0, 100, 0 /*linear*/, true, 1200, 3 },

    // LAMBDA — fast, sine-y arc, harasses with fast bullets.
    { 3, -3, 1, 80, 1 /*sine_y*/, true, 800, 4 },

    // NULL_STALKER — cloaks until close, no shots but rams the player.
    { 2, -2, 0, 70, 4 /*stalker_cloak*/, false, 0, 0 },

    // TAG_SCOUT — opens like `<tag>` and on death spawns 2 BRACE_SWARMs.
    // The "spawn on death" behavior is handled by game logic; data layer
    // just exposes the stats.
    { 2, -1, 0, 30, 0 /*linear*/, false, 0, 0 },

    // FOR_LOOP — homes slowly toward player and fires while pursuing.
    { 5, -1, 0, 60, 2 /*track_player_slow*/, true, 1000, 3 },

    // MISSILE — practically a fast bullet with HP=1. Spawned in volleys.
    { 1, -5, 0, 20, 0 /*linear*/, false, 0, 0 },
};
static_assert(sizeof(kEnemyTable) / sizeof(kEnemyTable[0]) ==
              static_cast<size_t>(EnemyId::_COUNT),
              "EnemyId enum and kEnemyTable are out of sync");

// ---------- Boss stats -----------------------------------------------------
// `entrance_x` sits just inside the right edge; `entrance_y` is the vertical
// center of the playfield (FIELD_Y0=30, FIELD_Y1=121 -> midpoint ~75).
static constexpr BossStats kBossTable[] = {
    // REGEX_TYRANT
    { /*hp*/ 200, /*ex*/ 188, /*ey*/ 75, /*pattern*/ 0 /*regex_walls*/,
      /*score*/ 1000, "REGEX_TYRANT",
      "Fires `.*` walls - strafe between them." },

    // CONTEXT_OVERFLOW
    { 400, 188, 75, 1 /*context_grow*/, 2000, "CONTEXT_OVERFLOW",
      "Grows nested braces. Hit the core." },

    // HALLUCINATION
    { 300, 188, 75, 2 /*hallucination_clones*/, 2500, "HALLUCINATION",
      "Three copies - only one is real." },

    // RATE_LIMITER
    { 350, 188, 75, 3 /*time_dilation*/, 3000, "RATE_LIMITER",
      "Slows your inputs - keep firing." },

    // ROGUE_TOOL_CALL — final boss, big HP, splits on hit.
    { 600, 188, 75, 4 /*splitting_glyphs*/, 5000, "ROGUE_TOOL_CALL",
      "Splits when hit. Don't let it spawn." },
};
static_assert(sizeof(kBossTable) / sizeof(kBossTable[0]) ==
              static_cast<size_t>(BossId::_COUNT),
              "BossId enum and kBossTable are out of sync");

// ---------- Biome spawn timelines -----------------------------------------
//
// Each timeline runs from t=0 up to BiomeConfig::duration_ms_before_boss,
// after which the game spawns the boss. y = -1 means "pick a random y in
// the playable band". group_size > 1 means spawn N enemies in a vertical
// column, group_spacing_ms apart.

// --- BIOME 0: STDOUT (60 s) — tutorial. Mostly BRACE_SWARM + sparse 404. --
// Enemy set: BRACE_SWARM, DRONE_404. No missiles, no stalkers, no lambdas.
static constexpr EnemySpawnEntry kSpawnsStdout[] = {
    { EnemyId::BRACE_SWARM,  3000,  60, 3, 80 },
    { EnemyId::BRACE_SWARM,  6500,  -1, 4, 70 },
    { EnemyId::BRACE_SWARM, 10500,  45, 4, 70 },
    { EnemyId::DRONE_404,   15000,  70, 1,   0 },
    { EnemyId::BRACE_SWARM, 19000,  -1, 5, 60 },
    { EnemyId::BRACE_SWARM, 24000,  90, 4, 70 },
    { EnemyId::DRONE_404,   29000,  50, 1,   0 },
    { EnemyId::BRACE_SWARM, 33000,  -1, 5, 60 },
    { EnemyId::BRACE_SWARM, 38000,  40, 4, 60 },
    { EnemyId::DRONE_404,   42500,  85, 2, 800 },
    { EnemyId::BRACE_SWARM, 47000,  -1, 6, 50 },
    { EnemyId::BRACE_SWARM, 52000,  55, 5, 55 },
    { EnemyId::DRONE_404,   56500,  70, 1,   0 },
};

// --- BIOME 1: STDIN (70 s) — TAG_SCOUT cascades + LAMBDA harassment. ------
// Enemy set: BRACE_SWARM, DRONE_404, LAMBDA, TAG_SCOUT. Still no missiles.
static constexpr EnemySpawnEntry kSpawnsStdin[] = {
    { EnemyId::BRACE_SWARM,  2000,  -1, 3, 70 },
    { EnemyId::TAG_SCOUT,    4500,  55, 1,   0 },
    { EnemyId::LAMBDA,       8000,  -1, 1,   0 },
    { EnemyId::DRONE_404,   11500,  70, 2, 700 },
    { EnemyId::TAG_SCOUT,   16000,  45, 2, 700 },
    { EnemyId::LAMBDA,      20500,  -1, 2, 500 },
    { EnemyId::BRACE_SWARM, 25000,  -1, 5, 60 },
    { EnemyId::TAG_SCOUT,   29500,  -1, 1,   0 },
    { EnemyId::LAMBDA,      33000,  60, 3, 350 },
    { EnemyId::DRONE_404,   38000,  -1, 2, 700 },
    { EnemyId::TAG_SCOUT,   43000,  80, 2, 600 },
    { EnemyId::LAMBDA,      48000,  -1, 3, 300 },
    { EnemyId::TAG_SCOUT,   53500,  -1, 2, 500 },
    { EnemyId::LAMBDA,      58000,  -1, 4, 280 },
    { EnemyId::DRONE_404,   63500,  50, 2, 600 },
    { EnemyId::TAG_SCOUT,   67500,  -1, 2, 500 },
};

// --- BIOME 2: STAGING (80 s) — DRONE_500 debut, dense V-formations. -------
// Enemy set: DRONE_404, DRONE_500, LAMBDA, BRACE_SWARM (V-wedges as filler).
static constexpr EnemySpawnEntry kSpawnsStaging[] = {
    { EnemyId::DRONE_404,    1800,  50, 2, 600 },
    { EnemyId::BRACE_SWARM,  5500,  -1, 5, 55 },
    { EnemyId::DRONE_500,    9500,  65, 1,   0 },
    { EnemyId::DRONE_404,   13500,  -1, 3, 500 },
    { EnemyId::LAMBDA,      18000,  -1, 2, 450 },
    { EnemyId::DRONE_500,   23000,  40, 1,   0 },
    { EnemyId::DRONE_500,   25500,  90, 1,   0 },
    { EnemyId::BRACE_SWARM, 30000,  -1, 6, 50 },
    { EnemyId::DRONE_404,   35000,  -1, 4, 450 },
    { EnemyId::DRONE_500,   41000,  60, 2, 700 },
    { EnemyId::LAMBDA,      47000,  -1, 3, 350 },
    { EnemyId::DRONE_500,   53000,  -1, 2, 600 },
    { EnemyId::DRONE_404,   58500,  -1, 3, 400 },
    { EnemyId::BRACE_SWARM, 63500,  -1, 7, 45 },
    { EnemyId::DRONE_500,   68500,  50, 2, 550 },
    { EnemyId::LAMBDA,      73000,  -1, 3, 300 },
    { EnemyId::DRONE_500,   77000,  -1, 2, 500 },
};

// --- BIOME 3: SANDBOX (90 s) — ambush + tracking + MISSILE volleys. -------
// Enemy set: NULL_STALKER, FOR_LOOP, MISSILE, LAMBDA, DRONE_500. No braces.
static constexpr EnemySpawnEntry kSpawnsSandbox[] = {
    { EnemyId::NULL_STALKER, 2000,  -1, 1,   0 },
    { EnemyId::FOR_LOOP,     5500,  60, 1,   0 },
    { EnemyId::MISSILE,     10000,  -1, 4, 110 },
    { EnemyId::NULL_STALKER,14000,  -1, 2, 700 },
    { EnemyId::FOR_LOOP,    19500,  -1, 2, 800 },
    { EnemyId::MISSILE,     25000,  -1, 5, 100 },
    { EnemyId::LAMBDA,      30000,  -1, 2, 400 },
    { EnemyId::NULL_STALKER,35500,  -1, 2, 600 },
    { EnemyId::MISSILE,     41000,  -1, 6,  95 },
    { EnemyId::FOR_LOOP,    47000,  45, 2, 700 },
    { EnemyId::DRONE_500,   53000,  -1, 1,   0 },
    { EnemyId::NULL_STALKER,57500,  -1, 3, 600 },
    { EnemyId::MISSILE,     63500,  -1, 7,  85 },
    { EnemyId::FOR_LOOP,    70000,  -1, 2, 700 },
    { EnemyId::NULL_STALKER,75500,  -1, 2, 500 },
    { EnemyId::MISSILE,     80000,  -1, 8,  75 },
    { EnemyId::LAMBDA,      85500,  -1, 3, 300 },
};

// --- BIOME 4: PROD (100 s) — every enemy, bullet-hell finale. -------------
// Enemy set: ALL 8. Steady alternation of heavies, trackers, and volleys.
static constexpr EnemySpawnEntry kSpawnsProd[] = {
    { EnemyId::DRONE_500,    1000,  50, 1,   0 },
    { EnemyId::LAMBDA,       3500,  -1, 2, 300 },
    { EnemyId::MISSILE,      7000,  -1, 5,  90 },
    { EnemyId::NULL_STALKER,11500,  -1, 2, 600 },
    { EnemyId::FOR_LOOP,    16000,  -1, 2, 600 },
    { EnemyId::TAG_SCOUT,   20500,  -1, 2, 500 },
    { EnemyId::BRACE_SWARM, 25000,  -1, 7,  45 },
    { EnemyId::MISSILE,     29500,  -1, 6,  80 },
    { EnemyId::DRONE_500,   34000,  -1, 3, 500 },
    { EnemyId::LAMBDA,      39500,  -1, 3, 280 },
    { EnemyId::NULL_STALKER,44500,  -1, 3, 600 },
    { EnemyId::DRONE_404,   49500,  -1, 4, 400 },
    { EnemyId::MISSILE,     54500,  -1, 7,  75 },
    { EnemyId::FOR_LOOP,    60000,  -1, 3, 550 },
    { EnemyId::TAG_SCOUT,   65000,  -1, 3, 450 },
    { EnemyId::DRONE_500,   70000,  -1, 3, 450 },
    { EnemyId::MISSILE,     75500,  -1, 8,  70 },
    { EnemyId::LAMBDA,      81000,  -1, 4, 240 },
    { EnemyId::NULL_STALKER,86000,  -1, 3, 500 },
    { EnemyId::MISSILE,     91000,  -1, 8,  65 },
    { EnemyId::BRACE_SWARM, 96000,  -1, 9,  35 },
};

// ---------- Biome table ----------------------------------------------------
// Indexed by BiomeId 0..4.
static constexpr BiomeConfig kBiomeTable[] = {
    { "STDOUT",  BossId::REGEX_TYRANT,      60000,
      kSpawnsStdout,
      sizeof(kSpawnsStdout) / sizeof(kSpawnsStdout[0]),  20 },
    { "STDIN",   BossId::CONTEXT_OVERFLOW,  70000,
      kSpawnsStdin,
      sizeof(kSpawnsStdin) / sizeof(kSpawnsStdin[0]),    40 },
    { "STAGING", BossId::HALLUCINATION,     80000,
      kSpawnsStaging,
      sizeof(kSpawnsStaging) / sizeof(kSpawnsStaging[0]), 60 },
    { "SANDBOX", BossId::RATE_LIMITER,      90000,
      kSpawnsSandbox,
      sizeof(kSpawnsSandbox) / sizeof(kSpawnsSandbox[0]), 80 },
    { "PROD",    BossId::ROGUE_TOOL_CALL,  100000,
      kSpawnsProd,
      sizeof(kSpawnsProd) / sizeof(kSpawnsProd[0]),     100 },
};
static constexpr int kBiomeCount =
    sizeof(kBiomeTable) / sizeof(kBiomeTable[0]);
static_assert(kBiomeCount == 5, "Expected exactly 5 biomes");

// ---------- Powerup weights -----------------------------------------------
// Weights sum to 75; the remaining 25/100 of the roll yields no powerup.
static constexpr PowerupSpawn kPowerupTable[] = {
    { PowerupId::HP,         20 },
    { PowerupId::CLEAR,      10 },
    { PowerupId::CACHE,      15 },
    { PowerupId::TOKENS,     15 },
    { PowerupId::SUBAGENT,   10 },
    { PowerupId::SKIP_PERMS,  5 },
};
static constexpr int kPowerupCount =
    sizeof(kPowerupTable) / sizeof(kPowerupTable[0]);
// Total weight used by the weighted pick. We compute it at compile time so
// the runtime loop stays branch-free.
static constexpr int kPowerupTotalWeight = 20 + 10 + 15 + 15 + 10 + 5;  // 75
static_assert(kPowerupTotalWeight == 75, "Adjust kPowerupTotalWeight");

// ---------- Loop difficulty curve -----------------------------------------
//
// The curve is loosely geometric (~1.2x per loop) with a hard cap at 250%
// so late-loop play stays beatable on a 240x135 screen at 30 fps. Values
// are hand-tuned rather than computed so we can ship a small lookup table
// and avoid runtime float math on the ESP32. Loops beyond the last entry
// clamp to the cap.
static constexpr int kLoopMultipliers[] = {
    100,  // loop 0 — baseline
    120,  // loop 1 — first repeat, +20%
    145,  // loop 2 — ~+1.21x compounding feel
    175,  // loop 3 — getting hard
    210,  // loop 4 — bullet-hell territory
    250,  // loop 5+ — cap
};
static constexpr int kLoopMultiplierCount =
    sizeof(kLoopMultipliers) / sizeof(kLoopMultipliers[0]);

// Default-constructed fallbacks for out-of-range queries. Defined once so
// lookup functions can return a const& without UB.
static constexpr EnemyStats kEnemyFallback  = { 0, 0, 0, 0, 0, false, 0, 0 };
static constexpr BossStats  kBossFallback   = { 0, 0, 0, 0, 0, "?", "?" };
static constexpr EnemySpawnEntry kEmptySpawns[] = {
    { EnemyId::BRACE_SWARM, 0, -1, 0, 0 }
};
static constexpr BiomeConfig kBiomeFallback = {
    "?", BossId::REGEX_TYRANT, 0, kEmptySpawns, 0, 0
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const EnemyStats& enemy_stats(EnemyId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(EnemyId::_COUNT)) return kEnemyFallback;
    return kEnemyTable[idx];
}

const BossStats& boss_stats(BossId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(BossId::_COUNT)) return kBossFallback;
    return kBossTable[idx];
}

const BiomeConfig& biome_config(BiomeId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(kBiomeCount)) return kBiomeFallback;
    return kBiomeTable[idx];
}

int biome_count() {
    return kBiomeCount;
}

BiomeId next_biome(BiomeId current) {
    const auto idx = static_cast<size_t>(current);
    // Cycle: PROD wraps back to STDOUT. Anything out-of-range also resets
    // to STDOUT rather than indexing garbage.
    if (idx >= static_cast<size_t>(kBiomeCount) - 1) return BiomeId::STDOUT;
    return static_cast<BiomeId>(idx + 1);
}

int loop_difficulty_multiplier(int loop_number) {
    if (loop_number <= 0) return kLoopMultipliers[0];
    if (loop_number >= kLoopMultiplierCount) {
        return kLoopMultipliers[kLoopMultiplierCount - 1];
    }
    return kLoopMultipliers[loop_number];
}

PowerupId pick_random_powerup() {
    // Pull a roll over the FULL [0..99] space, not just [0..total). That
    // way the unused 25 "weight" naturally falls through to a sensible
    // default (HP) — callers that want "no powerup" semantics should use
    // their own roll; this function always returns a valid PowerupId.
    int roll = rand() % 100;
    int acc  = 0;
    for (int i = 0; i < kPowerupCount; ++i) {
        acc += kPowerupTable[i].weight;
        if (roll < acc) return kPowerupTable[i].id;
    }
    return PowerupId::HP;
}

}  // namespace data
}  // namespace orbit
}  // namespace apps
