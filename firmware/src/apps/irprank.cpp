#include "irprank.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <esp_random.h>

#include "../app.h"
#include "../ir/learned.h"
#include "../settings/store.h"
#include "../theme.h"

namespace apps {
namespace irprank {

namespace {

constexpr uint16_t IR_PIN = 44;   // Cardputer / Cardputer ADV IR LED

enum class Cmd { VOL_DOWN, VOL_UP, MUTE };

// ---------------------------------------------------------------------------
// Brand code table. Classic, well-documented TV remote codes; most TVs of
// these brands still accept them. T on the setup screen verifies.
// ---------------------------------------------------------------------------
IRsend g_ir(IR_PIN);
bool g_rc5_toggle = false;

void send_samsung(Cmd c) {
    const uint64_t code = c == Cmd::VOL_DOWN ? 0xE0E0D02F
                        : c == Cmd::VOL_UP   ? 0xE0E0E01F
                                             : 0xE0E0F00F;
    g_ir.sendSAMSUNG(code);
}
void send_lg(Cmd c) {
    const uint64_t code = c == Cmd::VOL_DOWN ? 0x20DFC03F
                        : c == Cmd::VOL_UP   ? 0x20DF40BF
                                             : 0x20DF906F;
    g_ir.sendNEC(code);
}
void send_sony(Cmd c) {
    const uint64_t code = c == Cmd::VOL_DOWN ? 0xC90
                        : c == Cmd::VOL_UP   ? 0x490
                                             : 0x290;
    g_ir.sendSony(code, kSony12Bits, 2);   // Sony wants >= 3 frames
}
void send_philips(Cmd c) {
    const uint8_t cmd = c == Cmd::VOL_DOWN ? 0x11
                      : c == Cmd::VOL_UP   ? 0x10
                                           : 0x0D;
    uint64_t data = g_ir.encodeRC5(0x00, cmd);
    // RC5 TVs read the same toggle bit twice in a row as "button still
    // held", so flip it on every press like a real remote does.
    if (g_rc5_toggle) data = g_ir.toggleRC5(data);
    g_rc5_toggle = !g_rc5_toggle;
    g_ir.sendRC5(data);
}
void send_panasonic(Cmd c) {
    const uint32_t code = c == Cmd::VOL_DOWN ? 0x01008485
                        : c == Cmd::VOL_UP   ? 0x01000405
                                             : 0x01004C4D;
    g_ir.sendPanasonic(0x4004, code);
}

// ---------------------------------------------------------------------------
// Learned codes — captured from the user's own remote through an IR receiver
// on the Grove port and persisted in NVS. One slot per Cmd.
// ---------------------------------------------------------------------------
using ir::Learned;
using ir::N_SLOTS;
using ir::SLOT_NAMES;
Learned g_learned[N_SLOTS];

// Grove port signal pins. Which one carries the receiver output depends on
// the module/cable, so it's switchable on the learn screen.
constexpr uint8_t RX_PINS[] = {1, 2};
uint8_t g_rx_pin_idx = 0;

void load_learned() {
    for (auto& l : g_learned) l = Learned{};
    ir::load_slots(g_learned, &g_rx_pin_idx);
    g_rx_pin_idx %= sizeof(RX_PINS);
}

void save_learned() { ir::save_slots(g_learned, g_rx_pin_idx); }

void send_learned(Cmd c) {
    const Learned& l = g_learned[(int)c];
    if (l.type < 0) {
        Serial.printf("[irprank] learned slot %d empty\n", (int)c);
        return;
    }
    const decode_type_t t = (decode_type_t)l.type;
    // Sony TVs only act on a command after three identical frames.
    const uint16_t repeat = (t == SONY) ? 2 : kNoRepeat;
    g_ir.send(t, l.value, l.bits, repeat);
}

struct Brand {
    const char* name;
    void (*send)(Cmd);
};
constexpr Brand BRANDS[] = {
    {"Samsung",   send_samsung},
    {"LG",        send_lg},
    {"Sony",      send_sony},
    {"Philips",   send_philips},
    {"Panasonic", send_panasonic},
    {"Learned",   send_learned},
};
constexpr int N_BRANDS = sizeof(BRANDS) / sizeof(BRANDS[0]);

// ---------------------------------------------------------------------------
// Prank configuration
// ---------------------------------------------------------------------------
enum class Mode { CREEP_DOWN, CREEP_UP, GHOST_MUTE, CHAOS };
const char* const MODE_NAMES[] = {"Creep down", "Creep up", "Ghost mute", "Chaos"};
constexpr int N_MODES = 4;

struct Interval { const char* name; uint32_t min_s, max_s; };
constexpr Interval INTERVALS[] = {
    {"10-20 s",  10,  20},
    {"30-90 s",  30,  90},
    {"1-3 min",  60, 180},
    {"3-8 min", 180, 480},
};
constexpr int N_INTERVALS = sizeof(INTERVALS) / sizeof(INTERVALS[0]);

constexpr int STEP_LIMITS[] = {5, 10, 20, 0};   // 0 = unlimited
constexpr int N_LIMITS = sizeof(STEP_LIMITS) / sizeof(STEP_LIMITS[0]);

int g_brand = 0, g_mode = 0, g_interval = 1, g_limit = 1;
int g_row = 0;
constexpr int N_ROWS = 4;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
bool     g_running   = false;
bool     g_screen_on = true;
bool     g_wait_release = true;
uint32_t g_next_ms   = 0;
uint32_t g_wake_ms   = 0;
uint32_t g_unmute_ms = 0;     // pending second MUTE for ghost mute
int      g_sent      = 0;
uint32_t g_last_paint_ms = 0;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void set_screen(bool on) {
    g_screen_on = on;
    if (on) {
        uint8_t pct = settings::store().brightness_pct;
        if (pct < 10) pct = 10;
        M5.Display.setBrightness((uint8_t)((uint16_t)pct * 255 / 100));
    } else {
        M5.Display.setBrightness(0);
    }
}

void schedule_next() {
    const auto& iv = INTERVALS[g_interval];
    uint32_t span = (iv.max_s - iv.min_s) * 1000;
    g_next_ms = millis() + iv.min_s * 1000 + (span ? esp_random() % span : 0);
}

void blink_led() {
    // Tiny confirmation on the screen corner (only visible when awake).
    if (g_screen_on)
        M5.Display.fillCircle(theme::SCREEN_W - 10, theme::CONTENT_TOP + 10, 3,
                              to565(theme::ORANGE));
}

void fire(Cmd c) {
    Serial.printf("[irprank] %s cmd=%d\n", BRANDS[g_brand].name, (int)c);
    BRANDS[g_brand].send(c);
    blink_led();
}

void paint_setup() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.drawString("IR Prank", 8, theme::CONTENT_TOP + 4);

