#include "orbit_bg.h"

#include <M5Unified.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../theme.h"

namespace apps {
namespace orbit {
namespace bg {

namespace {

// -----------------------------------------------------------------------
// Playfield bounds — must match orbit.cpp exactly. Background never draws
// outside this rect.
// -----------------------------------------------------------------------
constexpr int FIELD_X0 = 2;
constexpr int FIELD_Y0 = 30;
constexpr int FIELD_X1 = 238;
constexpr int FIELD_Y1 = 121;
constexpr int FIELD_W  = FIELD_X1 - FIELD_X0;   // 236
constexpr int FIELD_H  = FIELD_Y1 - FIELD_Y0;   // 91

// Built-in M5GFX font glyph metrics at textSize 1 (5x7 with 1 px gutter).
constexpr int GLYPH_W = 6;
constexpr int GLYPH_H = 8;

// -----------------------------------------------------------------------
// Color helpers
// -----------------------------------------------------------------------
inline uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff,
                               (rgb >> 8) & 0xff,
                               rgb & 0xff);
}

// Dim variants for layer-3 debris (full-bright palette colors look too
// bright over IVORY at 1-3 px). 565 still, but pre-darkened.
inline uint16_t dim_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return M5.Display.color565(r, g, b);
}

// -----------------------------------------------------------------------
// Layer 1 — far/slow scrolling fake code snippets in MID_GRAY.
// -----------------------------------------------------------------------
const char* const SNIPPETS[] = {
    "if (x)",   "// TODO",  "return 0;", "nullptr",  "void *p",
    "0xFF",     "++i",      "[0..N]",    "<tag>",    "char c;",
    "while(1)", "free(p);", "size_t n",  "&ref",     "case 0:",
    "->next",   "0x1F1F",   "(void)",    "static",   "/* ok */",
};
constexpr int SNIPPET_COUNT = sizeof(SNIPPETS) / sizeof(SNIPPETS[0]);

constexpr int L1_SLOTS = 5;          // five rows of marquee text
struct L1Slot {
    const char* text;
    int16_t     x;        // top-left in screen space
    int8_t      row;      // 0..L1_SLOTS-1
    int8_t      prev_w;   // pixel width of last draw (for erase)
    int16_t     prev_x;
};
L1Slot g_l1[L1_SLOTS];

// -----------------------------------------------------------------------
// Layer 2 — silhouetted geometric shapes in LIGHT_GRAY.
// -----------------------------------------------------------------------
constexpr int L2_SLOTS = 4;
struct L2Slot {
    int16_t x, prev_x;
    int8_t  y;
    int8_t  w, h;
    uint8_t style;  // 0=rect outline, 1=bracket left, 2=bracket right, 3=diag
    bool    active;
};
L2Slot g_l2[L2_SLOTS];

// -----------------------------------------------------------------------
// Layer 3 — fast 1-3 px debris pixels in dim accent colors.
// -----------------------------------------------------------------------
constexpr int L3_SLOTS = 32;
struct L3Bit {
    int16_t  x, prev_x;
    int8_t   y;
    uint8_t  size;      // 1..3
    uint16_t color565;
    bool     active;
};
L3Bit g_l3[L3_SLOTS];

// -----------------------------------------------------------------------
// Particle pool — capped at 64. ~12 bytes/particle ⇒ ~770 B.
// -----------------------------------------------------------------------
constexpr int MAX_PARTICLES = 64;
struct Particle {
    int16_t  x, y;
    int16_t  prev_x, prev_y;
    int8_t   dx, dy;        // pixels per frame
    uint16_t color565;
    uint16_t age_ms;
    uint16_t lifetime_ms;
    bool     active;
};
Particle g_parts[MAX_PARTICLES];

// -----------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------
int      g_biome      = 0;
uint32_t g_frame      = 0;
uint32_t g_last_ms    = 0;
uint8_t  g_l1_phase   = 0;   // 0..5, advance 1 px every 6 frames
uint8_t  g_l2_phase   = 0;   // 0..2, advance 1 px every 3 frames

// -----------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------
inline int rnd(int lo, int hi) {   // inclusive both
    if (hi <= lo) return lo;
    return lo + (int)(random(hi - lo + 1));
}

int snippet_pixel_width(const char* s) {
    return (int)strlen(s) * GLYPH_W;
}

// Clamp a horizontal rect to the playfield. Returns true if any part is
// inside; updates x/w in place.
bool clip_h(int16_t& x, int16_t& w) {
    int16_t x1 = x + w;
    if (x  < FIELD_X0) x  = FIELD_X0;
    if (x1 > FIELD_X1) x1 = FIELD_X1;
    w = x1 - x;
    return w > 0;
}

inline bool in_field(int16_t x, int16_t y) {
    return x >= FIELD_X0 && x < FIELD_X1 && y >= FIELD_Y0 && y < FIELD_Y1;
}

// -----------------------------------------------------------------------
// Layer 1
// -----------------------------------------------------------------------
void l1_seed() {
    for (int i = 0; i < L1_SLOTS; ++i) {
        g_l1[i].text   = SNIPPETS[rnd(0, SNIPPET_COUNT - 1)];
        g_l1[i].row    = i;
        // Stagger initial x across the field width.
        g_l1[i].x      = FIELD_X0 + (FIELD_W * i) / L1_SLOTS + rnd(0, 30);
        g_l1[i].prev_x = g_l1[i].x;
        g_l1[i].prev_w = snippet_pixel_width(g_l1[i].text);
    }
}

void l1_advance() {
    // Speed: 1 px every 6 frames. STDOUT is calm, PROD pushes a bit.
    uint8_t period = (g_biome == 4) ? 4 : 6;
    if (++g_l1_phase < period) return;
    g_l1_phase = 0;
    for (int i = 0; i < L1_SLOTS; ++i) {
        g_l1[i].prev_x = g_l1[i].x;
        g_l1[i].x -= 1;
        // Wrap when fully off the left edge.
        if (g_l1[i].x + g_l1[i].prev_w < FIELD_X0) {
            g_l1[i].text   = SNIPPETS[rnd(0, SNIPPET_COUNT - 1)];
            g_l1[i].prev_w = snippet_pixel_width(g_l1[i].text);
            g_l1[i].x      = FIELD_X1 + rnd(0, 40);
            g_l1[i].prev_x = g_l1[i].x;
        }
    }
}

void l1_draw() {
    auto& d = M5.Display;
    const uint16_t bg   = to565(theme::IVORY);
    const uint16_t gray = to565(theme::MID_GRAY);

    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(gray, bg);

    for (int i = 0; i < L1_SLOTS; ++i) {
        // Row Y inside playfield, evenly spaced.
        int y = FIELD_Y0 + 4 + g_l1[i].row * ((FIELD_H - 12) / L1_SLOTS);
        if (y + GLYPH_H >= FIELD_Y1) continue;

        // Drawing in-field with bg color set will overwrite the previous
        // glyph cells. To handle the left-edge crop, manually erase the
        // previous footprint that lies inside the field.
        int16_t ex = g_l1[i].prev_x;
        int16_t ew = g_l1[i].prev_w;
        if (clip_h(ex, ew)) {
            d.fillRect(ex, y, ew, GLYPH_H, bg);
        }

        // Now print the new text, but only the portion inside the field.
        // M5GFX text rendering does not clip for us, so skip if fully out.
        int16_t tx = g_l1[i].x;
        int16_t tw = g_l1[i].prev_w;
        if (tx + tw < FIELD_X0) continue;
        if (tx >= FIELD_X1) continue;

        // Render the glyph row-by-row via setCursor; if part of the text
        // overflows the field, we just let the IVORY fillRect on next frame
        // clean up. To keep strictly in-bounds we draw inside a pushScissor.
        d.setClipRect(FIELD_X0, FIELD_Y0, FIELD_W, FIELD_H);
        d.setCursor(tx, y);
        d.print(g_l1[i].text);
        d.clearClipRect();
    }
}

// -----------------------------------------------------------------------
// Layer 2
// -----------------------------------------------------------------------
void l2_seed() {
    for (int i = 0; i < L2_SLOTS; ++i) {
        g_l2[i].active = true;
        g_l2[i].w      = rnd(10, 30);
        g_l2[i].h      = rnd(10, 26);
        g_l2[i].x      = FIELD_X0 + (FIELD_W * i) / L2_SLOTS + rnd(0, 40);
        g_l2[i].prev_x = g_l2[i].x;
        g_l2[i].y      = FIELD_Y0 + rnd(2, FIELD_H - g_l2[i].h - 2);
        g_l2[i].style  = rnd(0, 3);
    }
}

void l2_advance() {
    uint8_t period = 3;
    if (g_biome == 3) period = (g_frame & 0x07) < 3 ? 2 : 4;  // chaotic
    if (++g_l2_phase < period) return;
    g_l2_phase = 0;

    for (int i = 0; i < L2_SLOTS; ++i) {
        if (!g_l2[i].active) continue;
        g_l2[i].prev_x = g_l2[i].x;
        g_l2[i].x -= 1;
        if (g_l2[i].x + g_l2[i].w < FIELD_X0) {
            g_l2[i].w     = rnd(10, 30);
            g_l2[i].h     = rnd(10, 26);
            g_l2[i].x     = FIELD_X1 + rnd(0, 60);
            g_l2[i].prev_x = g_l2[i].x;
            g_l2[i].y     = FIELD_Y0 + rnd(2, FIELD_H - g_l2[i].h - 2);
            // STDIN gets more brackets; SANDBOX gets more diagonals.
            if (g_biome == 1) g_l2[i].style = rnd(1, 2);
            else if (g_biome == 3) g_l2[i].style = 3;
            else g_l2[i].style = rnd(0, 3);
        }
    }
}

void l2_draw_shape(const L2Slot& s, uint16_t color) {
    auto& d = M5.Display;
    // Manual clip: skip if entirely outside.
    if (s.x >= FIELD_X1 || s.x + s.w <= FIELD_X0) return;

    d.setClipRect(FIELD_X0, FIELD_Y0, FIELD_W, FIELD_H);
    switch (s.style) {
        case 0: { // rect outline
            d.drawRect(s.x, s.y, s.w, s.h, color);
            break;
        }
        case 1: { // left bracket  [
            d.drawFastVLine(s.x, s.y, s.h, color);
            d.drawFastHLine(s.x, s.y, 4, color);
            d.drawFastHLine(s.x, s.y + s.h - 1, 4, color);
            break;
        }
        case 2: { // right bracket ]
            d.drawFastVLine(s.x + s.w - 1, s.y, s.h, color);
            d.drawFastHLine(s.x + s.w - 4, s.y, 4, color);
            d.drawFastHLine(s.x + s.w - 4, s.y + s.h - 1, 4, color);
            break;
        }
        case 3: { // diagonal line
            d.drawLine(s.x, s.y, s.x + s.w, s.y + s.h, color);
            break;
        }
    }
    d.clearClipRect();
}

void l2_draw() {
    const uint16_t bg    = to565(theme::IVORY);
    const uint16_t lgray = to565(theme::LIGHT_GRAY);
    // Erase previous, draw current. We erase by redrawing the same shape
    // with IVORY at the previous location — cheaper than a bounding fill.
    for (int i = 0; i < L2_SLOTS; ++i) {
        if (!g_l2[i].active) continue;
        L2Slot prev = g_l2[i];
        prev.x = g_l2[i].prev_x;
        l2_draw_shape(prev, bg);
        l2_draw_shape(g_l2[i], lgray);
    }
}

// -----------------------------------------------------------------------
// Layer 3
// -----------------------------------------------------------------------
uint16_t l3_pick_color() {
    // Dimmed accents so 1 px specks don't dominate the scene.
    // Encoded as raw RGB565-friendly values via color565.
    switch (g_biome) {
        case 0:  // STDOUT: balanced + a touch of gray-blue
            switch (rnd(0, 2)) {
                case 0:  return dim_rgb(0x60, 0x40, 0x30);  // dim orange
                case 1:  return dim_rgb(0x30, 0x50, 0x60);  // dim cyan
                default: return dim_rgb(0x40, 0x40, 0x40);  // soft gray
            }
        case 1:  // STDIN: blue-leaning
            return (rnd(0, 1) == 0) ? dim_rgb(0x20, 0x60, 0x80)
                                    : dim_rgb(0x50, 0x40, 0x30);
        case 2:  // STAGING: green-dominant
            switch (rnd(0, 3)) {
                case 0:  return dim_rgb(0x20, 0x90, 0x30);
                case 1:  return dim_rgb(0x30, 0xA0, 0x40);
                case 2:  return dim_rgb(0x40, 0x70, 0x30);
                default: return dim_rgb(0x60, 0x40, 0x30);
            }
        case 3:  // SANDBOX: chaotic — random hues
            return dim_rgb(rnd(0x20, 0x90), rnd(0x20, 0x90), rnd(0x20, 0x90));
        case 4:  // PROD: tense orange + coral
            switch (rnd(0, 2)) {
                case 0:  return dim_rgb(0x90, 0x40, 0x20);  // orange
                case 1:  return dim_rgb(0xA0, 0x50, 0x40);  // coral
                default: return dim_rgb(0x70, 0x30, 0x20);
            }
        default: return dim_rgb(0x60, 0x60, 0x60);
    }
}

void l3_spawn(int slot) {
    g_l3[slot].active   = true;
    g_l3[slot].x        = FIELD_X1 - 1;
    g_l3[slot].prev_x   = g_l3[slot].x;
    g_l3[slot].y        = FIELD_Y0 + rnd(0, FIELD_H - 3);
    g_l3[slot].size     = (uint8_t)rnd(1, 3);
    g_l3[slot].color565 = l3_pick_color();
}

void l3_seed() {
    for (int i = 0; i < L3_SLOTS; ++i) {
        g_l3[i].active = false;
    }
    // Pre-populate scattered across the field.
    int initial = (g_biome == 0) ? 8
                 : (g_biome == 4) ? 22
                 : 14;
    for (int i = 0; i < initial && i < L3_SLOTS; ++i) {
        l3_spawn(i);
        g_l3[i].x      = FIELD_X0 + rnd(0, FIELD_W - 2);
        g_l3[i].prev_x = g_l3[i].x;
    }
}

void l3_advance() {
    // Try to spawn a new debris bit roughly every N frames based on biome.
    uint8_t spawn_period = (g_biome == 0) ? 5
                         : (g_biome == 4) ? 1
                         : 3;
    if ((g_frame % spawn_period) == 0) {
        for (int i = 0; i < L3_SLOTS; ++i) {
            if (!g_l3[i].active) { l3_spawn(i); break; }
        }
    }
    for (int i = 0; i < L3_SLOTS; ++i) {
        if (!g_l3[i].active) continue;
        g_l3[i].prev_x = g_l3[i].x;
        g_l3[i].x -= 1;
        if (g_l3[i].x + g_l3[i].size <= FIELD_X0) {
            g_l3[i].active = false;
        }
    }
}

void l3_draw() {
    auto& d = M5.Display;
    const uint16_t bg = to565(theme::IVORY);
    for (int i = 0; i < L3_SLOTS; ++i) {
        // Erase prev (clipped) if it was visible.
        int16_t ex = g_l3[i].prev_x;
        int16_t ew = g_l3[i].size;
        if (clip_h(ex, ew)) {
            d.fillRect(ex, g_l3[i].y, ew, g_l3[i].size, bg);
        }
        if (!g_l3[i].active) continue;
        int16_t dx = g_l3[i].x;
        int16_t dw = g_l3[i].size;
        if (clip_h(dx, dw)) {
            d.fillRect(dx, g_l3[i].y, dw, g_l3[i].size, g_l3[i].color565);
        }
    }
}

// -----------------------------------------------------------------------
// Particles
// -----------------------------------------------------------------------
void particles_reset() {
    for (int i = 0; i < MAX_PARTICLES; ++i) g_parts[i].active = false;
}

int find_free_particle() {
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        if (!g_parts[i].active) return i;
    }
    return -1;
}

