#include "orbit.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../app.h"
#include "../theme.h"
#include "orbit_bg.h"
#include "orbit_data.h"
#include "orbit_sfx.h"
#include "orbit_sprites.h"

namespace apps {
namespace orbit {

namespace {

// ---------------------------------------------------------------------------
// Layout constants. The global status bar at y=0..13 is owned by app.cpp; we
// only ever draw inside [CONTENT_TOP .. SCREEN_H-1]. The HUD lives in a
// 16 px chrome band right under it, footer is the bottom 14 px, and the rest
// is the playfield. These match the values orbit_bg.cpp scrolls inside of.
constexpr int HUD_Y      = theme::CONTENT_TOP;
constexpr int HUD_H      = 16;
constexpr int FIELD_X0   = 2;
constexpr int FIELD_Y0   = HUD_Y + HUD_H;       // 30
constexpr int FIELD_X1   = theme::SCREEN_W - 2; // 238
constexpr int FIELD_Y1   = theme::SCREEN_H - 14;// 121
constexpr int FOOTER_Y   = FIELD_Y1;
constexpr int FOOTER_H   = theme::SCREEN_H - FOOTER_Y;

constexpr int PLAYER_W   = 20;   // bumped from 16 for the bigger rocket sprite
constexpr int PLAYER_H   = 10;   // bumped from 8
constexpr int PLAYER_VEL = 2;
constexpr int FIRE_COOLDOWN_MS = 130;  // hold-fire rate ~7.5 shots/sec — less spammy
constexpr int FRAME_INTERVAL_MS = 33;     // ~30 fps

// Charge thresholds (ms held).
constexpr uint32_t CHARGE_L1_MS = 300;
constexpr uint32_t CHARGE_L2_MS = 600;
constexpr uint32_t CHARGE_L3_MS = 1200;

// Pool sizes — all under 4 KB total. enemies(32*~36) + e_bul(24*~12) +
// p_bul(16*~12) + powerups(6*~16) ~= 1.5 KB.
constexpr int MAX_ENEMIES       = 32;
constexpr int MAX_PLAYER_BULLET = 16;
constexpr int MAX_ENEMY_BULLET  = 24;
constexpr int MAX_POWERUPS      = 6;

// Game modes — the run loop branches on this for input + rendering.
enum class Mode : uint8_t {
    PLAYING,
    BOSS_INTRO,
    STAGE_CLEARED,   // brief celebratory modal between boss death and next biome
    PAUSED,
    DEAD,
};

// ---------- Entities ------------------------------------------------------

struct Player {
    int16_t x, y;
    int16_t prev_x, prev_y;
    uint8_t hp;
    bool    active;
    // charge shot
    bool     charging;
    uint32_t charge_started_ms;
    uint8_t  charge_visual_level;     // 0..3, what sprite is drawn
    uint8_t  free_charges;            // TOKENS powerup: instant charges left
    // invincibility (CACHE powerup) — render the ship blinking, ignore damage
    uint32_t invuln_until_ms;
    bool     shield_drawn_prev;       // shield ring was painted last frame
    int16_t  shield_prev_cx, shield_prev_cy;
    // subagent (5 s second ship at fixed offset, auto-fires)
    uint32_t subagent_until_ms;
    uint32_t subagent_last_fire_ms;
    int16_t  sub_prev_x, sub_prev_y;
    bool     sub_prev_drawn;
};

struct Enemy {
    int16_t x, y;
    int16_t prev_x, prev_y;
    int8_t  vx, vy;
    uint16_t hp;
    uint16_t score_value;
    uint16_t shoot_interval_ms;
    uint32_t last_shot_ms;
    uint32_t age_ms;                  // since spawn
    int16_t  spawn_y;                 // for sine pattern reference
    data::EnemyId id;
    uint8_t  pattern;
    uint8_t  bullet_speed;
    bool     can_shoot;
    bool     active;
    bool     prev_drawn;              // was it visible last frame? (cloak)
    bool     visible;                 // currently visible (cloak)
};

struct Bullet {
    int16_t x, y;
    int16_t prev_x, prev_y;
    int8_t  vx, vy;
    uint8_t  damage;
    bool     active;
    bool     prev_drawn;
    bool     is_charged;              // player bullets only
    bool     is_pierce;               // doesn't despawn on hit
    bool     is_enemy;                // marks origin (also sprite choice)
    bool     is_boss;                 // boss bullet sprite
};

struct Powerup {
    int16_t x, y;
    int16_t prev_x, prev_y;
    uint32_t spawn_ms;
    data::PowerupId id;
    bool     active;
    bool     prev_drawn;
};

// The orbiting pod ("subagent companion"). Two states: attached (orbits
// around the player) and detached (flies right, auto-fires, recalls after
// 1 s or 8 s in SKIP_PERMS mode).
enum class PodMode : uint8_t { LASER = 0, SPREAD = 1, PIERCE = 2 };
struct Pod {
    int16_t x, y;
    int16_t prev_x, prev_y;
    bool     attached;
    bool     active;                  // exists at all
    bool     prev_drawn;
    PodMode  mode;
    float    orbit_angle;             // when attached
    uint32_t last_fire_ms;
    uint32_t detach_started_ms;
    uint32_t skip_perms_until_ms;     // detached + rapid-fire window
};

struct Boss {
    int16_t x, y;
    int16_t prev_x, prev_y;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t score_value;
    data::BossId id;
    uint8_t  pattern_id;
    bool     active;
    bool     entering;                // sliding in from right
    bool     prev_drawn;
    uint32_t spawn_ms;
    uint32_t last_volley_ms;
    int8_t   sweep_dir;               // for regex walls, +1/-1
    int16_t  sweep_y;
};

// Pair of mini-bosses spawned when ROGUE_TOOL_CALL splits at <50% HP.
// While active they own the boss fight; g_boss.active is false. We use a
// fixed pool of 2 rather than dynamic alloc.
struct MiniBoss {
    int16_t  x, y;
    int16_t  prev_x, prev_y;
    int16_t  base_y;                  // sine reference
    uint16_t hp;
    uint16_t max_hp;
    uint32_t age_ms;
    uint32_t last_fire_ms;
    int8_t   vy_drift;                // +1 / -1 for the initial separation
    bool     active;
    bool     prev_drawn;
};

// Brief floating "+hp" / "/clear" labels above the player on pickup.
struct Floater {
    int16_t  x, y;
    int16_t  prev_x, prev_y;
    int16_t  spawn_y;                 // anchor for deterministic upward drift
    uint32_t spawn_ms;
    const char* text;
    bool     active;
    bool     prev_drawn;
};

// ---------- Globals -------------------------------------------------------

Player   g_player;
Pod      g_pod;
Boss     g_boss;
Enemy    g_enemies[MAX_ENEMIES];
Bullet   g_pbul[MAX_PLAYER_BULLET];
Bullet   g_ebul[MAX_ENEMY_BULLET];
Powerup  g_pups[MAX_POWERUPS];

// ROGUE_TOOL_CALL split state. When the boss drops below 50% it despawns
// and these two minis take over. on_boss_killed runs only when both are
// dead.
constexpr int MAX_MINIS = 2;
MiniBoss g_minis[MAX_MINIS];
bool     g_split_active = false;        // true while minis are the fight

// HALLUCINATION clone state. The single BOSS_HALLUCINATION sprite is
// 48x16 — three 16x16 ghosts side-by-side. We treat each 16-px column as
// a separate collidable clone; only g_hallu_real_idx takes damage.
int      g_hallu_real_idx = 0;
uint32_t g_hallu_last_rotate_ms = 0;
uint32_t g_hallu_last_fire_ms   = 0;
uint32_t g_hallu_flash_until_ms = 0;    // brief flash on the real clone

// RATE_LIMITER throttle window — see boss_tick_rate_limiter().
bool     g_rate_throttle_active   = false;
uint32_t g_rate_throttle_until_ms = 0;
uint32_t g_rate_next_throttle_ms  = 0;

// Floating powerup pickup labels. Tiny static pool — at most 4 visible
// at once.
constexpr int MAX_FLOATERS = 4;
Floater  g_floaters[MAX_FLOATERS];
constexpr uint32_t FLOATER_LIFETIME_MS = 500;
constexpr int      FLOATER_DRIFT_PX    = 16;

Mode     g_mode = Mode::PLAYING;
uint32_t g_last_fire_ms   = 0;
uint32_t g_last_frame_ms  = 0;
uint32_t g_biome_start_ms = 0;
uint32_t g_score          = 0;
uint32_t g_best_score     = 0;
uint8_t  g_loop_number    = 0;
uint8_t  g_best_loop      = 0;
data::BiomeId g_biome     = data::BiomeId::STDOUT;
size_t   g_next_spawn_idx = 0;        // index into the biome's spawn list
// When a wave entry has group_size > 1 we stagger members spaced by
// group_spacing_ms; this struct tracks the in-flight group.
struct PendingGroup {
    bool     active;
    uint8_t  remaining;
    uint32_t next_at_ms;
    uint16_t spacing_ms;
    data::EnemyId enemy;
    int16_t  y;
    int16_t  vshape_step;             // for swarm V formation
    uint8_t  total;
} g_pending = {};
bool     g_boss_intro_seen = false;
uint32_t g_boss_intro_started_ms = 0;
bool     g_hud_dirty       = true;
uint8_t  g_pause_idx       = 0;       // for pause menu cursor
bool     g_modal_drawn     = false;   // current modal painted to fb
uint32_t g_stage_cleared_until_ms = 0; // STAGE_CLEARED timer
data::BiomeId g_cleared_biome = data::BiomeId::STDOUT;  // shown in modal
uint32_t g_cleared_score_gain = 0;     // boss score added this clear
uint32_t g_death_at_ms = 0;            // when DEAD mode started — delays modal so the explosion can play

// Input-edge tracking: we want SPACE press/release to drive charge state.
bool g_space_prev = false;   // legacy name — now tracks ctrl-OR-space edge
bool g_l_prev     = false;
bool g_enter_prev = false;
bool g_esc_prev   = false;
bool g_r_prev     = false;
bool g_q_prev     = false;
bool g_up_prev    = false;   // w / ; (for menu nav edge detection)
bool g_down_prev  = false;   // s / .

// Rate-limit movement application: handle_input runs every Arduino loop tick
// (~1 kHz+) but movement at PLAYER_VEL=2 must apply at most once per render
// frame, otherwise the ship teleports across the playfield in milliseconds.
uint32_t g_last_move_ms = 0;

// NVS persistence — same Preferences API as settings/store.cpp.
constexpr const char* NVS_NS = "orbit";

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// ---------- Helpers -------------------------------------------------------

void load_highscore() {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) return;
    g_best_score = p.getUInt("best", 0);
    g_best_loop  = p.getUChar("loop", 0);
    p.end();
}

void save_highscore() {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) return;
    p.putUInt("best", g_best_score);
    p.putUChar("loop", g_best_loop);
    p.end();
}

int random_spawn_y() {
    // Enemies are ~16 px tall, so spawn y in [FIELD_Y0+2 .. FIELD_Y1-18].
    int lo = FIELD_Y0 + 2;
    int hi = FIELD_Y1 - 18;
    if (hi < lo) return lo;
    return lo + (int)(rand() % (hi - lo + 1));
}

void erase_rect(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return;
    // Clip to playfield so we don't punch holes in HUD / footer.
    if (x < FIELD_X0) { w -= (FIELD_X0 - x); x = FIELD_X0; }
    if (y < FIELD_Y0) { h -= (FIELD_Y0 - y); y = FIELD_Y0; }
    if (x + w > FIELD_X1) w = FIELD_X1 - x;
    if (y + h > FIELD_Y1) h = FIELD_Y1 - y;
    if (w <= 0 || h <= 0) return;
    M5.Display.fillRect(x, y, w, h, to565(theme::IVORY));
}

bool aabb_overlap(int ax, int ay, int aw, int ah,
                  int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

SpriteId enemy_sprite(data::EnemyId id) {
    switch (id) {
        case data::EnemyId::BRACE_SWARM:  return SpriteId::BRACE_SWARM;
        case data::EnemyId::DRONE_404:    return SpriteId::DRONE_404;
        case data::EnemyId::DRONE_500:    return SpriteId::DRONE_500;
        case data::EnemyId::LAMBDA:       return SpriteId::LAMBDA;
        case data::EnemyId::NULL_STALKER: return SpriteId::NULL_STALKER;
        case data::EnemyId::TAG_SCOUT:    return SpriteId::TAG_SCOUT;
        case data::EnemyId::FOR_LOOP:     return SpriteId::FOR_LOOP;
        case data::EnemyId::MISSILE:      return SpriteId::MISSILE;
        default:                          return SpriteId::DRONE_404;
    }
}

SpriteId powerup_sprite(data::PowerupId id) {
    switch (id) {
        case data::PowerupId::HP:         return SpriteId::POWERUP_HP;
        case data::PowerupId::CLEAR:      return SpriteId::POWERUP_CLEAR;
        case data::PowerupId::CACHE:      return SpriteId::POWERUP_CACHE;
        case data::PowerupId::TOKENS:     return SpriteId::POWERUP_TOKENS;
        case data::PowerupId::SUBAGENT:   return SpriteId::POWERUP_SUBAGENT;
        case data::PowerupId::SKIP_PERMS: return SpriteId::POWERUP_SKIP_PERMS;
        default:                          return SpriteId::POWERUP_HP;
    }
}

SpriteId boss_sprite(data::BossId id) {
    switch (id) {
        case data::BossId::REGEX_TYRANT:     return SpriteId::BOSS_REGEX_TYRANT;
        case data::BossId::CONTEXT_OVERFLOW: return SpriteId::BOSS_CONTEXT_OVERFLOW;
        case data::BossId::HALLUCINATION:    return SpriteId::BOSS_HALLUCINATION;
        case data::BossId::RATE_LIMITER:     return SpriteId::BOSS_RATE_LIMITER;
        case data::BossId::ROGUE_TOOL_CALL:  return SpriteId::BOSS_ROGUE_TOOL_CALL;
        default:                             return SpriteId::BOSS_REGEX_TYRANT;
    }
}

SpriteId pod_sprite(PodMode m) {
    switch (m) {
        case PodMode::LASER:  return SpriteId::POD_LASER;
        case PodMode::SPREAD: return SpriteId::POD_SPREAD;
        case PodMode::PIERCE: return SpriteId::POD_PIERCE;
    }
    return SpriteId::POD_LASER;
}

// ---------- HUD -----------------------------------------------------------

void draw_hud() {
    auto& d = M5.Display;
    const uint16_t bg     = to565(theme::IVORY);
    const uint16_t cream  = to565(theme::DARK);
    const uint16_t orange = to565(theme::ORANGE);
    const uint16_t gray   = to565(theme::MID_GRAY);
    const uint16_t lgray  = to565(theme::LIGHT_GRAY);
    const uint16_t green  = to565(theme::GREEN);

    d.fillRect(0, HUD_Y, theme::SCREEN_W, HUD_H, bg);
    d.drawFastHLine(0, HUD_Y, theme::SCREEN_W, lgray);
    d.drawFastHLine(0, HUD_Y + HUD_H - 1, theme::SCREEN_W, lgray);

    d.setTextSize(1);
    d.setTextDatum(top_left);

    // Diamond "spark" + label
    d.fillRect(4, HUD_Y + 5, 4, 4, orange);
    d.fillRect(3, HUD_Y + 6, 6, 2, orange);
    d.setTextColor(cream, bg);
    d.drawString("Orbit Fighter", 14, HUD_Y + 4);

    // Stage centered
    const char* biome_label = data::biome_config(g_biome).label;
    d.setTextDatum(top_center);
    d.setTextColor(gray, bg);
    char stage[24];
    snprintf(stage, sizeof(stage), "%s L%u", biome_label, (unsigned)g_loop_number);
    d.drawString(stage, theme::SCREEN_W / 2, HUD_Y + 4);

    // HP + score (right)
    char hud[28];
    snprintf(hud, sizeof(hud), "HP %u  TOK %lu",
             (unsigned)g_player.hp, (unsigned long)g_score);
    d.setTextDatum(top_right);
    d.setTextColor(cream, bg);
    d.drawString(hud, theme::SCREEN_W - 4, HUD_Y + 4);
    d.setTextDatum(top_left);

    // Boss HP bar — render as a thin orange bar across the top edge when
    // a boss is engaged. Drawn under the chrome line so the overall HUD
    // remains a single 16 px strip. During the ROGUE_TOOL_CALL split we
    // aggregate the surviving minis' HP into the same bar.
    uint32_t bar_cur = 0, bar_max = 0;
    if (g_boss.active && g_boss.max_hp > 0) {
        bar_cur = g_boss.hp;
        bar_max = g_boss.max_hp;
    } else if (g_split_active) {
        for (int i = 0; i < MAX_MINIS; ++i) {
            bar_cur += g_minis[i].hp;
            bar_max += g_minis[i].max_hp;
        }
    }
    if (bar_max > 0) {
        int bar_x = 14;
        int bar_y = HUD_Y + HUD_H - 3;
        int bar_w = theme::SCREEN_W - 28;
        int filled = (int)((uint32_t)bar_w * bar_cur / bar_max);
        d.fillRect(bar_x, bar_y, bar_w, 2, lgray);
        d.fillRect(bar_x, bar_y, filled, 2, green);
    }

    // RATE_LIMITER throttle indicator: a small coral "[!] throttled" tag
    // tucked into the top-right of the HUD band whenever the active boss
    // is throttling the player.
    if (g_boss.active && g_boss.pattern_id == 3 && g_rate_throttle_active) {
        const uint16_t coral = to565(0x9D4534);
        d.setTextDatum(top_right);
        d.setTextColor(coral, bg);
        d.drawString("[!] throttled", theme::SCREEN_W - 4, HUD_Y + 4);
        d.setTextDatum(top_left);
    }
}

void draw_footer() {
    auto& d = M5.Display;
    const uint16_t bg    = to565(theme::IVORY);
    const uint16_t gray  = to565(theme::MID_GRAY);
    const uint16_t lgray = to565(theme::LIGHT_GRAY);

    d.fillRect(0, FOOTER_Y, theme::SCREEN_W, FOOTER_H, bg);
    d.drawFastHLine(0, FOOTER_Y, theme::SCREEN_W, lgray);

    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(gray, bg);
    d.drawString("[K/space] fire  [L] pod  [esc] pause  [q] quit",
                 4, FOOTER_Y + 3);
}

// ---------- Modal boxes (drawn with primitives — font lacks box chars) ----

void draw_modal_box(int x, int y, int w, int h) {
    auto& d = M5.Display;
    const uint16_t bg    = to565(theme::IVORY);
    const uint16_t cream = to565(theme::DARK);
    const uint16_t lgray = to565(theme::LIGHT_GRAY);
    d.fillRect(x, y, w, h, bg);
    d.drawFastHLine(x + 1, y,           w - 2, cream);
    d.drawFastHLine(x + 1, y + h - 1,   w - 2, cream);
    d.drawFastVLine(x,         y + 1,   h - 2, cream);
    d.drawFastVLine(x + w - 1, y + 1,   h - 2, cream);
    // Rounded corners — single pixel inset (cosmetic)
    d.drawPixel(x, y, bg);
    d.drawPixel(x + w - 1, y, bg);
    d.drawPixel(x, y + h - 1, bg);
    d.drawPixel(x + w - 1, y + h - 1, bg);
    d.drawPixel(x + 1, y + 1, lgray);
    d.drawPixel(x + w - 2, y + 1, lgray);
    d.drawPixel(x + 1, y + h - 2, lgray);
    d.drawPixel(x + w - 2, y + h - 2, lgray);
}

void draw_death_modal() {
    auto& d = M5.Display;
    const uint16_t bg     = to565(theme::IVORY);
    const uint16_t cream  = to565(theme::DARK);
    const uint16_t orange = to565(theme::ORANGE);
    const uint16_t gray   = to565(theme::MID_GRAY);

    int mw = 200, mh = 78;
    int mx = (theme::SCREEN_W - mw) / 2;
    int my = FIELD_Y0 + (FIELD_Y1 - FIELD_Y0 - mh) / 2;
    draw_modal_box(mx, my, mw, mh);

    d.setTextSize(1);
    d.setTextDatum(top_center);
    d.setTextColor(orange, bg);
    d.drawString("[x] Process exited (code: 1)", mx + mw / 2, my + 6);

    d.setTextColor(cream, bg);
    char line[40];
    snprintf(line, sizeof(line), "score: %lu   loop: %u",
             (unsigned long)g_score, (unsigned)g_loop_number);
    d.drawString(line, mx + mw / 2, my + 22);

    d.setTextColor(gray, bg);
    snprintf(line, sizeof(line), "best: %lu (loop %u)",
             (unsigned long)g_best_score, (unsigned)g_best_loop);
    d.drawString(line, mx + mw / 2, my + 36);

    d.setTextColor(cream, bg);
    d.drawString("[r] retry   [esc] menu", mx + mw / 2, my + 56);
    d.setTextDatum(top_left);
}

void draw_pause_modal() {
    auto& d = M5.Display;
    const uint16_t bg     = to565(theme::IVORY);
    const uint16_t cream  = to565(theme::DARK);
    const uint16_t orange = to565(theme::ORANGE);
    const uint16_t gray   = to565(theme::MID_GRAY);

    int mw = 160, mh = 64;
    int mx = (theme::SCREEN_W - mw) / 2;
    int my = FIELD_Y0 + (FIELD_Y1 - FIELD_Y0 - mh) / 2;
    draw_modal_box(mx, my, mw, mh);

    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(gray, bg);
    d.drawString("/paused", mx + 8, my + 6);

    static const char* items[3] = { "/resume", "/restart", "/quit" };
    for (int i = 0; i < 3; ++i) {
        bool sel = (i == g_pause_idx);
        d.setTextColor(sel ? orange : cream, bg);
        char line[32];
        snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", items[i]);
        d.drawString(line, mx + 12, my + 22 + i * 12);
    }
    d.setTextDatum(top_left);
}

void draw_stage_cleared_modal() {
    auto& d = M5.Display;
    const uint16_t bg     = to565(theme::IVORY);
    const uint16_t cream  = to565(theme::DARK);
    const uint16_t orange = to565(theme::ORANGE);
    const uint16_t green  = to565(theme::GREEN);

    int mw = 200, mh = 56;
    int mx = (theme::SCREEN_W - mw) / 2;
    int my = FIELD_Y0 + (FIELD_Y1 - FIELD_Y0 - mh) / 2;
    draw_modal_box(mx, my, mw, mh);

    d.setTextSize(1);
    d.setTextDatum(top_center);
    d.setTextColor(green, bg);
    d.drawString("STAGE CLEARED  *", mx + mw / 2, my + 6);

    const char* biome_label = data::biome_config(g_cleared_biome).label;
    char line[48];
    d.setTextColor(cream, bg);
    snprintf(line, sizeof(line), "%s -> %s",
             biome_label,
             data::biome_config(data::next_biome(g_cleared_biome)).label);
    d.drawString(line, mx + mw / 2, my + 22);

    d.setTextColor(orange, bg);
    snprintf(line, sizeof(line), "+%lu TOK   loop %d",
             (unsigned long)g_cleared_score_gain, (int)g_loop_number);
    d.drawString(line, mx + mw / 2, my + 38);
    d.setTextDatum(top_left);
}

void draw_boss_intro_modal() {
    auto& d = M5.Display;
    const uint16_t bg     = to565(theme::IVORY);
    const uint16_t cream  = to565(theme::DARK);
    const uint16_t orange = to565(theme::ORANGE);
    const uint16_t gray   = to565(theme::MID_GRAY);

    const auto& bs = data::boss_stats(data::biome_config(g_biome).boss);

    int mw = 220, mh = 72;
    int mx = (theme::SCREEN_W - mw) / 2;
    int my = FIELD_Y0 + (FIELD_Y1 - FIELD_Y0 - mh) / 2;
    draw_modal_box(mx, my, mw, mh);

    d.setTextSize(1);
    d.setTextDatum(top_center);
    d.setTextColor(orange, bg);
    d.drawString("BOSS DETECTED", mx + mw / 2, my + 6);

    d.setTextColor(cream, bg);
    char line[48];
    snprintf(line, sizeof(line), "Stage %d: %s",
             (int)g_biome + 1, bs.name);
    d.drawString(line, mx + mw / 2, my + 22);

    d.setTextColor(gray, bg);
    d.drawString(bs.intro_hint, mx + mw / 2, my + 36);

    d.setTextColor(cream, bg);
    d.drawString("[enter] engage   [esc] retreat", mx + mw / 2, my + 54);
    d.setTextDatum(top_left);
}

// ---------- Reset / setup -------------------------------------------------

void reset_pools() {
    for (int i = 0; i < MAX_ENEMIES; ++i) g_enemies[i].active = false;
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) g_pbul[i].active = false;
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) g_ebul[i].active = false;
    for (int i = 0; i < MAX_POWERUPS; ++i) g_pups[i].active = false;
    for (int i = 0; i < MAX_ENEMIES; ++i) g_enemies[i].prev_drawn = false;
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) g_pbul[i].prev_drawn = false;
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) g_ebul[i].prev_drawn = false;
    for (int i = 0; i < MAX_POWERUPS; ++i) g_pups[i].prev_drawn = false;
    for (int i = 0; i < MAX_MINIS; ++i) {
        g_minis[i].active = false;
        g_minis[i].prev_drawn = false;
    }
    for (int i = 0; i < MAX_FLOATERS; ++i) {
        g_floaters[i].active = false;
        g_floaters[i].prev_drawn = false;
    }
    g_split_active = false;
    g_rate_throttle_active   = false;
    g_rate_throttle_until_ms = 0;
    g_rate_next_throttle_ms  = 0;
    g_hallu_flash_until_ms   = 0;
    g_pending.active = false;
    g_boss.active = false;
    g_boss.prev_drawn = false;
    g_boss_intro_seen = false;
}