    char lim[12];
    if (STEP_LIMITS[g_limit]) snprintf(lim, sizeof(lim), "%d", STEP_LIMITS[g_limit]);
    else snprintf(lim, sizeof(lim), "no limit");

    const char* labels[N_ROWS] = {"TV", "Mode", "Every", "Steps"};
    const char* values[N_ROWS] = {BRANDS[g_brand].name, MODE_NAMES[g_mode],
                                  INTERVALS[g_interval].name, lim};
    const int y0 = theme::CONTENT_TOP + 26, row_h = 17;
    for (int i = 0; i < N_ROWS; ++i) {
        const int y = y0 + i * row_h;
        const bool sel = i == g_row;
        const uint32_t bg = sel ? theme::LIGHT_GRAY : theme::IVORY;
        if (sel) M5.Display.fillRoundRect(4, y - 2, theme::SCREEN_W - 8, row_h,
                                          4, to565(bg));
        M5.Display.setTextSize(1.4f);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(bg));
        M5.Display.setTextDatum(top_left);
        M5.Display.drawString(labels[i], 12, y);
        M5.Display.setTextColor(to565(sel ? theme::ORANGE : theme::DARK), to565(bg));
        M5.Display.setTextDatum(top_right);
        char v[32];
        snprintf(v, sizeof(v), sel ? "< %s >" : "%s", values[i]);
        M5.Display.drawString(v, theme::SCREEN_W - 12, y);
    }

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("W/S A/D T test L learn ENTER arm ` back", 4,
                          theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_running() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.drawString("IR Prank armed", 8, theme::CONTENT_TOP + 4);

    char buf[48];
    M5.Display.setTextSize(1.4f);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    snprintf(buf, sizeof(buf), "%s / %s", BRANDS[g_brand].name, MODE_NAMES[g_mode]);
    M5.Display.drawString(buf, 8, theme::CONTENT_TOP + 28);

    const int limit = STEP_LIMITS[g_limit];
    if (limit) snprintf(buf, sizeof(buf), "Sent %d / %d", g_sent, limit);
    else       snprintf(buf, sizeof(buf), "Sent %d", g_sent);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.drawString(buf, 8, theme::CONTENT_TOP + 48);

    int32_t left = (int32_t)(g_next_ms - millis()) / 1000;
    if (left < 0) left = 0;
    snprintf(buf, sizeof(buf), "Next in %d s", (int)left);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.drawString(buf, 8, theme::CONTENT_TOP + 68);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("screen off in 5 s   ENTER/` stop", 4,
                          theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
    g_last_paint_ms = millis();
}

void paint_done() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.drawString("Mission complete", theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 40);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d commands sent", g_sent);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1.2f);
    M5.Display.drawString(buf, theme::SCREEN_W / 2, theme::CONTENT_TOP + 68);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("any key  back to setup", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void arm() {
    g_running = true;
    g_sent = 0;
    g_unmute_ms = 0;
    schedule_next();
    g_wake_ms = millis();
    set_screen(true);
    paint_running();
}

void disarm(bool show_done) {
    // Never leave a ghost-muted TV behind.
    if (g_unmute_ms) {
        fire(Cmd::MUTE);
        g_unmute_ms = 0;
    }
    g_running = false;
    set_screen(true);
    if (show_done) paint_done();
    else paint_setup();
}

void prank_step() {
    Mode m = (Mode)g_mode;
    if (m == Mode::CHAOS) m = (Mode)(esp_random() % 3);
    switch (m) {
        case Mode::CREEP_DOWN: fire(Cmd::VOL_DOWN); break;
        case Mode::CREEP_UP:   fire(Cmd::VOL_UP);   break;
        case Mode::GHOST_MUTE:
            fire(Cmd::MUTE);
            g_unmute_ms = millis() + 2000 + esp_random() % 3000;
            break;
        default: break;
    }
    g_sent++;
}

void tick_running(bool key_down, bool enter, char c) {
    const uint32_t now = millis();

    if (g_unmute_ms && (int32_t)(now - g_unmute_ms) >= 0) {
        fire(Cmd::MUTE);
        g_unmute_ms = 0;
    }

    const int limit = STEP_LIMITS[g_limit];
    if (!g_unmute_ms && limit && g_sent >= limit) {
        disarm(/*show_done=*/true);
        g_wait_release = true;
        return;
    }

    if (!g_unmute_ms && (int32_t)(now - g_next_ms) >= 0) {
        prank_step();
        schedule_next();
        if (g_screen_on) paint_running();
    }

    if (key_down) {
        if (enter || c == '`') {
            disarm(/*show_done=*/false);
            return;
        }
        // Any other key: wake the screen for a peek.
        g_wake_ms = now;
        if (!g_screen_on) set_screen(true);
        paint_running();
    }

    if (g_screen_on && now - g_wake_ms > 5000) set_screen(false);
    else if (g_screen_on && now - g_last_paint_ms > 1000) paint_running();
}

void cycle(int& v, int n, int d) { v = (v + d + n) % n; }

// ---------------------------------------------------------------------------
// Learn screen
// ---------------------------------------------------------------------------
bool     g_learning  = false;
int      g_learn_row = 0;
IRrecv*  g_rx        = nullptr;
uint32_t g_listen_ms = 0;          // 0 = not listening
char     g_learn_msg[48] = {0};
uint32_t g_learn_msg_color = theme::MID_GRAY;

constexpr uint32_t LISTEN_TIMEOUT_MS = 15000;

void stop_listening() {
    if (g_rx) {
        g_rx->disableIRIn();
        delete g_rx;
        g_rx = nullptr;
    }
    g_listen_ms = 0;
}

void start_listening() {
    stop_listening();
    g_rx = new IRrecv(RX_PINS[g_rx_pin_idx], 1024, 15, /*save_buffer=*/false);
    g_rx->enableIRIn();
    g_listen_ms = millis();
    snprintf(g_learn_msg, sizeof(g_learn_msg), "Press %s on your remote...",
             SLOT_NAMES[g_learn_row]);
    g_learn_msg_color = theme::YELLOW;
}

void describe(const Learned& l, char* out, size_t n) {
    if (l.type < 0) { snprintf(out, n, "--"); return; }
    String proto = typeToString((decode_type_t)l.type);
    String hex = uint64ToString(l.value, 16);
    hex.toUpperCase();
    snprintf(out, n, "%s %s", proto.c_str(), hex.c_str());
}

void paint_learn() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.drawString("Learn remote", 8, theme::CONTENT_TOP + 4);

    char pin[12];
    snprintf(pin, sizeof(pin), "RX G%d", RX_PINS[g_rx_pin_idx]);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString(pin, theme::SCREEN_W - 8, theme::CONTENT_TOP + 8);

    const int y0 = theme::CONTENT_TOP + 26, row_h = 17;
    for (int i = 0; i < N_SLOTS; ++i) {
        const int y = y0 + i * row_h;
        const bool sel = i == g_learn_row;
        const uint32_t bg = sel ? theme::LIGHT_GRAY : theme::IVORY;
        if (sel) M5.Display.fillRoundRect(4, y - 2, theme::SCREEN_W - 8, row_h,
                                          4, to565(bg));
        M5.Display.setTextDatum(top_left);
        M5.Display.setTextSize(1.3f);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(bg));
        M5.Display.drawString(SLOT_NAMES[i], 12, y);
        char v[40];
        describe(g_learned[i], v, sizeof(v));
        M5.Display.setTextDatum(top_right);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(to565(g_learned[i].type < 0 ? theme::MID_GRAY
                                                            : theme::GREEN),
                                to565(bg));
        M5.Display.drawString(v, theme::SCREEN_W - 12, y + 3);
    }

    M5.Display.setTextDatum(top_left);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(g_learn_msg_color), to565(theme::IVORY));
    M5.Display.drawString(g_learn_msg, 8, y0 + N_SLOTS * row_h + 4);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("W/S  ENTER learn  T test  P pin  ` back", 4,
                          theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void enter_learn() {
    g_learning = true;
    g_learn_row = 0;
    snprintf(g_learn_msg, sizeof(g_learn_msg),
             "IR receiver on the Grove port");
    g_learn_msg_color = theme::MID_GRAY;
    paint_learn();
}

void leave_learn() {
    stop_listening();
    g_learning = false;
    paint_setup();
}

// Poll the receiver while listening. Returns true if the screen changed.
bool poll_receiver() {
    if (!g_rx || !g_listen_ms) return false;

    if (millis() - g_listen_ms > LISTEN_TIMEOUT_MS) {
        stop_listening();
        snprintf(g_learn_msg, sizeof(g_learn_msg),
                 "Nothing received - check pin (P)");
        g_learn_msg_color = theme::ORANGE;
        return true;
    }

    decode_results r;
    if (!g_rx->decode(&r)) return false;

    bool changed = false;
    if (r.repeat) {
        // NEC "key held" frame — carries no command, keep listening.
    } else if (r.decode_type == decode_type_t::UNKNOWN) {
        snprintf(g_learn_msg, sizeof(g_learn_msg),
                 "Unknown protocol - try again");
        g_learn_msg_color = theme::ORANGE;
        changed = true;
    } else if (hasACState(r.decode_type)) {
        snprintf(g_learn_msg, sizeof(g_learn_msg),
                 "A/C remotes not supported");
        g_learn_msg_color = theme::ORANGE;
        changed = true;
    } else {
        Learned& l = g_learned[g_learn_row];
        l.type  = (int16_t)r.decode_type;
        l.bits  = r.bits;
        l.value = r.value;
        save_learned();
        stop_listening();
        char v[40];
        describe(l, v, sizeof(v));
        snprintf(g_learn_msg, sizeof(g_learn_msg), "Saved: %s", v);
        g_learn_msg_color = theme::GREEN;
        Serial.printf("[irprank] learned %s -> %s (%u bits)\n",
                      SLOT_NAMES[g_learn_row], v, (unsigned)l.bits);
        if (g_learn_row < N_SLOTS - 1) g_learn_row++;   // next role
        return true;
    }
    if (g_rx) g_rx->resume();
    return changed;
}

void tick_learn(bool key_down, bool enter, char c) {
    if (poll_receiver()) paint_learn();
    if (!key_down) return;

    if (c == '`') { leave_learn(); return; }
    if (enter) { start_listening(); paint_learn(); return; }

    switch (c) {
        case 'w': case ';': stop_listening(); cycle(g_learn_row, N_SLOTS, -1); break;
        case 's': case '.': stop_listening(); cycle(g_learn_row, N_SLOTS, +1); break;
        case 'p': {
            stop_listening();
            g_rx_pin_idx = (g_rx_pin_idx + 1) % sizeof(RX_PINS);
            save_learned();
            snprintf(g_learn_msg, sizeof(g_learn_msg), "Receiver pin now G%d",
                     RX_PINS[g_rx_pin_idx]);
            g_learn_msg_color = theme::MID_GRAY;
            break;
        }
        case 't': {
            const Learned& l = g_learned[g_learn_row];
            if (l.type < 0) {
                snprintf(g_learn_msg, sizeof(g_learn_msg), "Slot empty - learn first");
                g_learn_msg_color = theme::ORANGE;
            } else {
                // Don't hear our own transmission on the receiver.
                stop_listening();
                send_learned((Cmd)g_learn_row);
                snprintf(g_learn_msg, sizeof(g_learn_msg), "Sent %s",
                         SLOT_NAMES[g_learn_row]);
                g_learn_msg_color = theme::MID_GRAY;
            }
            break;
        }
        default: return;
    }
    paint_learn();
}

}  // namespace

void enter() {
    g_ir.begin();
    load_learned();
    g_learning = false;
    g_running = false;
    g_row = 0;
    g_wait_release = true;
    set_screen(true);
    paint_setup();
}

void tick() {
    const bool any_key = !M5Cardputer.Keyboard.keyList().empty();
    if (g_wait_release) {
        if (!any_key) g_wait_release = false;
        return;
    }

    const bool key_down = M5Cardputer.Keyboard.isChange() && any_key;
    auto& ks = M5Cardputer.Keyboard.keysState();
    const char c = ks.word.empty() ? 0 : ks.word.front();

    if (g_running) {
        tick_running(key_down, ks.enter, c);
        return;
    }
    if (g_learning) {
        tick_learn(key_down, ks.enter, c);
        return;
    }

    if (!key_down) return;

    // Back on the setup screen (also dismisses "Mission complete").
    if (c == '`') {
        set_screen(true);
        app::goto_screen(app::Screen::LAUNCHER);
        return;
    }
    if (ks.enter) { arm(); return; }
    if (c == 'l') { enter_learn(); return; }

    switch (c) {
        case 'w': case ';': cycle(g_row, N_ROWS, -1); break;
        case 's': case '.': cycle(g_row, N_ROWS, +1); break;
        case 'a': case ',': case 'd': case '/': {
            const int d = (c == 'a' || c == ',') ? -1 : +1;
            if (g_row == 0) cycle(g_brand, N_BRANDS, d);
            if (g_row == 1) cycle(g_mode, N_MODES, d);
            if (g_row == 2) cycle(g_interval, N_INTERVALS, d);
            if (g_row == 3) cycle(g_limit, N_LIMITS, d);
            break;
        }
        case 't':
            // Test with the mode's own command so the TV visibly reacts.
            fire(g_mode == (int)Mode::CREEP_UP ? Cmd::VOL_UP : Cmd::VOL_DOWN);
            delay(150);
            break;
        default:
            break;
    }
    paint_setup();
}

}  // namespace irprank
}  // namespace apps
