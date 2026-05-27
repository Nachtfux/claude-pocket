#include "orbit_sprites.h"

#include <M5Unified.h>

#include "../theme.h"

// orbit-fighter sprite atlas — implementation.
//
// Each sprite is a row-major constexpr array of palette indices (one byte
// per pixel). `static constexpr` arrays at namespace scope are placed in
// .rodata, which on ESP32 lives in flash — no RAM cost. We deliberately do
// NOT use the AVR PROGMEM macro: M5Stack Cardputer is ESP32-S3 with a unified
// address space, so a plain `static constexpr uint8_t[]` is already in flash
// and is readable with a normal pointer dereference.
//
// Drawing strategy: walk the bitmap row by row and coalesce horizontal runs
// of the same non-zero palette index into a single `fillRect(w, 1)` call.
// This is ~5-10x faster than per-pixel drawPixel for the bosses and keeps
// us well within the 30 fps budget even with ~20 active sprites.

namespace apps {
namespace orbit {

namespace {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// Coral / danger red — a darker orange used for enemy fire and "404" panels.
constexpr uint32_t CORAL    = 0x9D4534;
constexpr uint32_t HOT_PINK = 0xFF4FA0;   // enemy-bullet outline — bright vs dark bg
constexpr uint32_t HOT_RED  = 0xFF5050;   // enemy-bullet core   — high contrast

uint16_t palette_color(uint8_t idx) {
    switch (idx) {
        case 1: return to565(theme::DARK);        // cream
        case 2: return to565(theme::ORANGE);
        case 3: return to565(theme::BLUE);
        case 4: return to565(theme::GREEN);
        case 5: return to565(theme::YELLOW);
        case 6: return to565(theme::MID_GRAY);
        case 7: return to565(theme::LIGHT_GRAY);
        case 8: return to565(CORAL);
        case 9: return to565(HOT_PINK);           // enemy-bullet outline
        case 10: return to565(HOT_RED);           // enemy-bullet core
        default: return 0;                        // 0 = transparent; never queried
    }
}

// ---------------------------------------------------------------------------
// Sprite bitmaps
// ---------------------------------------------------------------------------
// Shorthand legend used in the comments next to each block:
//   . 0 transparent     c 1 cream         o 2 orange       b 3 blue
//   g 4 green           y 5 yellow        m 6 mid-gray     l 7 light-gray
//   r 8 coral (danger)

// --- Player ship 20x10 — classic retro cartoon rocket pointing right --------
//
// Hallmarks: orange engine + yellow flame at the rear (left), cream body
// with two vertical red bands, blue porthole window mid-body, pointed red
// nose cone at the right tip, gentle taper top/bottom for the rocket-fin look.
//
//      col:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
//      r0:  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  X  X     nose pin
//      r1:  .  .  .  .  .  .  .  c  c  c  c  c  c  c  c  X  X  X  X  .     top taper
//      r2:  .  .  .  c  c  c  c  c  c  c  c  c  c  c  c  X  X  X  c  .     body upper + nose
//      r3:  o  o  c  c  R  R  c  c  c  c  c  c  R  R  c  X  X  c  c  .     eng + bands + nose
//      r4:  y  y  c  c  R  R  c  3  3  c  c  c  R  R  c  X  X  X  c  c     flame + porthole + tip
//      r5:  y  y  c  c  R  R  c  3  3  c  c  c  R  R  c  X  X  X  c  c     flame + porthole + tip
//      r6:  o  o  c  c  R  R  c  c  c  c  c  c  R  R  c  X  X  c  c  .     eng + bands + nose
//      r7:  .  .  .  c  c  c  c  c  c  c  c  c  c  c  c  X  X  X  c  .     body lower + nose
//      r8:  .  .  .  .  .  .  .  c  c  c  c  c  c  c  c  X  X  X  X  .     bottom taper
//      r9:  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  X  X     nose pin
//
// palette: 1=cream(c), 2=orange(o engine), 3=blue(3 porthole),
//          5=yellow(y flame), 10=hot red (R bands + X nose)

// (Unused below — the old delta sprite arrays follow; kept commented-out so
// we can switch back without re-typing them.)
// --- DELTA (previous design, kept below in comments for reference) -- ------
//
// Hallmarks: forward-swept delta wings (tips at row 0 and 9), narrow body
// running through cols 3..18, engine cluster + yellow plume at the rear,
// pointed nose with orange accent at the right tip.
//
//      col:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
//      r0:  .  .  .  .  .  .  .  .  .  .  .  .  .  .  2  c  c  .  .  .   upper wingtip
//      r1:  .  .  .  .  .  .  .  .  .  .  .  c  c  c  c  c  c  c  .  .   upper wing
//      r2:  .  .  .  .  .  .  .  c  c  c  c  c  c  c  c  c  c  c  c  .   wing+body upper
//      r3:  o  o  o  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  2   engine + nose tip
//      r4:  o  y  y  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  2  2   flame + nose
//      r5:  o  y  y  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  2  2   flame + nose
//      r6:  o  o  o  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  c  2   engine + tip
//      r7:  .  .  .  .  .  .  .  c  c  c  c  c  c  c  c  c  c  c  c  .   wing+body lower
//      r8:  .  .  .  .  .  .  .  .  .  .  .  c  c  c  c  c  c  c  .  .   lower wing
//      r9:  .  .  .  .  .  .  .  .  .  .  .  .  .  .  2  c  c  .  .  .   lower wingtip
//
// palette: 1=cream(c body), 2=orange(o engine+nose tip), 5=yellow(y flame)
// Horizontally flipped so the red nose visually points RIGHT — the direction
// bullets fly and enemies come from. The previous design rendered with nose
// on the LEFT on the device (orientation quirk).
constexpr uint8_t SP_PLAYER[20 * 10] = {
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,2,2,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,2,2,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// Charge L1 — flame extends out (now to the right since sprite is mirrored).
constexpr uint8_t SP_PLAYER_C1[20 * 10] = {
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,2,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,2,5,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// Charge L2 — flame engulfs the engine (right side).
constexpr uint8_t SP_PLAYER_C2[20 * 10] = {
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,0,0,0,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,1,5,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,1,5,5,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,0,0,0,5,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// Charge L3 — max plume + sparkles trailing the engine (rightmost cols).
constexpr uint8_t SP_PLAYER_C3[20 * 10] = {
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,5,5,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,0,5,5,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,5,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,5,5,5,
    1,1,10,10,10,1,10,10,1,1,1,3,3,1,10,10,1,5,5,5,
    0,1,1,10,10,1,10,10,1,1,1,1,1,1,10,10,1,5,5,5,
    0,1,10,10,10,1,1,1,1,1,1,1,1,1,1,1,0,5,5,5,
    0,10,10,10,10,1,1,1,1,1,1,1,1,0,0,0,0,0,5,5,
    10,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,
};

// --- BraceSwarm 16x16 — green `{}` curly braces facing each other with glow
constexpr uint8_t SP_BRACE_SWARM[16 * 16] = {
    0,0,0,4,4,0,0,0,0,0,0,4,4,0,0,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,4,4,0,0,0,0,0,0,0,0,0,0,4,4,0,
    0,4,4,0,0,0,0,5,5,0,0,0,0,4,4,0,
    4,4,0,0,0,0,5,2,2,5,0,0,0,0,4,4,
    4,4,0,0,0,0,5,2,2,5,0,0,0,0,4,4,
    4,4,0,0,0,0,5,2,2,5,0,0,0,0,4,4,
    4,4,0,0,0,0,5,2,2,5,0,0,0,0,4,4,
    0,4,4,0,0,0,0,5,5,0,0,0,0,4,4,0,
    0,4,4,0,0,0,0,0,0,0,0,0,0,4,4,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,0,4,4,0,0,0,0,0,0,0,0,4,4,0,0,
    0,0,0,4,4,0,0,0,0,0,0,4,4,0,0,0,
};

// --- Drone 404 16x16 — cream chassis, coral "404" panel
constexpr uint8_t SP_DRONE_404[16 * 16] = {
    0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,6,6,1,1,1,1,1,1,6,6,1,1,0,
    0,1,1,6,6,1,1,1,1,1,1,6,6,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,8,8,8,8,8,8,8,8,1,1,1,1,
    1,1,1,1,8,1,8,1,8,1,8,8,1,1,1,1,
    1,1,1,1,8,1,8,1,8,1,8,8,1,1,1,1,
    1,1,1,1,8,8,8,8,8,8,8,8,1,1,1,1,
    1,1,1,1,8,1,1,8,1,1,1,8,1,1,1,1,
    1,1,1,1,8,8,8,8,8,8,8,8,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
    0,1,1,2,2,1,1,1,1,1,1,2,2,1,1,0,
    0,0,1,2,1,1,1,1,1,1,1,1,2,1,0,0,
    0,0,0,2,0,0,0,0,0,0,0,0,2,0,0,0,
};

// --- Drone 500 16x16 — heavier chassis, blue shield bar
constexpr uint8_t SP_DRONE_500[16 * 16] = {
    0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,6,6,1,1,1,1,1,1,6,6,1,1,1,
    1,1,1,6,6,1,1,1,1,1,1,6,6,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,8,8,8,8,8,8,1,1,1,1,1,
    1,1,1,1,8,1,1,1,1,1,1,8,1,1,1,1,
    1,1,1,1,8,1,1,8,8,1,1,8,1,1,1,1,
    1,1,1,1,8,8,8,8,8,8,8,8,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,2,2,1,1,1,1,1,1,2,2,1,1,1,
    0,1,1,2,1,1,1,1,1,1,1,1,2,1,1,0,
    0,0,2,0,0,0,0,0,0,0,0,0,0,2,0,0,
};

// --- Lambda fighter 16x16 — cream hull, orange lambda silhouette in cockpit
constexpr uint8_t SP_LAMBDA[16 * 16] = {
    0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,
    0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,2,2,1,1,2,1,1,1,1,1,0,
    0,1,1,1,1,2,2,1,2,2,1,1,1,1,1,0,
    1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,
    1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,
    1,1,1,1,1,2,1,1,1,2,1,1,1,1,1,1,
    0,1,1,1,2,1,1,1,1,1,2,1,1,1,1,0,
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,
    0,0,0,0,2,2,0,0,0,0,2,2,0,0,0,0,
    0,0,0,0,2,0,0,0,0,0,0,2,0,0,0,0,
};

// --- Null stalker 16x16 — dim coral outline, only eye glints
constexpr uint8_t SP_NULL_STALKER[16 * 16] = {
    0,0,0,0,0,8,8,8,8,8,8,0,0,0,0,0,
    0,0,0,0,8,0,0,0,0,0,0,8,0,0,0,0,
    0,0,0,8,0,0,0,0,0,0,0,0,8,0,0,0,
    0,0,8,0,0,0,0,0,0,0,0,0,0,8,0,0,
    0,0,8,0,0,5,5,0,0,5,5,0,0,8,0,0,
    0,0,8,0,0,5,5,0,0,5,5,0,0,8,0,0,
    0,8,0,0,0,0,0,0,0,0,0,0,0,0,8,0,
    0,8,0,0,0,0,0,0,0,0,0,0,0,0,8,0,
    0,8,0,0,0,0,0,0,0,0,0,0,0,0,8,0,
    0,8,0,0,0,0,0,0,0,0,0,0,0,0,8,0,
    0,0,8,0,0,0,0,0,0,0,0,0,0,8,0,0,
    0,0,8,0,0,0,0,0,0,0,0,0,0,8,0,0,
    0,0,0,8,0,0,0,0,0,0,0,0,8,0,0,0,
    0,0,0,0,8,0,0,0,0,0,0,8,0,0,0,0,
    0,0,0,0,0,8,8,0,0,8,8,0,0,0,0,0,
    0,0,0,0,8,0,0,0,0,0,0,8,0,0,0,0,
};

// --- Tag scout 16x16 — green `<tag>` profile
constexpr uint8_t SP_TAG_SCOUT[16 * 16] = {
    0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,
    0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,
    0,0,0,0,0,4,4,0,0,4,4,0,0,0,0,0,
    0,0,0,0,4,4,0,0,0,0,4,4,0,0,0,0,
    0,0,0,4,4,0,1,1,1,1,0,4,4,0,0,0,
    0,0,4,4,0,0,1,1,1,1,0,0,4,4,0,0,
    0,4,4,0,0,1,1,2,2,1,1,0,0,4,4,0,
    4,4,0,0,0,1,1,2,2,1,1,0,0,0,4,4,
    4,4,0,0,0,1,1,2,2,1,1,0,0,0,4,4,
    0,4,4,0,0,1,1,2,2,1,1,0,0,4,4,0,
    0,0,4,4,0,0,1,1,1,1,0,0,4,4,0,0,
    0,0,0,4,4,0,1,1,1,1,0,4,4,0,0,0,
    0,0,0,0,4,4,0,0,0,0,4,4,0,0,0,0,
    0,0,0,0,0,4,4,0,0,4,4,0,0,0,0,0,
    0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,
    0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,
};

// --- For-loop 16x16 — orange `for(;;)` brackets on cream chassis
constexpr uint8_t SP_FOR_LOOP[16 * 16] = {
    0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,2,2,1,2,1,1,2,1,1,2,2,2,1,1,
    1,1,2,1,1,2,2,1,2,1,1,2,1,1,1,1,
    1,1,2,2,1,2,1,1,2,1,1,2,2,2,1,1,
    1,1,2,1,1,2,1,1,2,1,1,1,1,2,1,1,
    1,1,2,1,1,2,1,1,2,1,1,2,2,2,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,2,1,1,1,2,1,1,2,1,1,1,2,1,1,
    1,1,2,1,1,1,1,2,2,1,1,1,1,2,1,1,
    1,1,2,1,1,1,1,2,2,1,1,1,1,2,1,1,
    1,1,2,1,1,1,2,1,1,2,1,1,1,2,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,2,2,1,1,1,1,1,1,2,2,1,1,1,
    0,1,1,2,1,1,1,1,1,1,1,1,2,1,1,0,
    0,0,2,0,0,0,0,0,0,0,0,0,0,2,0,0,
};

// --- Missile 8x4 — coral dart pointing left
constexpr uint8_t SP_MISSILE[8 * 4] = {
    0,0,0,0,0,8,8,0,
    8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,
    0,0,0,0,0,8,8,0,
};

// --- Pod laser 8x8 — orange diamond/spark
constexpr uint8_t SP_POD_LASER[8 * 8] = {
    0,0,0,2,2,0,0,0,
    0,0,2,2,2,2,0,0,
    0,2,2,5,5,2,2,0,
    2,2,5,2,2,5,2,2,
    2,2,5,2,2,5,2,2,
    0,2,2,5,5,2,2,0,
    0,0,2,2,2,2,0,0,
    0,0,0,2,2,0,0,0,
};

// --- Pod spread 8x8 — three-prong spread silhouette
constexpr uint8_t SP_POD_SPREAD[8 * 8] = {
    2,0,0,0,0,0,0,2,
    2,2,0,0,0,0,2,2,
    0,2,2,2,2,2,2,0,
    0,2,5,2,2,5,2,0,
    0,2,2,2,2,2,2,0,
    2,2,0,0,0,0,2,2,
    2,0,2,2,2,2,0,2,
    0,0,2,0,0,2,0,0,
};

// --- Pod pierce 8x8 — elongated arrow silhouette
constexpr uint8_t SP_POD_PIERCE[8 * 8] = {
    0,0,2,0,0,0,0,0,
    0,2,2,2,0,0,0,0,
    2,2,5,2,2,2,2,0,
    2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,
    2,2,5,2,2,2,2,0,
    0,2,2,2,0,0,0,0,
    0,0,2,0,0,0,0,0,
};

// --- Player bullet 4x2 — orange with yellow trail
constexpr uint8_t SP_BULLET_PLAYER[4 * 2] = {
    5,2,2,2,
    5,2,2,2,
};

// --- Charged bullet 8x4 — large orange beam with yellow core
constexpr uint8_t SP_BULLET_CHARGED[8 * 4] = {
    0,2,2,2,2,2,2,0,
    2,2,5,5,5,5,2,2,
    2,2,5,5,5,5,2,2,
    0,2,2,2,2,2,2,0,
};

// --- Enemy bullet 4x3 — hot pink outline + yellow core
// Wider + brighter than the original coral 4x2 so incoming shots stand
// out against the dark IVORY background and the player can react / dodge.
constexpr uint8_t SP_BULLET_ENEMY[4 * 3] = {
    9, 9, 9, 9,
    9, 5, 5, 9,
    9, 9, 9, 9,
};

// --- Boss bullet 6x4 — hot pink outline + yellow body, even more visible
constexpr uint8_t SP_BULLET_BOSS[6 * 4] = {
    0, 9, 9, 9, 9, 0,
    9, 5, 5, 5, 5, 9,
    9, 5, 5, 5, 5, 9,
    0, 9, 9, 9, 9, 0,
};

// --- Power-up HP 12x12 — green heart with yellow sparkle
constexpr uint8_t SP_POWERUP_HP[12 * 12] = {
    0,4,4,0,0,0,0,0,0,4,4,5,
    4,4,4,4,0,0,0,0,4,4,4,4,
    4,4,4,4,4,0,0,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,
    4,4,4,4,4,4,4,4,4,4,4,4,
    0,4,4,4,4,4,4,4,4,4,4,0,
    0,0,4,4,4,4,4,4,4,4,0,0,
    0,0,0,4,4,4,4,4,4,0,0,0,
    0,0,0,0,4,4,4,4,0,0,0,0,
    0,0,0,0,0,4,4,0,0,0,0,0,
    0,0,0,0,0,4,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,
};

// --- Power-up CLEAR 12x12 — blue bomb / slash
constexpr uint8_t SP_POWERUP_CLEAR[12 * 12] = {
    0,0,0,0,0,3,3,0,0,0,0,5,
    0,0,0,0,3,3,3,3,0,0,0,0,
    0,0,0,3,3,3,3,3,3,0,0,0,
    0,0,3,3,3,3,3,3,3,3,0,0,
    0,3,3,3,3,1,1,3,3,3,3,0,
    3,3,3,3,1,1,3,3,3,3,3,3,
    3,3,3,1,1,3,3,3,3,3,3,3,
    3,3,3,1,3,3,3,3,3,3,3,3,
    0,3,3,3,3,3,3,3,3,3,3,0,
    0,0,3,3,3,3,3,3,3,3,0,0,
    0,0,0,3,3,3,3,3,3,0,0,0,
    0,0,0,0,0,3,3,0,0,0,0,0,
};

// --- Power-up CACHE 12x12 — orange sparkle
constexpr uint8_t SP_POWERUP_CACHE[12 * 12] = {
    0,0,0,0,0,2,2,0,0,0,0,5,
    0,0,0,0,0,2,2,0,0,0,0,0,
    0,0,0,0,2,2,2,2,0,0,0,0,
    0,0,5,0,2,5,5,2,0,5,0,0,
    0,0,0,0,5,2,2,5,0,0,0,0,
    2,2,2,2,2,2,2,2,2,2,2,2,
    2,2,2,2,2,2,2,2,2,2,2,2,
    0,0,0,0,5,2,2,5,0,0,0,0,
    0,0,5,0,2,5,5,2,0,5,0,0,
    0,0,0,0,2,2,2,2,0,0,0,0,
    0,0,0,0,0,2,2,0,0,0,0,0,
    0,0,0,0,0,2,2,0,0,0,0,0,
};

// --- Power-up TOKENS 12x12 — yellow `++`
constexpr uint8_t SP_POWERUP_TOKENS[12 * 12] = {
    0,0,0,0,0,0,0,0,0,0,0,5,
    0,0,1,1,0,0,0,0,1,1,0,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
    5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,
    0,0,1,1,0,0,0,0,1,1,0,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,
};

// --- Power-up SUBAGENT 12x12 — small ghost ship outline
constexpr uint8_t SP_POWERUP_SUBAGENT[12 * 12] = {
    0,0,0,1,1,1,1,1,1,0,0,5,
    0,0,1,1,1,1,1,1,1,1,0,0,
    0,1,1,5,5,1,1,5,5,1,1,0,
    0,1,1,5,5,1,1,5,5,1,1,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    0,1,1,1,2,2,2,2,1,1,1,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    0,1,0,1,0,1,0,1,0,1,0,0,
    0,1,0,1,0,1,0,1,0,1,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,
};

// --- Power-up SKIP_PERMS 12x12 — orange wrench
constexpr uint8_t SP_POWERUP_SKIP_PERMS[12 * 12] = {
    0,0,0,0,0,0,0,0,0,2,2,5,
    0,0,0,0,0,0,0,0,2,2,2,2,
    0,0,0,0,0,0,0,2,2,5,2,2,
    0,0,0,0,0,0,2,2,2,2,2,0,
    0,0,0,0,0,2,2,2,2,2,0,0,
    0,0,0,0,2,2,2,2,2,0,0,0,
    0,0,0,2,2,2,2,2,0,0,0,0,
    0,0,2,2,2,2,2,0,0,0,0,0,
    0,2,2,2,2,2,0,0,0,0,0,0,
    2,2,5,2,2,0,0,0,0,0,0,0,
    2,2,2,2,0,0,0,0,0,0,0,0,
    0,2,2,0,0,0,0,0,0,0,0,0,
};

// ---------------------------------------------------------------------------
// Bosses — bigger, but only a handful of them.
// ---------------------------------------------------------------------------
//
// To keep this file readable, the bosses are described procedurally inside
// draw_sprite() rather than as 2-3 KB byte arrays. They still draw from the
// same palette and use the same render path under the hood. Their bounding
// rects are reported by sprite_w / sprite_h so callers can erase / collide.

constexpr int BOSS_REGEX_TYRANT_W     = 48;
constexpr int BOSS_REGEX_TYRANT_H     = 40;
constexpr int BOSS_CONTEXT_OVERFLOW_W = 64;
constexpr int BOSS_CONTEXT_OVERFLOW_H = 56;
constexpr int BOSS_HALLUCINATION_W    = 48;
constexpr int BOSS_HALLUCINATION_H    = 16;
constexpr int BOSS_RATE_LIMITER_W     = 48;
constexpr int BOSS_RATE_LIMITER_H     = 40;
constexpr int BOSS_ROGUE_TOOL_CALL_W  = 48;
constexpr int BOSS_ROGUE_TOOL_CALL_H  = 40;

// ---------------------------------------------------------------------------
// Sprite table — maps id → (bitmap pointer, w, h). Bosses store nullptr and
// are dispatched to procedural draw routines below.
// ---------------------------------------------------------------------------

struct SpriteDesc {
    const uint8_t* data;
    uint8_t w;
    uint8_t h;
};

constexpr SpriteDesc SPRITES[] = {
    { SP_PLAYER,            20, 10 },
    { SP_PLAYER_C1,         20, 10 },
    { SP_PLAYER_C2,         20, 10 },
    { SP_PLAYER_C3,         20, 10 },

    { SP_BRACE_SWARM,       16, 16 },
    { SP_DRONE_404,         16, 16 },
    { SP_DRONE_500,         16, 16 },
    { SP_LAMBDA,            16, 16 },
    { SP_NULL_STALKER,      16, 16 },
    { SP_TAG_SCOUT,         16, 16 },
    { SP_FOR_LOOP,          16, 16 },
    { SP_MISSILE,            8,  4 },

    { SP_POD_LASER,          8,  8 },
    { SP_POD_SPREAD,         8,  8 },
    { SP_POD_PIERCE,         8,  8 },

    { SP_BULLET_PLAYER,      4,  2 },
    { SP_BULLET_CHARGED,     8,  4 },
    { SP_BULLET_ENEMY,       4,  3 },
    { SP_BULLET_BOSS,        6,  4 },

    { SP_POWERUP_HP,        12, 12 },
    { SP_POWERUP_CLEAR,     12, 12 },
    { SP_POWERUP_CACHE,     12, 12 },
    { SP_POWERUP_TOKENS,    12, 12 },
    { SP_POWERUP_SUBAGENT,  12, 12 },
    { SP_POWERUP_SKIP_PERMS,12, 12 },

    { nullptr, BOSS_REGEX_TYRANT_W,     BOSS_REGEX_TYRANT_H     },
    { nullptr, BOSS_CONTEXT_OVERFLOW_W, BOSS_CONTEXT_OVERFLOW_H },
    { nullptr, BOSS_HALLUCINATION_W,    BOSS_HALLUCINATION_H    },
    { nullptr, BOSS_RATE_LIMITER_W,     BOSS_RATE_LIMITER_H     },
    { nullptr, BOSS_ROGUE_TOOL_CALL_W,  BOSS_ROGUE_TOOL_CALL_H  },
};

static_assert(sizeof(SPRITES) / sizeof(SPRITES[0]) ==
                  static_cast<size_t>(SpriteId::SPRITE_COUNT),
              "SPRITES table out of sync with SpriteId enum");

// ---------------------------------------------------------------------------
// Bitmap renderer — coalesces runs of identical palette indices into a
// single fillRect call per run.
// ---------------------------------------------------------------------------
void draw_bitmap(int16_t x, int16_t y, const uint8_t* data, int w, int h) {
    auto& d = M5.Display;
    for (int row = 0; row < h; ++row) {
        const uint8_t* line = data + row * w;
        int col = 0;
        while (col < w) {
            uint8_t idx = line[col];
            if (idx == 0) { ++col; continue; }
            int run_start = col;
            while (col < w && line[col] == idx) ++col;
            int run_len = col - run_start;
            d.fillRect(x + run_start, y + row, run_len, 1, palette_color(idx));
        }
    }
}

// ---------------------------------------------------------------------------
// Procedural bosses
// ---------------------------------------------------------------------------

// Regex Tyrant — bulbous cream body with orange `.*` motifs sprinkled across,
// coral eyes glaring out of the upper torso.
void draw_boss_regex_tyrant(int16_t x, int16_t y) {
    auto& d = M5.Display;
    uint16_t cream  = palette_color(1);
    uint16_t orange = palette_color(2);
    uint16_t coral  = palette_color(8);
    uint16_t gray   = palette_color(6);

    // Bulbous body: an ellipse approximated with stacked rects.
    const int W = BOSS_REGEX_TYRANT_W, H = BOSS_REGEX_TYRANT_H;
    static const uint8_t insets[40] = {
        16,12,9,7,5,4,3,2,2,1,
         1, 0,0,0,0,0,0,0,0,0,
         0, 0,0,0,0,0,0,0,1,1,
         2, 2,3,4,5,7,9,12,16,20
    };
    for (int row = 0; row < H; ++row) {
        int inset = insets[row];
        if (inset >= W / 2) continue;
        d.fillRect(x + inset, y + row, W - 2 * inset, 1, cream);
    }

    // Eyes (coral) — two ovals near the top
    d.fillRect(x + 14, y + 11, 5, 3, coral);
    d.fillRect(x + 29, y + 11, 5, 3, coral);
    d.fillRect(x + 15, y + 12, 3, 1, gray);
    d.fillRect(x + 30, y + 12, 3, 1, gray);

    // `.*` motifs scattered (dot + star)
    auto motif = [&](int mx, int my) {
        d.fillRect(x + mx,     y + my + 2, 2, 2, orange);  // dot
        d.drawPixel(x + mx + 4, y + my,     orange);       // star arms
        d.drawPixel(x + mx + 6, y + my + 2, orange);
        d.drawPixel(x + mx + 4, y + my + 4, orange);
        d.drawPixel(x + mx + 3, y + my + 1, orange);
        d.drawPixel(x + mx + 5, y + my + 3, orange);
        d.drawPixel(x + mx + 4, y + my + 2, orange);
    };
    motif(8,  20);
    motif(22, 18);
    motif(34, 24);
    motif(14, 28);
}

// Context Overflow — nested brace pattern radiating outward.
void draw_boss_context_overflow(int16_t x, int16_t y) {
    auto& d = M5.Display;
    uint16_t green  = palette_color(4);
    uint16_t orange = palette_color(2);
    uint16_t cream  = palette_color(1);

    const int W = BOSS_CONTEXT_OVERFLOW_W, H = BOSS_CONTEXT_OVERFLOW_H;
    // Outer ring of `{ }`
    for (int row = 4; row < H - 4; ++row) {
        d.drawPixel(x + 2, y + row, green);
        d.drawPixel(x + 3, y + row, green);
        d.drawPixel(x + W - 3, y + row, green);
        d.drawPixel(x + W - 4, y + row, green);
    }
    // Inner ring of `{ }`
    for (int row = 10; row < H - 10; ++row) {
        d.drawPixel(x + 10, y + row, orange);
        d.drawPixel(x + 11, y + row, orange);
        d.drawPixel(x + W - 11, y + row, orange);
        d.drawPixel(x + W - 12, y + row, orange);
    }
    // Even more inner — cream
    for (int row = 18; row < H - 18; ++row) {
        d.drawPixel(x + 20, y + row, cream);
        d.drawPixel(x + 21, y + row, cream);
        d.drawPixel(x + W - 21, y + row, cream);
        d.drawPixel(x + W - 22, y + row, cream);
    }
    // Curls at top/bottom of outer braces
    d.fillRect(x + 3,    y + 3,     4, 1, green);
    d.fillRect(x + W - 7,y + 3,     4, 1, green);
    d.fillRect(x + 3,    y + H - 4, 4, 1, green);
    d.fillRect(x + W - 7,y + H - 4, 4, 1, green);
    // Heart at center
    d.fillRect(x + W / 2 - 4, y + H / 2 - 2, 8, 4, orange);
    d.fillRect(x + W / 2 - 2, y + H / 2 - 4, 4, 8, orange);
}

// Hallucination — three small ghost ships in a triangle.
void draw_boss_hallucination(int16_t x, int16_t y) {
    auto& d = M5.Display;
    uint16_t cream = palette_color(1);
    uint16_t coral = palette_color(8);

    auto ghost = [&](int gx, int gy) {
        // 16x16 outline
        d.drawFastHLine(x + gx + 3, y + gy + 0, 10, cream);
        d.drawFastHLine(x + gx + 1, y + gy + 1, 14, cream);
        for (int row = 2; row < 13; ++row) {
            d.drawPixel(x + gx + 0,  y + gy + row, cream);
            d.drawPixel(x + gx + 15, y + gy + row, cream);
        }
        // Eyes
        d.fillRect(x + gx + 4, y + gy + 5, 2, 2, coral);
        d.fillRect(x + gx + 10, y + gy + 5, 2, 2, coral);
        // Wavy bottom
        for (int i = 0; i < 4; ++i) {
            int bx = x + gx + i * 4;
            d.drawPixel(bx + 0, y + gy + 13, cream);
            d.drawPixel(bx + 1, y + gy + 14, cream);
            d.drawPixel(bx + 2, y + gy + 13, cream);
            d.drawPixel(bx + 3, y + gy + 14, cream);
        }
    };
    // Top ghost (centered), bottom-left, bottom-right
    ghost(16, 0);
    ghost(0,  0);
    ghost(32, 0);
}

// Rate Limiter — clock face with a red warning ring.
void draw_boss_rate_limiter(int16_t x, int16_t y) {
    auto& d = M5.Display;
    uint16_t cream  = palette_color(1);
    uint16_t coral  = palette_color(8);
    uint16_t orange = palette_color(2);
    uint16_t gray   = palette_color(6);

    const int W = BOSS_RATE_LIMITER_W, H = BOSS_RATE_LIMITER_H;
    int cx = x + W / 2;
    int cy = y + H / 2;

    // Filled circle (mid-gray face) approximated with stacked rects
    static const uint8_t r_insets[40] = {
        24,18,14,11,9,7,6,5,4,3,
         3, 2,2,1,1,1,0,0,0,0,
         0, 0,0,0,1,1,1,2,2,3,
         3, 4,5,6,7,9,11,14,18,24
    };
    for (int row = 0; row < H; ++row) {
        int inset = r_insets[row];
        if (inset >= W / 2) continue;
        // outer ring = coral, then gray face inside
        d.fillRect(x + inset,        y + row, 2,           1, coral);
        d.fillRect(x + W - inset - 2,y + row, 2,           1, coral);
        if (W - 2 * (inset + 2) > 0) {
            d.fillRect(x + inset + 2, y + row, W - 2 * (inset + 2), 1, gray);
        }
    }

    // 12 / 3 / 6 / 9 ticks
    d.fillRect(cx - 1, cy - 16, 2, 3, cream);
    d.fillRect(cx - 1, cy + 14, 2, 3, cream);
    d.fillRect(cx - 20, cy - 1, 3, 2, cream);
    d.fillRect(cx + 18, cy - 1, 3, 2, cream);

    // Hour hand (orange, pointing to 2 o'clock-ish)
    for (int t = 0; t < 10; ++t) {
        d.drawPixel(cx + t,      cy - t / 2, orange);
        d.drawPixel(cx + t,      cy - t / 2 - 1, orange);
    }
    // Center hub
    d.fillRect(cx - 2, cy - 2, 4, 4, cream);
    d.fillRect(cx - 1, cy - 1, 2, 2, coral);
}

// Rogue Tool Call — boxy "mcp__shell__exec" label with menacing coral eyes.
void draw_boss_rogue_tool_call(int16_t x, int16_t y) {
    auto& d = M5.Display;
    uint16_t cream  = palette_color(1);
    uint16_t coral  = palette_color(8);
    uint16_t orange = palette_color(2);
    uint16_t gray   = palette_color(7);

    const int W = BOSS_ROGUE_TOOL_CALL_W, H = BOSS_ROGUE_TOOL_CALL_H;

    // Outer chassis (cream)
    d.fillRect(x, y + 2, W, H - 4, cream);
    d.drawFastHLine(x + 2, y + 1,     W - 4, cream);
    d.drawFastHLine(x + 2, y + H - 2, W - 4, cream);
    // Bevel
    d.drawFastHLine(x + 1, y + 3,     W - 2, gray);
    d.drawFastHLine(x + 1, y + H - 4, W - 2, gray);

    // Eyes (two coral squares high up)
    d.fillRect(x + 10, y + 7, 6, 4, coral);
    d.fillRect(x + W - 16, y + 7, 6, 4, coral);
    d.fillRect(x + 12, y + 8, 2, 2, cream);
    d.fillRect(x + W - 14, y + 8, 2, 2, cream);

    // Tool label region (dark band with `mcp__` glyphs as orange bars)
    d.fillRect(x + 4, y + 16, W - 8, 14, gray);
    // Faux-text: bars representing "mcp__shell__exec"
    static const uint8_t bars[]   = { 2, 2, 3, 2, 2, 2, 2, 1, 1, 3, 2, 3, 1 };
    static const uint8_t spaces[] = { 1, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1 };
    int bx = x + 6;
    for (size_t i = 0; i < sizeof(bars); ++i) {
        d.fillRect(bx, y + 20, bars[i], 5, orange);
        bx += bars[i] + spaces[i];
        if (bx >= x + W - 6) break;
    }

    // Bottom "fangs"
    d.fillRect(x + 6,      y + H - 3, 3, 2, coral);
    d.fillRect(x + 14,     y + H - 3, 3, 2, coral);
    d.fillRect(x + W - 9,  y + H - 3, 3, 2, coral);
    d.fillRect(x + W - 17, y + H - 3, 3, 2, coral);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int sprite_w(SpriteId id) {
    return SPRITES[static_cast<size_t>(id)].w;
}

int sprite_h(SpriteId id) {
    return SPRITES[static_cast<size_t>(id)].h;
}

void draw_sprite(int16_t x, int16_t y, SpriteId id) {
    const auto& s = SPRITES[static_cast<size_t>(id)];
    if (s.data != nullptr) {
        draw_bitmap(x, y, s.data, s.w, s.h);
        return;
    }
    switch (id) {
        case SpriteId::BOSS_REGEX_TYRANT:     draw_boss_regex_tyrant(x, y);     break;
        case SpriteId::BOSS_CONTEXT_OVERFLOW: draw_boss_context_overflow(x, y); break;
        case SpriteId::BOSS_HALLUCINATION:    draw_boss_hallucination(x, y);    break;
        case SpriteId::BOSS_RATE_LIMITER:     draw_boss_rate_limiter(x, y);     break;
        case SpriteId::BOSS_ROGUE_TOOL_CALL:  draw_boss_rogue_tool_call(x, y);  break;
        default: break;  // unreachable
    }
}

}  // namespace orbit
}  // namespace apps