void start_biome(data::BiomeId b, bool reset_score) {
    g_biome = b;
    g_biome_start_ms = millis();
    g_next_spawn_idx = 0;
    g_pending.active = false;
    g_boss_intro_seen = false;
    if (reset_score) g_score = 0;
    bg::set_biome((int)b);
    bg::clear_field();
    g_hud_dirty = true;
}

void reset_game() {
    // Drain every queued SFX before the new run starts. Without this the
    // death-sound queue (and any explosions still in flight) keep playing
    // through the first second of the next run and stomp on hit/fire blips.
    sfx::stop_all();

    g_player.x = FIELD_X0 + 8;
    g_player.y = (FIELD_Y0 + FIELD_Y1) / 2 - PLAYER_H / 2;
    g_player.prev_x = g_player.x;
    g_player.prev_y = g_player.y;
    g_player.hp = 5;
    g_player.active = true;
    g_player.charging = false;
    g_player.charge_started_ms = 0;
    g_player.charge_visual_level = 0;
    g_player.free_charges = 0;
    g_player.invuln_until_ms = 0;
    g_player.shield_drawn_prev = false;
    g_player.shield_prev_cx = 0;
    g_player.shield_prev_cy = 0;
    g_player.subagent_until_ms = 0;
    g_player.subagent_last_fire_ms = 0;
    g_player.sub_prev_drawn = false;

    g_pod.active = true;
    g_pod.attached = true;
    g_pod.mode = PodMode::LASER;
    g_pod.orbit_angle = 0.0f;
    g_pod.last_fire_ms = 0;
    g_pod.detach_started_ms = 0;
    g_pod.skip_perms_until_ms = 0;
    g_pod.prev_drawn = false;
    g_pod.x = g_player.x + 18;
    g_pod.y = g_player.y;
    g_pod.prev_x = g_pod.x;
    g_pod.prev_y = g_pod.y;

    g_score = 0;
    g_loop_number = 0;
    g_last_fire_ms = 0;
    g_last_frame_ms = 0;

    reset_pools();
    g_mode = Mode::PLAYING;
    g_modal_drawn = false;
    g_hud_dirty = true;
    g_death_at_ms = 0;

    // Hard-reset biome state to STDOUT. Previously this only reset some
    // fields, which left the boss in-flight state hanging on retry — the
    // user could die mid-boss-fight and respawn straight back at the boss.
    g_boss.active = false;
    g_boss.prev_drawn = false;
    g_boss_intro_seen = false;
    g_split_active = false;
    g_rate_throttle_active = false;
    g_pending.active = false;

    bg::enter((int)data::BiomeId::STDOUT);
    g_biome = data::BiomeId::STDOUT;
    g_biome_start_ms = millis();
    g_next_spawn_idx = 0;

    draw_hud();
    draw_footer();
}