uint16_t explosion_color(int kind) {
    switch (kind) {
        case 0: // enemy_pop → ORANGE + YELLOW
            return (rnd(0, 1) == 0) ? to565(theme::ORANGE) : to565(theme::YELLOW);
        case 1: // boss_hit → BLUE + ORANGE
            return (rnd(0, 1) == 0) ? to565(theme::BLUE) : to565(theme::ORANGE);
        case 2: // charged_shot_impact → YELLOW + DARK (brightest)
        default:
            return (rnd(0, 1) == 0) ? to565(theme::YELLOW) : to565(theme::DARK);
    }
}

void spawn_one(int16_t x, int16_t y, int8_t dx, int8_t dy,
               uint16_t color, uint16_t lifetime) {
    int idx = find_free_particle();
    if (idx < 0) return;
    g_parts[idx].x           = x;
    g_parts[idx].y           = y;
    g_parts[idx].prev_x      = x;
    g_parts[idx].prev_y      = y;
    g_parts[idx].dx          = dx;
    g_parts[idx].dy          = dy;
    g_parts[idx].color565    = color;
    g_parts[idx].age_ms      = 0;
    g_parts[idx].lifetime_ms = lifetime;
    g_parts[idx].active      = true;
}

}  // namespace

// =======================================================================
// Public API
// =======================================================================

