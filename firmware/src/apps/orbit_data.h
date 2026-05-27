#pragma once

// orbit-fighter: static data tables.
//
// This file declares the enum IDs, POD structs, and lookup functions for
// every "design-time" value the shoot-em-up needs: enemy stats, bosses,
// biome spawn timelines, powerup weights, and the loop-difficulty curve.
//
// All tables live in flash (PROGMEM / static constexpr arrays); no PSRAM
// is required. Implementation lives in orbit_data.cpp.

#include <stddef.h>
#include <stdint.h>

namespace apps {
namespace orbit {
namespace data {

// ---------------------------------------------------------------------------
// Enum IDs
// ---------------------------------------------------------------------------

// Enemy IDs intentionally match the sprite-atlas IDs a parallel agent is
// authoring — keep these in lock-step. Order is the public enumeration
// order; numeric values are auto-assigned starting at 0.
enum class EnemyId : uint8_t {
    BRACE_SWARM = 0,
    DRONE_404,
    DRONE_500,
    LAMBDA,
    NULL_STALKER,
    TAG_SCOUT,
    FOR_LOOP,
    MISSILE,
    _COUNT,
};

enum class BossId : uint8_t {
    REGEX_TYRANT = 0,
    CONTEXT_OVERFLOW,
    HALLUCINATION,
    RATE_LIMITER,
    ROGUE_TOOL_CALL,
    _COUNT,
};

// BiomeId values are fixed (0..4) because they're used as array indices
// for the static biome config table.
enum class BiomeId : uint8_t {
    STDOUT  = 0,
    STDIN   = 1,
    STAGING = 2,
    SANDBOX = 3,
    PROD    = 4,
};

enum class PowerupId : uint8_t {
    HP = 0,
    CLEAR,
    CACHE,
    TOKENS,
    SUBAGENT,
    SKIP_PERMS,
    _COUNT,
};

// Movement patterns the runtime understands. The data layer just tags an
// enemy with one of these; orbit.cpp decides what it actually means.
//   0 = linear            : march left at speed_x
//   1 = sine_y            : linear x, sinusoidal y
//   2 = track_player_slow : home toward player at speed_x magnitude
//   3 = swarm_v_formation : grouped V wedge, marches left
//   4 = stalker_cloak     : invisible until within striking distance
// (Kept as bare uint8_t in EnemyStats so future patterns can be added
// without changing the binary table layout.)

// ---------------------------------------------------------------------------
// Stat structs
// ---------------------------------------------------------------------------

struct EnemyStats {
    uint8_t  max_hp;
    int8_t   speed_x;                      // px/frame, negative = leftward
    int8_t   speed_y;                      // px/frame, 0 for linear movers
    uint8_t  score_value;
    uint8_t  movement_pattern;             // see comment above
    bool     can_shoot;
    uint16_t shoot_interval_ms;            // 0 when can_shoot == false
    uint8_t  bullet_speed_px_per_frame;
};

struct BossStats {
    uint16_t    max_hp;                    // 200..800
    int16_t     entrance_x;                // typically FIELD_X1 - 50
    int16_t     entrance_y;                // vertical center of playfield
    uint8_t     pattern_id;                // 0..4, see boss patterns
    uint16_t    score_value;
    const char* name;
    const char* intro_hint;                // shown in boss intro modal
};

// One scheduled enemy spawn inside a biome timeline.
struct EnemySpawnEntry {
    EnemyId  enemy;
    uint32_t at_ms;                        // when in the biome to spawn (up to ~100s)
    int16_t  y;                            // y position; -1 = random
    int8_t   group_size;                   // column size on spawn
    int16_t  group_spacing_ms;             // delay between group members (data uses up to ~900)
};

struct PowerupSpawn {
    PowerupId id;
    uint8_t   weight;                      // relative weight
};

struct BiomeConfig {
    const char*            label;          // "STDOUT", ...
    BossId                 boss;
    uint32_t               duration_ms_before_boss;
    const EnemySpawnEntry* spawns;
    size_t                 spawn_count;
    uint8_t                bg_intensity;   // 0..100, background hint
};

// ---------------------------------------------------------------------------
// Lookup functions
// ---------------------------------------------------------------------------

const EnemyStats&  enemy_stats(EnemyId id);
const BossStats&   boss_stats(BossId id);
const BiomeConfig& biome_config(BiomeId id);

int      biome_count();                    // returns 5
BiomeId  next_biome(BiomeId current);      // STDOUT -> STDIN -> ... -> STDOUT

// Returns a percentage (100 = 1.00x). Used to scale enemy HP / speed as
// the player loops through biomes more than once. Capped at 250%.
int      loop_difficulty_multiplier(int loop_number);

// Weighted random pick. The total weight in the table is intentionally
// less than 100 so "no powerup" is the implicit majority outcome.
PowerupId pick_random_powerup();

}  // namespace data
}  // namespace orbit
}  // namespace apps