// ---------- Spawning ------------------------------------------------------

Enemy* spawn_enemy(data::EnemyId id, int16_t x, int16_t y) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (g_enemies[i].active) continue;
        Enemy& e = g_enemies[i];
        const auto& s = data::enemy_stats(id);
        int mult = data::loop_difficulty_multiplier(g_loop_number);
        e.id = id;
        e.x = x; e.y = y;
        e.prev_x = x; e.prev_y = y;
        e.vx = s.speed_x;
        e.vy = s.speed_y;
        // Scale speed (magnitude) by loop multiplier — clamp so we don't
        // overflow the int8_t.
        int sx = (int)s.speed_x * mult / 100;
        if (sx > 127) sx = 127; if (sx < -127) sx = -127;
        e.vx = (int8_t)sx;
        int hp = (int)s.max_hp * mult / 100;
        if (hp < 1) hp = 1;
        e.hp = (uint16_t)hp;
        e.score_value = s.score_value;
        e.shoot_interval_ms = s.shoot_interval_ms;
        e.can_shoot = s.can_shoot;
        e.bullet_speed = s.bullet_speed_px_per_frame;
        e.pattern = s.movement_pattern;
        e.age_ms = 0;
        e.spawn_y = y;
        e.last_shot_ms = millis();
        e.active = true;
        e.prev_drawn = false;
        // Cloak enemies start invisible until close.
        e.visible = (e.pattern != 4);
        return &e;
    }
    return nullptr;
}

void schedule_group(const data::EnemySpawnEntry& entry, uint32_t now_ms) {
    g_pending.active   = true;
    g_pending.enemy    = entry.enemy;
    g_pending.remaining= entry.group_size > 0 ? entry.group_size : 1;
    g_pending.total    = g_pending.remaining;
    g_pending.spacing_ms = entry.group_spacing_ms;
    g_pending.next_at_ms = now_ms;
    g_pending.y        = entry.y;
    g_pending.vshape_step = 0;
}

void tick_spawner(uint32_t now_ms) {
    const auto& cfg = data::biome_config(g_biome);
    uint32_t t = now_ms - g_biome_start_ms;

    // Pump scheduled waves into the pending group.
    while (g_next_spawn_idx < cfg.spawn_count && !g_pending.active) {
        const auto& entry = cfg.spawns[g_next_spawn_idx];
        if (t < entry.at_ms) break;
        schedule_group(entry, now_ms);
        g_next_spawn_idx++;
    }

    if (g_pending.active && (int32_t)(now_ms - g_pending.next_at_ms) >= 0) {
        // Pick y — random or fixed.
        int y = (g_pending.y < 0) ? random_spawn_y() : g_pending.y;

        // Apply pattern-specific shape:
        // - swarm V (pattern 3) is set by BRACE_SWARM but we honor it
        //   regardless. Members stagger vertically into a V.
        Enemy* e = nullptr;
        const auto& stats = data::enemy_stats(g_pending.enemy);
        if (stats.movement_pattern == 3) {
            int member = g_pending.total - g_pending.remaining;
            int half = g_pending.total / 2;
            int dy = (member - half) * 4;
            e = spawn_enemy(g_pending.enemy, FIELD_X1 - 4, y + dy);
        } else {
            e = spawn_enemy(g_pending.enemy, FIELD_X1 - 4, y);
        }
        (void)e;
        g_pending.remaining--;
        if (g_pending.remaining == 0) {
            g_pending.active = false;
        } else {
            g_pending.next_at_ms = now_ms + g_pending.spacing_ms;
        }
    }
}

// ---------- Bullets -------------------------------------------------------

Bullet* alloc_player_bullet() {
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) {
        if (!g_pbul[i].active) return &g_pbul[i];
    }
    return nullptr;
}

Bullet* alloc_enemy_bullet() {
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) {
        if (!g_ebul[i].active) return &g_ebul[i];
    }
    return nullptr;
}

void fire_player_normal() {
    Bullet* b = alloc_player_bullet();
    if (!b) return;
    // "Power bullets" while the cache-hit shield is active: bigger BULLET_CHARGED
    // sprite (8x4 yellow), 2x damage, slightly faster. Visual cue that the
    // pickup buffs you OFFENSIVELY as well as defensively.
    bool shielded = (millis() < g_player.invuln_until_ms);
    b->x = g_player.x + PLAYER_W;
    b->y = g_player.y + PLAYER_H / 2 - (shielded ? 2 : 1);
    b->prev_x = b->x; b->prev_y = b->y;
    b->vx = shielded ? 6 : 5;
    b->vy = 0;
    b->damage = shielded ? 2 : 1;
    b->active = true;
    b->prev_drawn = false;
    b->is_charged = shielded;       // re-uses the bigger yellow sprite + hitbox
    b->is_pierce  = false;
    b->is_enemy   = false;
    b->is_boss    = false;
    sfx::play_fire();
}

void fire_player_charged(uint8_t level) {
    Bullet* b = alloc_player_bullet();
    if (!b) return;
    b->x = g_player.x + PLAYER_W;
    b->y = g_player.y + PLAYER_H / 2 - 2;
    b->prev_x = b->x; b->prev_y = b->y;
    b->vx = 6; b->vy = 0;
    b->damage = (level >= 3) ? 3 : (level == 2 ? 2 : 2);
    b->active = true;
    b->prev_drawn = false;
    b->is_charged = true;
    b->is_pierce  = (level >= 2);
    b->is_enemy   = false;
    b->is_boss    = false;
    sfx::play_charge_release();
}

void fire_pod_bullet(int16_t x, int16_t y, int8_t vx, int8_t vy, bool pierce) {
    Bullet* b = alloc_player_bullet();
    if (!b) return;
    b->x = x; b->y = y;
    b->prev_x = x; b->prev_y = y;
    b->vx = vx; b->vy = vy;
    b->damage = 1;
    b->active = true;
    b->prev_drawn = false;
    b->is_charged = false;
    b->is_pierce  = pierce;
    b->is_enemy   = false;
    b->is_boss    = false;
}

void enemy_shoot_at_player(Enemy& e) {
    Bullet* b = alloc_enemy_bullet();
    if (!b) return;
    int sw = sprite_w(enemy_sprite(e.id));
    int sh = sprite_h(enemy_sprite(e.id));
    int sx = e.x;
    int sy = e.y + sh / 2;
    int dx_ = (g_player.x + PLAYER_W / 2) - sx;
    int dy_ = (g_player.y + PLAYER_H / 2) - sy;
    float len = sqrtf((float)(dx_ * dx_ + dy_ * dy_));
    if (len < 1.0f) len = 1.0f;
    int spd = e.bullet_speed > 0 ? e.bullet_speed : 3;
    b->vx = (int8_t)((dx_ / len) * spd);
    b->vy = (int8_t)((dy_ / len) * spd);
    if (b->vx == 0 && b->vy == 0) b->vx = -spd;
    b->x = sx; b->y = sy;
    b->prev_x = sx; b->prev_y = sy;
    b->damage = 1;
    b->active = true;
    b->prev_drawn = false;
    b->is_charged = false;
    b->is_pierce  = false;
    b->is_enemy   = true;
    b->is_boss    = false;
    (void)sw;
}

// ---------- Powerups ------------------------------------------------------

void maybe_drop_powerup(int16_t x, int16_t y) {
    // 8% chance.
    if ((rand() % 100) >= 8) return;
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        if (g_pups[i].active) continue;
        data::PowerupId pick = data::pick_random_powerup();
        // CLEAR was deemed too strong in playtests (one bomb wipes the
        // whole screen); halve its effective drop rate by re-rolling 50%
        // of the time. Anything else passes through unchanged.
        if (pick == data::PowerupId::CLEAR && (rand() % 2 == 0)) {
            pick = data::pick_random_powerup();
            // If the re-roll happens to be CLEAR again, accept it — the
            // expected rate is still halved on average.
        }
        g_pups[i].id = pick;
        g_pups[i].x = x; g_pups[i].y = y;
        g_pups[i].prev_x = x; g_pups[i].prev_y = y;
        g_pups[i].spawn_ms = millis();
        g_pups[i].active = true;
        g_pups[i].prev_drawn = false;
        sfx::play_powerup_spawn();
        return;
    }
}

// Pop a floating pickup label above the player. Cheap pool-of-4; if it's
// full we silently drop the request — the SFX still fires.
void spawn_floater(const char* text) {
    if (!text) return;
    for (int i = 0; i < MAX_FLOATERS; ++i) {
        if (g_floaters[i].active) continue;
        g_floaters[i].active = true;
        g_floaters[i].prev_drawn = false;
        g_floaters[i].text = text;
        g_floaters[i].x = g_player.x + PLAYER_W / 2;
        g_floaters[i].y = g_player.y - 2;
        g_floaters[i].spawn_y = g_floaters[i].y;
        g_floaters[i].prev_x = g_floaters[i].x;
        g_floaters[i].prev_y = g_floaters[i].y;
        g_floaters[i].spawn_ms = millis();
        return;
    }
}

