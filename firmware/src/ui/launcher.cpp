#include "launcher.h"

#include <M5Cardputer.h>
#include <M5Unified.h>

#include "../app.h"
#include "../theme.h"
#include "burst_frames.h"
#include "spark.h"

namespace ui {

namespace {

struct Entry {
    const char* label;
    const char* hint;
    app::Screen target;
};

// Alphabetical, with Settings pinned to the end as a "utilities last"
// convention — the everyday apps are at the top so the cursor lands on
// one of them after a fresh boot.
constexpr Entry ENTRIES[] = {
    {"Claude Buddy",  "BLE companion",   app::Screen::BUDDY},
    {"Claude Pocket", "Talk to Claude",  app::Screen::POCKET},
    {"Claude Orbit",  "Pixel shmup",     app::Screen::ORBIT},
    {"Radio",         "Internet radio",  app::Screen::RADIO},
    {"Snake",         "Classic",         app::Screen::SNAKE},
    {"Translator",    "Voice translate", app::Screen::TRANSLATE},
    {"Weather",       "Local forecast",  app::Screen::WEATHER},
    {"Settings",      "WiFi, BT, Audio", app::Screen::SETTINGS},
};
constexpr int N = sizeof(ENTRIES) / sizeof(ENTRIES[0]);

int g_sel = 0;
float g_spark_phase = 0.0f;
uint32_t g_last_anim_ms = 0;
M5Canvas g_spark_canvas(&M5.Display);
bool g_spark_ready = false;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void paint_full() {
    const int x0 = 0;
    const int y0 = theme::CONTENT_TOP;
    const int w  = theme::SCREEN_W;
    const int h  = theme::CONTENT_H;

    M5.Display.fillRect(x0, y0, w, h, to565(theme::IVORY));

    // Spark hero on the left (animated in tick()).
    if (!g_spark_ready) {
        Spark::create(g_spark_canvas);
        g_spark_ready = true;
    }

    // Menu list on the right. 7 entries × 17 px = 119 px fits the content
    // area exactly. Labels at 1.6× — bigger than the old 1.2× so the
    // menu reads cleanly at arm's length without scrolling.
    const int list_x = 96;
    const int row_h  = 17;
    const int list_y = y0 + 2;

    for (int i = 0; i < N; ++i) {
        int ry = list_y + i * row_h;
        bool selected = (i == g_sel);
        if (selected) {
            M5.Display.fillRoundRect(list_x - 4, ry - 1, w - list_x, row_h - 2, 4,
                                     to565(theme::LIGHT_GRAY));
            M5.Display.fillRect(list_x - 4, ry - 1, 3, row_h - 2, to565(theme::ORANGE));
        }
        M5.Display.setTextColor(to565(theme::DARK), selected ? to565(theme::LIGHT_GRAY)
                                                             : to565(theme::IVORY));
        M5.Display.setTextSize(1.6f);
        M5.Display.setTextDatum(top_left);
        M5.Display.drawString(ENTRIES[i].label, list_x + 4, ry);
    }
}

}  // namespace

void Launcher::enter() {
    g_sel = 0;
    g_spark_phase = 0.0f;
    g_last_anim_ms = millis();
    paint_full();
}

void Launcher::tick() {
    // Slow the burst to a calm idle rhythm — 150 ms per frame, ~2.4 s loop.
    uint32_t now = millis();
    if (now - g_last_anim_ms >= 150) {
        g_last_anim_ms = now;
        g_spark_phase += 1.0f;
        int idx = ((int)g_spark_phase) % burst::N_FRAMES;
        Spark::draw_frame(g_spark_canvas, idx, theme::IVORY);
        g_spark_canvas.pushSprite(8, theme::CONTENT_TOP + 22);
    }

    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    auto status = M5Cardputer.Keyboard.keysState();
    // Enter / Space ride on dedicated boolean flags in M5Cardputer's KeysState,
    // not the printable `word` vector. Navigation keys are still printable.
    if (status.enter || status.space) {
        app::goto_screen(ENTRIES[g_sel].target);
        return;
    }
    for (auto k : status.word) {
        if (k == ';' || k == 'w') {        // up: ';' is the up-arrow glyph
            g_sel = (g_sel - 1 + N) % N;
            paint_full();
        } else if (k == '.' || k == 's') { // down
            g_sel = (g_sel + 1) % N;
            paint_full();
        }
    }
}

}  // namespace ui