void clear_field() {
    M5.Display.fillRect(FIELD_X0, FIELD_Y0, FIELD_W, FIELD_H,
                        to565(theme::IVORY));
}

void set_biome(int biome) {
    if (biome < 0) biome = 0;
    if (biome > 4) biome = 4;
    g_biome = biome;
}

void enter(int biome) {
    set_biome(biome);
    g_frame    = 0;
    g_last_ms  = millis();
    g_l1_phase = 0;
    g_l2_phase = 0;

    clear_field();
    l1_seed();
    l2_seed();
    l3_seed();
    particles_reset();
}

void tick() {
    g_frame++;

    // Advance scroll phases.
    l1_advance();
    l2_advance();
    l3_advance();

    // Render order: far → near.
    l1_draw();
    l2_draw();
    l3_draw();
}

void fx_spawn_explosion(int16_t x, int16_t y, int kind) {
    int n = rnd(6, 12);
    for (int i = 0; i < n; ++i) {
        int8_t dx = (int8_t)rnd(-3, 3);
        int8_t dy = (int8_t)rnd(-3, 3);
        if (dx == 0 && dy == 0) dx = 1;
        spawn_one(x, y, dx, dy, explosion_color(kind),
                  (uint16_t)rnd(200, 400));
    }
}

void fx_spawn_sparkle(int16_t x, int16_t y) {
    int n = rnd(3, 4);
    uint16_t color = to565(theme::ORANGE);
    for (int i = 0; i < n; ++i) {
        int8_t dx = (int8_t)rnd(-2, 2);
        int8_t dy = (int8_t)rnd(-3, -1);   // bias upward — "rising sparkle"
        if (dx == 0) dx = (rnd(0, 1) == 0) ? -1 : 1;
        spawn_one(x, y, dx, dy, color, (uint16_t)rnd(180, 320));
    }
}

void fx_tick() {
    auto& d = M5.Display;
    const uint16_t bg = to565(theme::IVORY);

    uint32_t now = millis();
    uint32_t dt  = now - g_last_ms;
    if (dt > 100) dt = 100;   // clamp on hiccups
    g_last_ms = now;

    for (int i = 0; i < MAX_PARTICLES; ++i) {
        if (!g_parts[i].active) continue;
        Particle& p = g_parts[i];

        // Erase previous position (only if it was inside the field).
        if (in_field(p.prev_x, p.prev_y)) {
            d.drawPixel(p.prev_x, p.prev_y, bg);
        }

        // Age.
        p.age_ms = (uint16_t)(p.age_ms + dt);
        if (p.age_ms >= p.lifetime_ms) {
            p.active = false;
            continue;
        }

        // Advance.
        p.prev_x = p.x;
        p.prev_y = p.y;
        p.x = (int16_t)(p.x + p.dx);
        p.y = (int16_t)(p.y + p.dy);

        if (!in_field(p.x, p.y)) {
            p.active = false;
            continue;
        }
        d.drawPixel(p.x, p.y, p.color565);
    }
}

}  // namespace bg
}  // namespace orbit
}  // namespace apps