void apply_powerup(data::PowerupId id) {
    uint32_t now = millis();
    sfx::play_pickup();
    // Floating-label text per powerup (must outlive this function — string
    // literals do).
    const char* label = nullptr;
    switch (id) {
        case data::PowerupId::HP:         label = "+hp";          break;
        case data::PowerupId::CLEAR:      label = "/clear";       break;
        case data::PowerupId::CACHE:      label = "cache hit";    break;
        case data::PowerupId::TOKENS:     label = "++max_tokens"; break;
        case data::PowerupId::SUBAGENT:   label = "subagent";     break;
        case data::PowerupId::SKIP_PERMS: label = "/skip-perms";  break;
        default: break;
    }
    spawn_floater(label);
    switch (id) {
        case data::PowerupId::HP:
            if (g_player.hp < 5) g_player.hp++;
            g_hud_dirty = true;
            break;
        case data::PowerupId::CLEAR:
            // Bomb: kill every enemy + clear all enemy bullets.
            for (int i = 0; i < MAX_ENEMIES; ++i) {
                if (!g_enemies[i].active) continue;
                Enemy& e = g_enemies[i];
                g_score += e.score_value;
                bg::fx_spawn_explosion(e.x + 8, e.y + 8, 0);
                if (e.prev_drawn) {
                    int sw = sprite_w(enemy_sprite(e.id));
                    int sh = sprite_h(enemy_sprite(e.id));
                    erase_rect(e.prev_x, e.prev_y, sw, sh);
                }
                e.active = false;
                e.prev_drawn = false;
            }
            for (int i = 0; i < MAX_ENEMY_BULLET; ++i) {
                if (g_ebul[i].active && g_ebul[i].prev_drawn) {
                    erase_rect(g_ebul[i].prev_x, g_ebul[i].prev_y, 4, 3);
                }
                g_ebul[i].active = false;
                g_ebul[i].prev_drawn = false;
            }
            sfx::play_bomb();
            g_hud_dirty = true;
            break;
        case data::PowerupId::CACHE:
            g_player.invuln_until_ms = now + 3000;
            break;
        case data::PowerupId::TOKENS:
            g_player.free_charges = 3;
            break;
        case data::PowerupId::SUBAGENT:
            g_player.subagent_until_ms = now + 5000;
            break;
        case data::PowerupId::SKIP_PERMS: {
            // SKIP_PERMS detaches the pod into auto-fire mode for 8 s, or
            // 4 s if picked up while a boss is actively engaged (the
            // duration would otherwise trivialize the fight).
            bool boss_engaged = (g_boss.active && !g_boss.entering) ||
                                g_split_active;
            uint32_t dur = boss_engaged ? 4000u : 8000u;
            g_pod.attached = false;
            g_pod.skip_perms_until_ms = now + dur;
            g_pod.detach_started_ms = now;
            break;
        }
        default: break;
    }
}

// ---------- Boss ---------------------------------------------------------

void spawn_boss() {
    const auto& cfg = data::biome_config(g_biome);
    const auto& bs  = data::boss_stats(cfg.boss);
    int mult = data::loop_difficulty_multiplier(g_loop_number);
    int hp = (int)bs.max_hp * mult / 100;
    if (hp < 1) hp = 1;
    g_boss.id = cfg.boss;
    g_boss.pattern_id = bs.pattern_id;
    g_boss.hp = (uint16_t)hp;
    g_boss.max_hp = (uint16_t)hp;
    g_boss.score_value = bs.score_value;
    g_boss.entering = true;
    g_boss.active = true;
    g_boss.prev_drawn = false;
    SpriteId sp = boss_sprite(cfg.boss);
    int sw = sprite_w(sp);
    int sh = sprite_h(sp);
    // Slide in from off-screen right.
    g_boss.x = theme::SCREEN_W;
    g_boss.y = (FIELD_Y0 + FIELD_Y1) / 2 - sh / 2;
    if (g_boss.y < FIELD_Y0) g_boss.y = FIELD_Y0;
    g_boss.prev_x = g_boss.x;
    g_boss.prev_y = g_boss.y;
    g_boss.spawn_ms = millis();
    g_boss.last_volley_ms = millis();
    g_boss.sweep_dir = 1;
    g_boss.sweep_y = FIELD_Y0 + 4;
    (void)sw;

    // Per-pattern init.
    uint32_t now = millis();
    g_rate_throttle_active   = false;
    g_rate_throttle_until_ms = 0;
    // First throttle window 4–6 s after the boss anchors.
    g_rate_next_throttle_ms  = now + 4000 + (rand() % 2000);

    g_hallu_real_idx       = rand() % 3;
    g_hallu_last_rotate_ms = now;
    g_hallu_last_fire_ms   = now;
    g_hallu_flash_until_ms = 0;

    g_split_active = false;
    for (int i = 0; i < MAX_MINIS; ++i) g_minis[i].active = false;

    g_hud_dirty = true;
}

void boss_fire_regex_wall() {
    // Pattern 0: a horizontal wall of 6 bullets at the current sweep_y.
    int spacing = 8;
    int start_x = g_boss.x - 8;
    for (int i = 0; i < 6; ++i) {
        Bullet* b = alloc_enemy_bullet();
        if (!b) break;
        b->x = start_x;
        b->y = g_boss.sweep_y + i * spacing;
        if (b->y > FIELD_Y1 - 4) b->y = FIELD_Y1 - 4;
        b->prev_x = b->x; b->prev_y = b->y;
        b->vx = -3;
        b->vy = 0;
        b->damage = 1;
        b->active = true;
        b->prev_drawn = false;
        b->is_charged = false;
        b->is_pierce  = false;
        b->is_enemy   = true;
        b->is_boss    = true;
    }
}

// Spawn an enemy bullet at (sx, sy) aimed at the player's center at `spd`
// px/frame. Returns true on success.
bool boss_fire_aimed(int sx, int sy, int spd) {
    Bullet* b = alloc_enemy_bullet();
    if (!b) return false;
    int dx_ = (g_player.x + PLAYER_W / 2) - sx;
    int dy_ = (g_player.y + PLAYER_H / 2) - sy;
    float len = sqrtf((float)(dx_ * dx_ + dy_ * dy_));
    if (len < 1.0f) len = 1.0f;
    b->vx = (int8_t)((dx_ / len) * spd);
    b->vy = (int8_t)((dy_ / len) * spd);
    if (b->vx == 0 && b->vy == 0) b->vx = (int8_t)-spd;
    b->x = sx; b->y = sy;
    b->prev_x = sx; b->prev_y = sy;
    b->damage = 1;
    b->active = true;
    b->prev_drawn = false;
    b->is_charged = false;
    b->is_pierce  = false;
    b->is_enemy   = true;
    b->is_boss    = true;
    return true;
}

// ---- RATE_LIMITER (pattern_id=3) ----------------------------------------
// Idle 4-6 s, then a 2 s "throttle" window where the boss is invulnerable,
// the player's fire cooldown / move-gate are doubled, and a 3-bullet aimed
// spread is dropped on the player.
void boss_tick_rate_limiter(uint32_t now) {
    if (g_rate_throttle_active) {
        if ((int32_t)(now - g_rate_throttle_until_ms) >= 0) {
            g_rate_throttle_active = false;
            // Next idle window: 4–6 s.
            g_rate_next_throttle_ms = now + 4000 + (rand() % 2000);
            g_hud_dirty = true;
        } else if (now - g_boss.last_volley_ms >= 600) {
            // 3-bullet aimed spread, ~600 ms cadence during the window.
            g_boss.last_volley_ms = now;
            SpriteId sp = boss_sprite(g_boss.id);
            int sh = sprite_h(sp);
            int sx = g_boss.x;
            int sy = g_boss.y + sh / 2;
            // Center shot + slightly offset duplicates for a spread feel.
            boss_fire_aimed(sx, sy, 3);
            boss_fire_aimed(sx, sy - 6, 3);
            boss_fire_aimed(sx, sy + 6, 3);
        }
    } else if ((int32_t)(now - g_rate_next_throttle_ms) >= 0) {
        g_rate_throttle_active   = true;
        g_rate_throttle_until_ms = now + 2000;
        g_boss.last_volley_ms    = now;   // fire immediately on entry
        g_hud_dirty = true;
    }
}

// ---- HALLUCINATION (pattern_id=2) ---------------------------------------
// All three clones fire a single aimed bullet every 1200 ms; the "real"
// clone rotates every 2500 ms. Movement is a slow vertical bob.
void boss_tick_hallucination(uint32_t now) {
    if (now - g_hallu_last_rotate_ms >= 2500) {
        g_hallu_last_rotate_ms = now;
        // Pick a new index that isn't the current one so the player sees
        // an actual change.
        int next = (g_hallu_real_idx + 1 + (rand() % 2)) % 3;
        g_hallu_real_idx = next;
    }
    if (now - g_hallu_last_fire_ms >= 1200) {
        g_hallu_last_fire_ms = now;
        // Each clone is 16x16 inside the 48x16 sprite at offset i*16.
        for (int i = 0; i < 3; ++i) {
            int sx = g_boss.x + i * 16;
            int sy = g_boss.y + 8;
            boss_fire_aimed(sx, sy, 3);
        }
    }
    // Slow bob so the sprite reads as alive. Anchor against the entrance
    // y captured in BossStats — sweep_y is unused for this pattern so we
    // borrow it as a stable anchor on first call.
    if (g_boss.sweep_y == FIELD_Y0 + 4) {
        // First tick after entrance: capture the resting y as anchor.
        g_boss.sweep_y = g_boss.y;
    }
    float s = sinf(now * 0.003f);
    g_boss.y = g_boss.sweep_y + (int)(s * 3.0f);

    // Pod-proximity flash: if pod is detached and within 30 px of the real
    // clone, briefly mark it. The render layer reads g_hallu_flash_until_ms.
    if (g_pod.active && !g_pod.attached) {
        int rcx = g_boss.x + g_hallu_real_idx * 16 + 8;
        int rcy = g_boss.y + 8;
        int dx = (g_pod.x + 4) - rcx;
        int dy = (g_pod.y + 4) - rcy;
        if (dx * dx + dy * dy <= 30 * 30) {
            g_hallu_flash_until_ms = now + 250;
        }
    }
}

// ---- ROGUE_TOOL_CALL (pattern_id=4) -------------------------------------
// Pre-split, just lobs aimed bullets. The split is performed in the bullet-
// hit code when boss hp drops below 50%; tick handles the post-split minis.
void spawn_minis_from_boss() {
    SpriteId sp = boss_sprite(g_boss.id);
    int sw = sprite_w(sp);
    int sh = sprite_h(sp);
    // Erase the original boss sprite once so it doesn't ghost into the
    // split-pair rendering.
    if (g_boss.prev_drawn) erase_rect(g_boss.prev_x, g_boss.prev_y, sw, sh);
    int cx = g_boss.x + sw / 2;
    int cy = g_boss.y + sh / 2;
    uint16_t mini_hp = (uint16_t)((g_boss.max_hp / 2) > 1 ? g_boss.max_hp / 2 : 1);
    for (int i = 0; i < MAX_MINIS; ++i) {
        g_minis[i].active = true;
        g_minis[i].prev_drawn = false;
        // Half-size bounding box; we render the half-size sprite by stepping
        // every other pixel in the draw helper below.
        g_minis[i].x = cx - sw / 4;     // sw/2 wide => half-size
        g_minis[i].y = cy - sh / 4;
        g_minis[i].prev_x = g_minis[i].x;
        g_minis[i].prev_y = g_minis[i].y;
        g_minis[i].base_y = g_minis[i].y;
        g_minis[i].hp = mini_hp;
        g_minis[i].max_hp = mini_hp;
        g_minis[i].age_ms = 0;
        g_minis[i].last_fire_ms = millis();
        g_minis[i].vy_drift = (i == 0) ? -1 : 1;
    }
    g_split_active = true;
    g_boss.active = false;       // hand the fight over to the minis
    g_boss.prev_drawn = false;
    g_hud_dirty = true;
    sfx::play_boss_hit();
    bg::fx_spawn_explosion(cx, cy, 1);
}

void boss_tick_minis(uint32_t now) {
    if (!g_split_active) return;
    SpriteId sp = boss_sprite(g_boss.id);
    int half_w = sprite_w(sp) / 2;
    int half_h = sprite_h(sp) / 2;
    (void)half_w;

    for (int i = 0; i < MAX_MINIS; ++i) {
        MiniBoss& m = g_minis[i];
        if (!m.active) continue;
        m.prev_x = m.x;
        m.prev_y = m.y;
        m.age_ms += FRAME_INTERVAL_MS;

        // Initial separation: drift apart vertically for ~1.2 s, then
        // sinusoidal bob around the spread position.
        if (m.age_ms < 1200) {
            m.y += m.vy_drift;
            m.base_y = m.y;
        } else {
            float s = sinf(m.age_ms * 0.005f);
            m.y = m.base_y + (int)(s * 8.0f);
        }
        // Clamp to playfield.
        if (m.y < FIELD_Y0 + 2) m.y = FIELD_Y0 + 2;
        if (m.y + half_h > FIELD_Y1 - 2) m.y = FIELD_Y1 - 2 - half_h;

        // Fire every 1500 ms.
        if (now - m.last_fire_ms >= 1500) {
            m.last_fire_ms = now;
            boss_fire_aimed(m.x, m.y + half_h / 2, 3);
        }
    }
}

void tick_boss(uint32_t now) {
    // Mini-boss phase: g_boss isn't active but the split fight is.
    if (g_split_active) {
        boss_tick_minis(now);
        return;
    }
    if (!g_boss.active) return;
    g_boss.prev_x = g_boss.x;
    g_boss.prev_y = g_boss.y;
    SpriteId sp = boss_sprite(g_boss.id);
    int sw = sprite_w(sp);

    // Entrance — slide in until anchored at FIELD_X1 - sw - 4
    int anchor_x = FIELD_X1 - sw - 4;
    if (g_boss.entering) {
        g_boss.x -= 2;
        if (g_boss.x <= anchor_x) {
            g_boss.x = anchor_x;
            g_boss.entering = false;
            // Reset the throttle clock now that the boss can act.
            g_rate_next_throttle_ms = now + 4000 + (rand() % 2000);
            g_hallu_last_rotate_ms  = now;
            g_hallu_last_fire_ms    = now;
        }
        return;
    }

    // Pattern dispatch.
    switch (g_boss.pattern_id) {
        case 0:  // REGEX_TYRANT — sweeping walls of bullets.
            if (now - g_boss.last_volley_ms >= 1500) {
                g_boss.last_volley_ms = now;
                boss_fire_regex_wall();
                g_boss.sweep_y += g_boss.sweep_dir * 12;
                if (g_boss.sweep_y > FIELD_Y1 - 48) g_boss.sweep_dir = -1;
                if (g_boss.sweep_y < FIELD_Y0 + 4)  g_boss.sweep_dir = 1;
            }
            break;
        case 2:  // HALLUCINATION — three clones, only one is real.
            boss_tick_hallucination(now);
            break;
        case 3:  // RATE_LIMITER — throttle window + invuln.
            boss_tick_rate_limiter(now);
            break;
        case 4:  // ROGUE_TOOL_CALL — single body until split.
            if (now - g_boss.last_volley_ms >= 1100) {
                g_boss.last_volley_ms = now;
                int sh = sprite_h(sp);
                boss_fire_aimed(g_boss.x, g_boss.y + sh / 2, 3);
            }
            break;
        case 1:  // CONTEXT_OVERFLOW — generic; the big sprite carries it.
        default:
            if (now - g_boss.last_volley_ms >= 900) {
                g_boss.last_volley_ms = now;
                int sh = sprite_h(sp);
                boss_fire_aimed(g_boss.x, g_boss.y + sh / 2, 3);
            }
            break;
    }
    (void)sw;
}

void on_boss_killed() {
    SpriteId sp = boss_sprite(g_boss.id);
    int sw = sprite_w(sp);
    int sh = sprite_h(sp);
    // Multiple explosions across the boss sprite.
    for (int i = 0; i < 6; ++i) {
        int ex = g_boss.x + (rand() % sw);
        int ey = g_boss.y + (rand() % sh);
        bg::fx_spawn_explosion(ex, ey, 1);
    }
    if (g_boss.prev_drawn) erase_rect(g_boss.prev_x, g_boss.prev_y, sw, sh);
    // Also clean any residual mini-boss draw rects (only set when the
    // ROGUE_TOOL_CALL split actually ran).
    for (int i = 0; i < MAX_MINIS; ++i) {
        MiniBoss& m = g_minis[i];
        if (!m.prev_drawn) continue;
        int hw = sw / 2;
        int hh = sh / 2;
        erase_rect(m.prev_x, m.prev_y, hw, hh);
        m.prev_drawn = false;
    }
    g_split_active = false;
    uint32_t score_gain = g_boss.score_value;
    g_score += score_gain;
    g_boss.active = false;
    g_boss.prev_drawn = false;
    g_rate_throttle_active = false;
    sfx::play_boss_death();

    // Stash the cleared biome + score gain so the STAGE_CLEARED modal can
    // display them. The actual biome transition happens in
    // advance_to_next_biome() after the modal has been on screen long enough.
    g_cleared_biome = g_biome;
    g_cleared_score_gain = score_gain;
    g_stage_cleared_until_ms = millis() + 1800;   // 1.8 s celebration
    g_mode = Mode::STAGE_CLEARED;
    g_modal_drawn = false;
    return;   // skip the immediate biome advance — that runs from tick()
}

// Runs after the STAGE_CLEARED modal expires. This is the second half of
// the old on_boss_killed flow — separated so we can pause for celebration.
void advance_to_next_biome() {
    // Defensive scrub before re-entering PLAYING. The user reported total
    // freezes immediately after a boss kill — most likely candidates are
    // (a) stale bullets / enemies / minis still mid-update logic and (b) a
    // saturated SFX channel queue from the boss-death + level-complete
    // fanfare. Clear both.
    sfx::stop_all();
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (g_enemies[i].prev_drawn) {
            int sw = sprite_w(enemy_sprite(g_enemies[i].id));
            int sh = sprite_h(enemy_sprite(g_enemies[i].id));
            erase_rect(g_enemies[i].prev_x, g_enemies[i].prev_y, sw, sh);
        }
        g_enemies[i].active = false;
        g_enemies[i].prev_drawn = false;
    }
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) g_pbul[i].active = false;
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) g_pbul[i].prev_drawn = false;
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i)  g_ebul[i].active = false;
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i)  g_ebul[i].prev_drawn = false;
    for (int i = 0; i < MAX_MINIS; ++i) {
        g_minis[i].active = false;
        g_minis[i].prev_drawn = false;
    }
    g_split_active = false;
    g_rate_throttle_active   = false;
    g_rate_throttle_until_ms = 0;
    g_rate_next_throttle_ms  = 0;
    g_hallu_flash_until_ms   = 0;

    // Transition to next biome (loop if we wrapped from PROD).
    data::BiomeId next = data::next_biome(g_biome);
    if (next == data::BiomeId::STDOUT && g_biome == data::BiomeId::PROD) {
        g_loop_number++;
    }
    g_biome = next;
    g_biome_start_ms = millis();
    g_next_spawn_idx = 0;
    g_pending.active = false;
    g_boss_intro_seen = false;
    bg::set_biome((int)next);
    bg::clear_field();
    sfx::play_biome_transition();
    sfx::play_level_complete();   // 6-note fanfare layered on top of the sweep
    g_hud_dirty = true;
}

// ---------- Death ---------------------------------------------------------

void player_takes_damage(uint8_t dmg) {
    uint32_t now = millis();
    if (now < g_player.invuln_until_ms) return;
    if (g_player.hp <= dmg) {
        g_player.hp = 0;
        g_player.active = false;
        // Erase last-drawn ship + subagent so they don't linger under the
        // death modal once the explosion FX clear.
        erase_rect(g_player.prev_x, g_player.prev_y, PLAYER_W, PLAYER_H);
        if (g_player.sub_prev_drawn) {
            erase_rect(g_player.sub_prev_x, g_player.sub_prev_y,
                       PLAYER_W, PLAYER_H);
            g_player.sub_prev_drawn = false;
        }
        // Dramatic multi-explosion at the ship — bigger, brighter, louder.
        // 8 staggered spawns across the sprite footprint with mixed kinds:
        //   kind 0 → ORANGE + YELLOW (the warmest burst — most pixels)
        //   kind 2 → YELLOW + cream/white-hot core (peak brightness)
        //   kind 1 → BLUE + ORANGE (sparkler-style sparks for variety)
        // Plus 3 sparkle bursts for the closing flicker.
        for (int e = 0; e < 5; ++e) {
            int ex = g_player.x + (rand() % PLAYER_W);
            int ey = g_player.y + (rand() % PLAYER_H);
            bg::fx_spawn_explosion(ex, ey, 0);     // orange+yellow
        }
        for (int e = 0; e < 2; ++e) {
            int ex = g_player.x + (rand() % PLAYER_W);
            int ey = g_player.y + (rand() % PLAYER_H);
            bg::fx_spawn_explosion(ex, ey, 2);     // yellow+hot core
        }
        bg::fx_spawn_explosion(g_player.x + PLAYER_W / 2,
                               g_player.y + PLAYER_H / 2, 1);  // blue spark
        for (int e = 0; e < 3; ++e) {
            int ex = g_player.x + (rand() % PLAYER_W);
            int ey = g_player.y + (rand() % PLAYER_H);
            bg::fx_spawn_sparkle(ex, ey);
        }
        // Layered audio: deep bass thump (bomb) + ascending explosion blast +
        // the descending death glitch on top.
        sfx::play_bomb();
        sfx::play_explode();
        sfx::play_death();
        if (g_score > g_best_score) {
            g_best_score = g_score;
            g_best_loop = g_loop_number;
            save_highscore();
        }
        g_mode = Mode::DEAD;
        g_modal_drawn = false;
        g_death_at_ms = now;            // delays the retry modal so the
                                         // explosion FX have a moment to play
    } else {
        g_player.hp -= dmg;
        // Brief mercy invincibility so a single rough frame isn't fatal.
        g_player.invuln_until_ms = now + 500;
        sfx::play_player_hit();
        g_hud_dirty = true;
    }
}

// ---------- Update --------------------------------------------------------

void update_enemies(uint32_t now) {
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy& e = g_enemies[i];
        if (!e.active) continue;
        e.prev_x = e.x;
        e.prev_y = e.y;
        e.age_ms += FRAME_INTERVAL_MS;

        switch (e.pattern) {
            case 0:  // linear
                e.x += e.vx;
                e.y += e.vy;
                break;
            case 1: {  // sine_y
                e.x += e.vx;
                float s = sinf(e.age_ms * 0.005f);
                e.y = e.spawn_y + (int16_t)(s * 20.0f);
                break;
            }
            case 2: {  // track_player_slow
                e.x += e.vx;
                int target_y = g_player.y + PLAYER_H / 2;
                if (e.y + 8 < target_y) e.y += 1;
                else if (e.y + 8 > target_y) e.y -= 1;
                break;
            }
            case 3:  // swarm_v_formation — same as linear once spawned
                e.x += e.vx;
                e.y += e.vy;
                break;
            case 4: {  // stalker_cloak
                int dx = (g_player.x + PLAYER_W / 2) - (e.x + 8);
                int dy = (g_player.y + PLAYER_H / 2) - (e.y + 8);
                int dist2 = dx*dx + dy*dy;
                if (dist2 < 60 * 60) {
                    if (!e.visible) {
                        // Becoming visible — bump speed.
                        if (e.vx > -127) e.vx -= 1;
                    }
                    e.visible = true;
                } else {
                    e.visible = false;
                }
                e.x += e.vx;
                e.y += e.vy;
                break;
            }
            default:
                e.x += e.vx;
                break;
        }

        // Despawn if off the left edge or vertically off-screen.
        if (e.x < FIELD_X0 - 20 ||
            e.y < FIELD_Y0 - 20 || e.y > FIELD_Y1 + 20) {
            // Erase prev if drawn
            if (e.prev_drawn) {
                int sw = sprite_w(enemy_sprite(e.id));
                int sh = sprite_h(enemy_sprite(e.id));
                erase_rect(e.prev_x, e.prev_y, sw, sh);
            }
            e.active = false;
            e.prev_drawn = false;
            continue;
        }

        // Shoot
        if (e.can_shoot && e.visible &&
            (now - e.last_shot_ms) > e.shoot_interval_ms) {
            e.last_shot_ms = now;
            enemy_shoot_at_player(e);
        }
    }
}

void update_player_bullets(uint32_t now) {
    (void)now;
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) {
        Bullet& b = g_pbul[i];
        if (!b.active) continue;
        b.prev_x = b.x; b.prev_y = b.y;
        b.x += b.vx;
        b.y += b.vy;
        if (b.x > FIELD_X1 || b.x < FIELD_X0 - 8 ||
            b.y < FIELD_Y0 - 8 || b.y > FIELD_Y1 + 8) {
            b.active = false;
            continue;
        }

        // Bullet vs enemy
        int bw = b.is_charged ? 8 : 4;
        int bh = b.is_charged ? 4 : 2;
        bool consumed = false;
        for (int j = 0; j < MAX_ENEMIES; ++j) {
            Enemy& e = g_enemies[j];
            if (!e.active || !e.visible) continue;
            int sw = sprite_w(enemy_sprite(e.id));
            int sh = sprite_h(enemy_sprite(e.id));
            if (aabb_overlap(b.x, b.y, bw, bh, e.x, e.y, sw, sh)) {
                // Hit
                if (e.hp <= b.damage) {
                    g_score += e.score_value;
                    bg::fx_spawn_explosion(e.x + sw / 2, e.y + sh / 2, 0);
                    if (e.prev_drawn) erase_rect(e.prev_x, e.prev_y, sw, sh);
                    e.active = false;
                    e.prev_drawn = false;
                    sfx::play_explode();
                    maybe_drop_powerup(e.x, e.y);
                    g_hud_dirty = true;
                } else {
                    e.hp -= b.damage;
                    sfx::play_hit();
                }
                if (!b.is_pierce) {
                    consumed = true;
                    break;
                }
            }
        }

        // L3 charge splash damage. Only level-3 charged bullets splash —
        // L1 and L2 just hit a single target (or pierce, for L2). When a
        // pierce L3 plows through a wave, splash also fires per pierce hit.
        if (b.is_charged && b.damage >= 3) {
            int sx = b.x + bw / 2;
            int sy = b.y + bh / 2;
            // Scan for any enemy whose center is within 20 px of impact;
            // skip ones we already nuked above (active==false).
            for (int j = 0; j < MAX_ENEMIES; ++j) {
                Enemy& e = g_enemies[j];
                if (!e.active || !e.visible) continue;
                int ew = sprite_w(enemy_sprite(e.id));
                int eh = sprite_h(enemy_sprite(e.id));
                int dx = (e.x + ew / 2) - sx;
                int dy = (e.y + eh / 2) - sy;
                if (dx * dx + dy * dy > 20 * 20) continue;
                // Don't double-count the primary target if it was already
                // consumed (active==false above) — that's already handled.
                if (e.hp <= 1) {
                    g_score += e.score_value;
                    bg::fx_spawn_explosion(e.x + ew / 2, e.y + eh / 2, 0);
                    if (e.prev_drawn) erase_rect(e.prev_x, e.prev_y, ew, eh);
                    e.active = false;
                    e.prev_drawn = false;
                    maybe_drop_powerup(e.x, e.y);
                    g_hud_dirty = true;
                } else {
                    e.hp -= 1;
                }
            }
        }

        // Bullet vs hallucination clones (only the "real" one takes damage).
        if (!consumed && g_boss.active && !g_boss.entering &&
            g_boss.pattern_id == 2) {
            SpriteId sp = boss_sprite(g_boss.id);
            int sh = sprite_h(sp);
            for (int ci = 0; ci < 3; ++ci) {
                int cx = g_boss.x + ci * 16;
                if (!aabb_overlap(b.x, b.y, bw, bh, cx, g_boss.y, 16, sh)) {
                    continue;
                }
                if (ci == g_hallu_real_idx) {
                    if (g_boss.hp <= b.damage) {
                        g_boss.hp = 0;
                        on_boss_killed();
                        consumed = true;
                    } else {
                        g_boss.hp -= b.damage;
                        bg::fx_spawn_explosion(b.x, b.y, 1);
                        sfx::play_boss_hit();
                        g_hud_dirty = true;
                        if (!b.is_pierce) consumed = true;
                    }
                } else {
                    // Decoy clone — bullet passes through with a small fizzle.
                    bg::fx_spawn_sparkle(b.x, b.y);
                    if (!b.is_pierce) consumed = true;
                }
                break;
            }
        }
        // Bullet vs boss (generic + ROGUE_TOOL_CALL pre-split).
        else if (!consumed && g_boss.active && !g_boss.entering) {
            SpriteId sp = boss_sprite(g_boss.id);
            int sw = sprite_w(sp);
            int sh = sprite_h(sp);
            // RATE_LIMITER throttle window — invulnerable to bullets.
            bool invuln_throttle = (g_boss.pattern_id == 3) &&
                                   g_rate_throttle_active;
            if (aabb_overlap(b.x, b.y, bw, bh, g_boss.x, g_boss.y, sw, sh)) {
                if (invuln_throttle) {
                    bg::fx_spawn_sparkle(b.x, b.y);
                    if (!b.is_pierce) consumed = true;
                } else if (g_boss.hp <= b.damage) {
                    g_boss.hp = 0;
                    on_boss_killed();
                    consumed = true;
                } else {
                    g_boss.hp -= b.damage;
                    bg::fx_spawn_explosion(b.x, b.y, 1);
                    sfx::play_boss_hit();
                    g_hud_dirty = true;
                    // ROGUE_TOOL_CALL: when HP crosses 50%, split into minis.
                    if (g_boss.pattern_id == 4 &&
                        g_boss.hp * 2 < g_boss.max_hp) {
                        spawn_minis_from_boss();
                        consumed = true;
                    } else if (!b.is_pierce) {
                        consumed = true;
                    }
                }
            }
        }

        // Bullet vs split mini-bosses.
        if (!consumed && g_split_active) {
            SpriteId sp = boss_sprite(g_boss.id);
            int half_w = sprite_w(sp) / 2;
            int half_h = sprite_h(sp) / 2;
            for (int mi = 0; mi < MAX_MINIS; ++mi) {
                MiniBoss& m = g_minis[mi];
                if (!m.active) continue;
                if (!aabb_overlap(b.x, b.y, bw, bh, m.x, m.y, half_w, half_h)) {
                    continue;
                }
                if (m.hp <= b.damage) {
                    m.hp = 0;
                    m.active = false;
                    bg::fx_spawn_explosion(m.x + half_w / 2,
                                           m.y + half_h / 2, 1);
                    if (m.prev_drawn) {
                        erase_rect(m.prev_x, m.prev_y, half_w, half_h);
                        m.prev_drawn = false;
                    }
                    sfx::play_boss_hit();
                    // If both minis are now dead, run the standard kill flow
                    // (biome transition, score, etc.) via on_boss_killed —
                    // but it expects g_boss state, which we already cleared.
                    // Re-arm a synthetic g_boss just long enough for it to
                    // emit explosions and rotate the biome.
                    bool both_dead = true;
                    for (int j2 = 0; j2 < MAX_MINIS; ++j2) {
                        if (g_minis[j2].active) { both_dead = false; break; }
                    }
                    if (both_dead) {
                        g_split_active = false;
                        // on_boss_killed reads g_boss.x/y/score/id — those
                        // still hold the pre-split values (we kept the boss
                        // struct intact when handing off to the minis).
                        g_boss.active = true;       // briefly, so kill flow runs
                        on_boss_killed();
                    }
                } else {
                    m.hp -= b.damage;
                    bg::fx_spawn_explosion(b.x, b.y, 0);
                    sfx::play_boss_hit();
                    g_hud_dirty = true;
                }
                if (!b.is_pierce) consumed = true;
                break;
            }
        }

        if (consumed) {
            b.active = false;
        }
    }
}

void update_enemy_bullets(uint32_t /*now*/) {
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) {
        Bullet& b = g_ebul[i];
        if (!b.active) continue;
        b.prev_x = b.x; b.prev_y = b.y;
        b.x += b.vx;
        b.y += b.vy;
        if (b.x < FIELD_X0 - 8 || b.x > FIELD_X1 + 8 ||
            b.y < FIELD_Y0 - 8 || b.y > FIELD_Y1 + 8) {
            b.active = false;
            continue;
        }
        // Vs player
        // Enemy-bullet hitbox matches the new 4x3 sprite (boss bullets are
        // 6x4 — we conservatively use the smaller box for non-boss here so
        // grazing the edge of a bullet feels lenient).
        int ebw = b.is_boss ? 6 : 4;
        int ebh = b.is_boss ? 4 : 3;
        if (g_player.active &&
            aabb_overlap(b.x, b.y, ebw, ebh,
                         g_player.x, g_player.y, PLAYER_W, PLAYER_H)) {
            b.active = false;
            player_takes_damage(b.damage);
        }
    }
}

void update_powerups(uint32_t /*now*/) {
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        Powerup& p = g_pups[i];
        if (!p.active) continue;
        p.prev_x = p.x; p.prev_y = p.y;
        p.x -= 1;
        if (p.x < FIELD_X0 - 16) {
            p.active = false;
            continue;
        }
        // Pickup
        if (g_player.active &&
            aabb_overlap(p.x, p.y, 12, 12,
                         g_player.x, g_player.y, PLAYER_W, PLAYER_H)) {
            bg::fx_spawn_sparkle(p.x + 6, p.y + 6);
            apply_powerup(p.id);
            p.active = false;
        }
    }
}

void update_floaters(uint32_t now) {
    for (int i = 0; i < MAX_FLOATERS; ++i) {
        Floater& f = g_floaters[i];
        if (!f.active) continue;
        f.prev_x = f.x;
        f.prev_y = f.y;
        uint32_t age = now - f.spawn_ms;
        if (age >= FLOATER_LIFETIME_MS) {
            f.active = false;
            continue;
        }
        // Linear upward drift anchored at spawn_y (deterministic per age,
        // no integration drift).
        int dy = (int)((int32_t)age * FLOATER_DRIFT_PX /
                       (int32_t)FLOATER_LIFETIME_MS);
        f.y = f.spawn_y - (int16_t)dy;
        // Don't let the label punch into the HUD band; clamp to top of
        // playfield so a pickup near the top still drifts visibly.
        if (f.y < FIELD_Y0) f.y = FIELD_Y0;
    }
}

void update_pod(uint32_t now) {
    if (!g_pod.active) return;
    g_pod.prev_x = g_pod.x;
    g_pod.prev_y = g_pod.y;
    if (g_pod.attached) {
        g_pod.orbit_angle += 0.12f;
        float r = 14.0f;
        int cx = g_player.x + PLAYER_W / 2;
        int cy = g_player.y + PLAYER_H / 2;
        g_pod.x = (int16_t)(cx + cosf(g_pod.orbit_angle) * r) - 4;
        g_pod.y = (int16_t)(cy + sinf(g_pod.orbit_angle) * r) - 4;
        // Clamp pod into the playfield. Without this, the orbit phase that
        // sits above the player can paint into the HUD strip (y < FIELD_Y0)
        // when the player flies up against the top edge.
        if (g_pod.y < FIELD_Y0) g_pod.y = FIELD_Y0;
        if (g_pod.y + 8 > FIELD_Y1) g_pod.y = FIELD_Y1 - 8;
        if (g_pod.x < FIELD_X0) g_pod.x = FIELD_X0;
        if (g_pod.x + 8 > FIELD_X1) g_pod.x = FIELD_X1 - 8;
    } else {
        // Detached: fly right, auto-fire, recall after 1 s (or 8 s in SKIP).
        g_pod.x += 4;
        if (g_pod.x > FIELD_X1) g_pod.x = FIELD_X1 - 8;
        uint32_t detach_window = (g_pod.skip_perms_until_ms > now) ? 8000 : 1000;
        if (now - g_pod.detach_started_ms > detach_window) {
            g_pod.attached = true;
        }
    }

    // Auto-fire when in front of player or detached.
    bool in_front = g_pod.x > g_player.x + PLAYER_W;
    uint32_t fire_ivl = (g_pod.skip_perms_until_ms > now) ? 100 : 250;
    if ((in_front || !g_pod.attached) && (now - g_pod.last_fire_ms) > fire_ivl) {
        g_pod.last_fire_ms = now;
        sfx::play_pod_fire();
        int px = g_pod.x + 8;
        int py = g_pod.y + 3;
        switch (g_pod.mode) {
            case PodMode::LASER:
                fire_pod_bullet(px, py, 5, 0, false);
                break;
            case PodMode::SPREAD:
                fire_pod_bullet(px, py,     5,  0, false);
                fire_pod_bullet(px, py - 1, 5, -1, false);
                fire_pod_bullet(px, py + 1, 5,  1, false);
                break;
            case PodMode::PIERCE:
                fire_pod_bullet(px, py, 5, 0, true);
                break;
        }
    }
}

void update_player(uint32_t now) {
    // Enemy/player collision
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy& e = g_enemies[i];
        if (!e.active || !e.visible) continue;
        int sw = sprite_w(enemy_sprite(e.id));
        int sh = sprite_h(enemy_sprite(e.id));
        if (aabb_overlap(e.x, e.y, sw, sh,
                         g_player.x, g_player.y, PLAYER_W, PLAYER_H)) {
            bg::fx_spawn_explosion(e.x + sw / 2, e.y + sh / 2, 0);
            if (e.prev_drawn) erase_rect(e.prev_x, e.prev_y, sw, sh);
            e.active = false;
            e.prev_drawn = false;
            player_takes_damage(1);
        }
    }

    // Mini-boss vs player collision (post-split phase).
    if (g_split_active) {
        SpriteId sp = boss_sprite(g_boss.id);
        int half_w = sprite_w(sp) / 2;
        int half_h = sprite_h(sp) / 2;
        for (int i = 0; i < MAX_MINIS; ++i) {
            MiniBoss& m = g_minis[i];
            if (!m.active) continue;
            if (aabb_overlap(m.x, m.y, half_w, half_h,
                             g_player.x, g_player.y, PLAYER_W, PLAYER_H)) {
                player_takes_damage(1);
            }
        }
    }

    // Subagent auto-fire
    if (now < g_player.subagent_until_ms) {
        if (now - g_player.subagent_last_fire_ms > 200) {
            g_player.subagent_last_fire_ms = now;
            Bullet* b = alloc_player_bullet();
            if (b) {
                b->x = g_player.x + PLAYER_W;
                b->y = g_player.y - 8;
                if (b->y < FIELD_Y0) b->y = FIELD_Y0;
                b->prev_x = b->x; b->prev_y = b->y;
                b->vx = 5; b->vy = 0;
                b->damage = 1;
                b->active = true;
                b->prev_drawn = false;
                b->is_charged = false;
                b->is_pierce  = false;
                b->is_enemy   = false;
                b->is_boss    = false;
            }
        }
    }
}

// ---------- Render --------------------------------------------------------

SpriteId player_current_sprite() {
    switch (g_player.charge_visual_level) {
        case 1:  return SpriteId::PLAYER_CHARGE_L1;
        case 2:  return SpriteId::PLAYER_CHARGE_L2;
        case 3:  return SpriteId::PLAYER_CHARGE_L3;
        default: return SpriteId::PLAYER;
    }
}

void render_entities(uint32_t now) {
    auto& d = M5.Display;

    // Clip every entity erase + draw to the playfield rect. Without this,
    // a 16-px enemy sprite drifting near the top or bottom edge can spill
    // pixels into the HUD / footer strips (or even the global status bar
    // at y < 14). Cleared at the end of the function so subsequent HUD /
    // status redraws aren't masked.
    d.setClipRect(FIELD_X0, FIELD_Y0,
                  FIELD_X1 - FIELD_X0, FIELD_Y1 - FIELD_Y0);

    // Erase old positions for everything that moved or died.
    if (g_player.prev_x != g_player.x || g_player.prev_y != g_player.y) {
        erase_rect(g_player.prev_x, g_player.prev_y, PLAYER_W, PLAYER_H);
    }
    // Erase last frame's shield ring (it can extend a few px past the sprite).
    // Also fires when the shield expired — the ring pixels would otherwise
    // stay on screen until the player moves.
    if (g_player.shield_drawn_prev) {
        int r = (PLAYER_W / 2) + 2;
        int ex = g_player.shield_prev_cx - r - 1;
        int ey = g_player.shield_prev_cy - r - 1;
        int ew = (r + 1) * 2 + 1;
        int eh = (r + 1) * 2 + 1;
        // Clip to playfield so we don't smudge the HUD strip above.
        if (ey < FIELD_Y0) { eh -= (FIELD_Y0 - ey); ey = FIELD_Y0; }
        if (ex < FIELD_X0) { ew -= (FIELD_X0 - ex); ex = FIELD_X0; }
        if (eh > 0 && ew > 0) erase_rect(ex, ey, ew, eh);
        g_player.shield_drawn_prev = false;
    }
    if (g_pod.active && g_pod.prev_drawn &&
        (g_pod.prev_x != g_pod.x || g_pod.prev_y != g_pod.y)) {
        erase_rect(g_pod.prev_x, g_pod.prev_y, 8, 8);
    }
    if (g_player.sub_prev_drawn && now >= g_player.subagent_until_ms) {
        erase_rect(g_player.sub_prev_x, g_player.sub_prev_y, PLAYER_W, PLAYER_H);
        g_player.sub_prev_drawn = false;
    }
    if (g_boss.active && g_boss.prev_drawn &&
        (g_boss.prev_x != g_boss.x || g_boss.prev_y != g_boss.y)) {
        SpriteId sp = boss_sprite(g_boss.id);
        erase_rect(g_boss.prev_x, g_boss.prev_y, sprite_w(sp), sprite_h(sp));
    }
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy& e = g_enemies[i];
        if (!e.prev_drawn) continue;
        bool need_erase = !e.active || !e.visible ||
                          e.prev_x != e.x || e.prev_y != e.y;
        if (need_erase) {
            int sw = sprite_w(enemy_sprite(e.id));
            int sh = sprite_h(enemy_sprite(e.id));
            erase_rect(e.prev_x, e.prev_y, sw, sh);
            e.prev_drawn = false;
        }
    }
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) {
        Bullet& b = g_pbul[i];
        if (!b.prev_drawn) continue;
        if (!b.active || b.prev_x != b.x || b.prev_y != b.y) {
            int bw = b.is_charged ? 8 : 4;
            int bh = b.is_charged ? 4 : 2;
            erase_rect(b.prev_x, b.prev_y, bw, bh);
            b.prev_drawn = false;
        }
    }
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) {
        Bullet& b = g_ebul[i];
        if (!b.prev_drawn) continue;
        if (!b.active || b.prev_x != b.x || b.prev_y != b.y) {
            int bw = b.is_boss ? 6 : 4;
            int bh = b.is_boss ? 4 : 3;
            erase_rect(b.prev_x, b.prev_y, bw, bh);
            b.prev_drawn = false;
        }
    }
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        Powerup& p = g_pups[i];
        if (!p.prev_drawn) continue;
        if (!p.active || p.prev_x != p.x || p.prev_y != p.y) {
            erase_rect(p.prev_x, p.prev_y, 12, 12);
            p.prev_drawn = false;
        }
    }

    // Draw in order: enemies, bullets, powerups, boss, player + pod.
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Enemy& e = g_enemies[i];
        if (!e.active || !e.visible) continue;
        draw_sprite(e.x, e.y, enemy_sprite(e.id));
        e.prev_drawn = true;
    }
    for (int i = 0; i < MAX_PLAYER_BULLET; ++i) {
        Bullet& b = g_pbul[i];
        if (!b.active) continue;
        SpriteId sp = b.is_charged ? SpriteId::BULLET_CHARGED
                                   : SpriteId::BULLET_PLAYER;
        draw_sprite(b.x, b.y, sp);
        b.prev_drawn = true;
    }
    for (int i = 0; i < MAX_ENEMY_BULLET; ++i) {
        Bullet& b = g_ebul[i];
        if (!b.active) continue;
        SpriteId sp = b.is_boss ? SpriteId::BULLET_BOSS
                                : SpriteId::BULLET_ENEMY;
        draw_sprite(b.x, b.y, sp);
        b.prev_drawn = true;
    }
    for (int i = 0; i < MAX_POWERUPS; ++i) {
        Powerup& p = g_pups[i];
        if (!p.active) continue;
        draw_sprite(p.x, p.y, powerup_sprite(p.id));
        p.prev_drawn = true;
    }
    if (g_boss.active) {
        draw_sprite(g_boss.x, g_boss.y, boss_sprite(g_boss.id));
        g_boss.prev_drawn = true;
        // HALLUCINATION: when the real-clone flash is live, overlay a tiny
        // orange marker dot above the real ghost so the player can confirm
        // a successful pod scout.
        if (g_boss.pattern_id == 2 && now < g_hallu_flash_until_ms) {
            int mx = g_boss.x + g_hallu_real_idx * 16 + 6;
            int my = g_boss.y - 3;
            if (my < FIELD_Y0) my = FIELD_Y0;
            M5.Display.fillRect(mx, my, 4, 2, to565(theme::ORANGE));
        }
    }

    // Mini-boss pair (ROGUE_TOOL_CALL post-split). Drawn as filled rounded
    // orange blocks — they read as halved fragments of the original boss
    // without needing a separate sprite. Includes per-frame erase tracking.
    if (g_split_active) {
        SpriteId sp = boss_sprite(g_boss.id);
        int half_w = sprite_w(sp) / 2;
        int half_h = sprite_h(sp) / 2;
        const uint16_t fill   = to565(theme::ORANGE);
        const uint16_t accent = to565(0x9D4534);  // coral / inner detail
        for (int i = 0; i < MAX_MINIS; ++i) {
            MiniBoss& m = g_minis[i];
            if (!m.active) continue;
            if (m.prev_drawn &&
                (m.prev_x != m.x || m.prev_y != m.y)) {
                erase_rect(m.prev_x, m.prev_y, half_w, half_h);
            }
            M5.Display.fillRect(m.x, m.y, half_w, half_h, fill);
            // Inner detail row so it doesn't look like a generic block.
            M5.Display.fillRect(m.x + 2, m.y + half_h / 2 - 1,
                                half_w - 4, 2, accent);
            m.prev_x = m.x;
            m.prev_y = m.y;
            m.prev_drawn = true;
        }
    }

    // Subagent ghost-ship (drawn semi-transparent via blue tint by reusing
    // the player sprite — visually distinct enough at 16x8).
    if (now < g_player.subagent_until_ms) {
        int sx = g_player.x;
        int sy = g_player.y - 12;
        if (sy < FIELD_Y0) sy = FIELD_Y0;
        if (g_player.sub_prev_drawn &&
            (g_player.sub_prev_x != sx || g_player.sub_prev_y != sy)) {
            erase_rect(g_player.sub_prev_x, g_player.sub_prev_y,
                       PLAYER_W, PLAYER_H);
        }
        draw_sprite(sx, sy, SpriteId::PLAYER);
        g_player.sub_prev_x = sx;
        g_player.sub_prev_y = sy;
        g_player.sub_prev_drawn = true;
    }

    // Player blink during invuln
    bool blink_off = (now < g_player.invuln_until_ms) && ((now / 80) & 1);
    if (!blink_off && g_player.active) {
        draw_sprite(g_player.x, g_player.y, player_current_sprite());
        // Visible blue shield bubble while cache-hit is active. Drawn AFTER
        // the ship so the ring sits on top of the sprite's edge pixels.
        // Erase covers the bubble's bbox in the next frame's render_entities
        // because prev_x/y catch (PLAYER_W + 4) of horizontal extent via the
        // standard erase_rect that already runs on player move.
        if (now < g_player.invuln_until_ms) {
            uint16_t blue = to565(theme::BLUE);
            int cx = g_player.x + PLAYER_W / 2;
            int cy = g_player.y + PLAYER_H / 2;
            int r  = (PLAYER_W / 2) + 2;
            // Clip the ring to the playfield so it never paints into the HUD
            // when the player is near the top edge.
            M5.Display.setClipRect(FIELD_X0, FIELD_Y0,
                                    FIELD_X1 - FIELD_X0, FIELD_Y1 - FIELD_Y0);
            M5.Display.drawCircle(cx, cy, r,     blue);
            M5.Display.drawCircle(cx, cy, r - 1, blue);
            M5.Display.clearClipRect();
            g_player.shield_drawn_prev = true;
            g_player.shield_prev_cx = cx;
            g_player.shield_prev_cy = cy;
        }
    }
    // Commit the rendered player position so the next frame's erase covers
    // exactly the rect we just painted. All four PLAYER / PLAYER_CHARGE_L*
    // sprites share PLAYER_W x PLAYER_H dimensions, so this rect is correct
    // for every charge variant.
    g_player.prev_x = g_player.x;
    g_player.prev_y = g_player.y;
    if (g_pod.active) {
        draw_sprite(g_pod.x, g_pod.y, pod_sprite(g_pod.mode));
        g_pod.prev_drawn = true;
    }

    // Floating pickup labels — drawn last in the entity layer, just below
    // the HUD overlay. Cream text on the IVORY background; we erase the
    // previous bbox before painting the new one so trailing pixels don't
    // streak as the label drifts up.
    {
        const uint16_t bg    = to565(theme::IVORY);
        const uint16_t cream = to565(theme::DARK);
        d.setTextSize(1);
        d.setTextDatum(top_center);
        d.setTextColor(cream, bg);
        for (int i = 0; i < MAX_FLOATERS; ++i) {
            Floater& f = g_floaters[i];
            if (!f.active && !f.prev_drawn) continue;
            // Conservative bbox for the default 6x8 font: max label is
            // "++max_tokens" (12 chars) ≈ 72 px wide.
            int est_w = (f.text ? (int)strlen(f.text) : 12) * 6 + 4;
            if (f.prev_drawn &&
                (!f.active || f.prev_x != f.x || f.prev_y != f.y)) {
                erase_rect(f.prev_x - est_w / 2, f.prev_y, est_w, 8);
                f.prev_drawn = false;
            }
            if (f.active && f.text) {
                d.drawString(f.text, f.x, f.y);
                f.prev_drawn = true;
            }
        }
        d.setTextDatum(top_left);
    }
    // Release the playfield clip so the subsequent HUD/footer draws can
    // touch their rows again.
    d.clearClipRect();
    (void)d;
}

// ---------- Input handling -----------------------------------------------

// Returns true if the caller should consume the rest of the tick (e.g., we
// just transitioned away from the app).
bool handle_input(uint32_t now) {
    if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) {
        // Still need to drain the keyboard state (space hold, etc.) below.
    }

    auto status = M5Cardputer.Keyboard.keysState();
    // Fire key matrix: CTRL was preferred but on the Cardputer's TCA8418
    // keyboard, holding CTRL can suppress concurrent printable keys (WASD
    // movement breaks). Primary fire is now K (right-hand, doesn't transform
    // other keys, plays well in parallel with left-hand WASD). SPACE and
    // CTRL still trigger fire so existing reflexes work either way.
    bool fire_now  = status.ctrl || status.space;
    bool enter_now = status.enter;
    bool l_now    = false;
    bool r_now    = false;
    bool esc_now  = false;
    bool q_now    = false;
    bool up_now   = false;
    bool down_now = false;
    int  dx = 0, dy = 0;
    for (auto k : status.word) {
        if      (k == 'w' || k == ';') { dy -= PLAYER_VEL; up_now   = true; }
        else if (k == 's' || k == '.') { dy += PLAYER_VEL; down_now = true; }
        else if (k == 'a' || k == ',')   dx -= PLAYER_VEL;
        else if (k == 'd' || k == '/')   dx += PLAYER_VEL;
        else if (k == 'l' || k == 'L')   l_now   = true;
        else if (k == 'r' || k == 'R')   r_now   = true;
        else if (k == '`' || k == '~' || k == 27) esc_now = true;
        else if (k == 'q' || k == 'Q')   q_now   = true;
        else if (k == 'k' || k == 'K')   fire_now = true;  // primary fire key
    }
    // The Cardputer maps the small dedicated ESC key into status.word as
    // backtick on most builds; tilde works as our pause-key surrogate.

    // Edge detection for one-shot keys (ESC, R, Q). Holding ESC must not
    // immediately re-open / re-close the pause modal; we act on press only.
    bool esc_edge   = esc_now   && !g_esc_prev;
    bool r_edge     = r_now     && !g_r_prev;
    bool q_edge     = q_now     && !g_q_prev;
    bool enter_edge = enter_now && !g_enter_prev;
    bool up_edge    = up_now    && !g_up_prev;
    bool down_edge  = down_now  && !g_down_prev;

    auto commit_prevs = [&]() {
        g_enter_prev = enter_now;
        g_space_prev = fire_now;
        g_l_prev     = l_now;
        g_esc_prev   = esc_now;
        g_r_prev     = r_now;
        g_q_prev     = q_now;
        g_up_prev    = up_now;
        g_down_prev  = down_now;
    };

    // ----- PAUSED -----
    if (g_mode == Mode::PAUSED) {
        // ESC closes the pause menu and resumes play (edge-triggered so a
        // held ESC from opening the menu doesn't immediately close it).
        if (esc_edge) {
            g_mode = Mode::PLAYING;
            g_modal_drawn = false;
            bg::clear_field();
            g_hud_dirty = true;
            sfx::play_menu_tick();
            commit_prevs();
            return false;
        }
        // `q` from pause exits straight to the launcher.
        if (q_edge) {
            sfx::play_menu_confirm();
            app::goto_screen(app::Screen::LAUNCHER);
            return true;
        }
        // Menu navigation — edge-triggered so a single key press = one step.
        // Previously this whole block was wrapped in `isChange()` which only
        // fires on the keyboard's transition tick and would routinely miss
        // presses (and silently swallow Enter as well). Edge detection on
        // persistent prev-flags is robust.
        if (enter_edge) {
            if (g_pause_idx == 0) {
                g_mode = Mode::PLAYING;
                g_modal_drawn = false;
                bg::clear_field();
                g_hud_dirty = true;
                sfx::play_menu_confirm();
            } else if (g_pause_idx == 1) {
                sfx::play_menu_confirm();
                reset_game();
            } else {
                sfx::play_menu_confirm();
                app::goto_screen(app::Screen::LAUNCHER);
                commit_prevs();
                return true;
            }
        }
        if (up_edge) {
            g_pause_idx = (g_pause_idx + 2) % 3;
            g_modal_drawn = false;
            sfx::play_menu_tick();
        }
        if (down_edge) {
            g_pause_idx = (g_pause_idx + 1) % 3;
            g_modal_drawn = false;
            sfx::play_menu_tick();
        }
        commit_prevs();
        return false;
    }

    // ----- STAGE_CLEARED modal — block all gameplay input ----
    // Movement + fire are NO-OPs while the celebration modal is up; otherwise
    // the player keeps drifting under the modal and ends up in odd positions
    // when the new biome starts. Quit-to-launcher still works.
    if (g_mode == Mode::STAGE_CLEARED) {
        if (q_edge || esc_edge) {
            app::goto_screen(app::Screen::LAUNCHER);
            return true;
        }
        commit_prevs();
        return false;
    }

    // ----- DEAD modal -----
    if (g_mode == Mode::DEAD) {
        // [r] or [enter] retry; [esc]/[q] back to launcher.
        if (r_edge || enter_edge) {
            reset_game();
            commit_prevs();
            return false;
        }
        if (esc_edge || q_edge) {
            app::goto_screen(app::Screen::LAUNCHER);
            return true;
        }
        commit_prevs();
        return false;
    }

    // ----- BOSS_INTRO modal -----
    if (g_mode == Mode::BOSS_INTRO) {
        if (enter_edge) {
            g_mode = Mode::PLAYING;
            g_modal_drawn = false;
            bg::clear_field();
            g_hud_dirty = true;
            sfx::play_menu_confirm();
            spawn_boss();
            commit_prevs();
            return false;
        }
        if (esc_edge) {
            // "Retreat" — back to launcher.
            app::goto_screen(app::Screen::LAUNCHER);
            return true;
        }
        commit_prevs();
        return false;
    }

    // ----- PLAYING -----
    // ESC: open pause. `q` quits straight to launcher.
    if (esc_edge) {
        g_mode = Mode::PAUSED;
        g_pause_idx = 0;
        g_modal_drawn = false;
        sfx::play_menu_tick();
        commit_prevs();
        return false;
    }
    if (q_edge) {
        app::goto_screen(app::Screen::LAUNCHER);
        return true;
    }

    // L toggle pod attach/detach (edge-triggered)
    if (l_now && !g_l_prev) {
        if (g_pod.active) {
            g_pod.attached = !g_pod.attached;
            g_pod.detach_started_ms = now;
            sfx::play_menu_tick();
        }
    }
    g_l_prev = l_now;

    // Movement.
    //
    // handle_input runs every Arduino loop tick (>1 kHz), so we MUST rate-
    // limit movement application — otherwise PLAYER_VEL=2 applied a thousand
    // times per second teleports the ship across the playfield in a few ms.
    // Gate to one application per FRAME_INTERVAL_MS (~30 Hz). Diagonal moves
    // are normalized to ±1 so diagonal magnitude isn't 1.41× faster than
    // cardinal magnitude.
    //
    // NOTE: we deliberately do NOT touch g_player.prev_x / prev_y here —
    // those are owned by render_entities() and updated once per render frame
    // right after the sprite is drawn (so the renderer's "erase at prev"
    // never loses track of the actually-drawn position).
    // During the RATE_LIMITER throttle the player's movement gate is
    // doubled (effectively halving traversal speed).
    uint32_t move_gate = g_rate_throttle_active
                            ? (uint32_t)(FRAME_INTERVAL_MS * 2)
                            : (uint32_t)FRAME_INTERVAL_MS;
    if ((dx || dy) && now - g_last_move_ms >= move_gate) {
        g_last_move_ms = now;
        if (dx && dy) {                         // diagonal — normalize
            dx = (dx > 0) ? 1 : -1;
            dy = (dy > 0) ? 1 : -1;
        }
        g_player.x += dx;
        g_player.y += dy;
        if (g_player.x < FIELD_X0) g_player.x = FIELD_X0;
        if (g_player.y < FIELD_Y0) g_player.y = FIELD_Y0;
        if (g_player.x + PLAYER_W > FIELD_X1) g_player.x = FIELD_X1 - PLAYER_W;
        if (g_player.y + PLAYER_H > FIELD_Y1) g_player.y = FIELD_Y1 - PLAYER_H;
    }

    // Fire / charge: SPACE pressed = either fire-normal (every cooldown) OR
    // start charge after holding. We approximate "tap vs hold" by firing a
    // normal shot on the falling edge if held < CHARGE_L1_MS, otherwise
    // release a charged bullet of the visual level reached.
    // Fire mode: hold-to-rapid-fire.
    // Tapping SPACE: a single shot on the press edge, then every
    // FIRE_COOLDOWN_MS while the key stays held. Charge-shot is dropped
    // in this build — the user prefers continuous auto-fire à la classic
    // 80s shmups. Free-charges from the ++max_tokens powerup still trigger
    // a one-off charged shot on press.
    if (fire_now) {
        uint32_t fire_cd = g_rate_throttle_active
                              ? (uint32_t)(FIRE_COOLDOWN_MS * 2)
                              : (uint32_t)FIRE_COOLDOWN_MS;
        if (!g_space_prev) {  // edge press
            // Edge press: if we have a free charge, expend it as L3 here.
            if (g_player.free_charges > 0) {
                fire_player_charged(3);
                g_player.free_charges--;
                g_last_fire_ms = now;
            } else if (now - g_last_fire_ms > fire_cd) {
                fire_player_normal();
                g_last_fire_ms = now;
            }
        } else {
            // Held — keep firing at the cooldown rate.
            if (now - g_last_fire_ms > fire_cd) {
                fire_player_normal();
                g_last_fire_ms = now;
            }
        }
    }
    // Make sure no leftover charge state interferes with auto-fire.
    g_player.charging = false;
    g_player.charge_visual_level = 0;
    commit_prevs();
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void enter() {
    randomSeed((unsigned)esp_random());
    load_highscore();
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    reset_game();
}

void tick() {
    uint32_t now = millis();

    // Defensive: clear any leftover playfield clip rect from the previous
    // frame's render_entities. Without this guard, an early-return path
    // (e.g. into DEAD mode mid-render) leaves the clip active and the next
    // modal / HUD draw silently fails to paint inside the HUD strip.
    M5.Display.clearClipRect();

    // Defensive: if the player ever ends up outside the playfield (shouldn't
    // happen with the standard clamps, but state can drift across mode
    // transitions), snap them back to a safe spot so they can never appear
    // "frozen at the top row" while inputs are doing nothing visible.
    if (g_mode == Mode::PLAYING) {
        if (g_player.x < FIELD_X0 - 1 || g_player.x > FIELD_X1 - 1 ||
            g_player.y < FIELD_Y0 - 1 || g_player.y > FIELD_Y1 - 1) {
            g_player.x = FIELD_X0 + 8;
            g_player.y = (FIELD_Y0 + FIELD_Y1) / 2 - PLAYER_H / 2;
            g_player.prev_x = g_player.x;
            g_player.prev_y = g_player.y;
        }
    }

    // Always poll input — even on non-frame ticks — so the keyboard never
    // feels laggy. The renderer + simulation are gated by FRAME_INTERVAL_MS.
    if (handle_input(now)) return;

    // handle_input may have called reset_game(), which sets
    // g_biome_start_ms = millis() — a value *strictly greater* than the
    // `now` we captured at the top of tick(). The biome_t subtraction below
    // is unsigned, so a stale `now` wraps to ~4.3 billion ms, instantly
    // satisfying boss_phase and dropping the player into a boss intro the
    // first frame after retry. Refresh `now` to keep the math sane.
    now = millis();

    if (now - g_last_frame_ms < FRAME_INTERVAL_MS) {
        sfx::tick();
        return;
    }
    g_last_frame_ms = now;

    // ----- Modal-paused branches: just paint the modal once + tick sfx ----
    if (g_mode == Mode::PAUSED) {
        if (!g_modal_drawn) {
            draw_pause_modal();
            g_modal_drawn = true;
        }
        sfx::tick();
        return;
    }
    if (g_mode == Mode::DEAD) {
        // For the first ~1 s after death, hold off on the retry modal and
        // keep running the background + particle FX so the death-explosion
        // is actually visible. After that, paint the modal and freeze.
        if (now - g_death_at_ms < 1000) {
            bg::tick();
            bg::fx_tick();
        } else if (!g_modal_drawn) {
            draw_death_modal();
            g_modal_drawn = true;
        }
        sfx::tick();
        return;
    }
    if (g_mode == Mode::BOSS_INTRO) {
        if (!g_modal_drawn) {
            draw_boss_intro_modal();
            g_modal_drawn = true;
        }
        sfx::tick();
        return;
    }
    if (g_mode == Mode::STAGE_CLEARED) {
        if (!g_modal_drawn) {
            draw_stage_cleared_modal();
            g_modal_drawn = true;
        }
        if (now >= g_stage_cleared_until_ms) {
            // Modal timer expired — actually run the biome transition now.
            advance_to_next_biome();
            g_mode = Mode::PLAYING;
            g_modal_drawn = false;
        }
        sfx::tick();
        return;
    }

    // ----- Spawning -------------------------------------------------------
    const auto& cfg = data::biome_config(g_biome);
    // Defensive: if biome_start_ms is somehow in the future relative to now
    // (cross-frame `now` capture mismatch, NVS-loaded stale clock, etc.),
    // treat biome_t as 0 instead of letting the unsigned wrap dump the
    // player straight into a boss fight.
    uint32_t biome_t = (now >= g_biome_start_ms)
                          ? (now - g_biome_start_ms)
                          : 0;
    bool boss_phase = biome_t >= cfg.duration_ms_before_boss;
    if (!boss_phase && !g_boss.active) {
        tick_spawner(now);
    } else if (boss_phase && !g_boss.active && !g_boss_intro_seen) {
        // Stop spawning trash, show the intro modal, wait for [enter].
        g_boss_intro_seen = true;
        sfx::play_boss_intro();
        g_mode = Mode::BOSS_INTRO;
        g_boss_intro_started_ms = now;
        g_modal_drawn = false;
        return;
    }

    // ----- Simulation -----------------------------------------------------
    update_enemies(now);
    update_player_bullets(now);
    update_enemy_bullets(now);
    update_powerups(now);
    update_floaters(now);
    update_pod(now);
    tick_boss(now);
    update_player(now);

    // ----- Render ---------------------------------------------------------
    bg::tick();
    render_entities(now);

    if (g_hud_dirty || g_boss.active || g_split_active) {
        draw_hud();
        g_hud_dirty = false;
    }

    bg::fx_tick();
    sfx::tick();
}

}  // namespace orbit
}  // namespace apps
