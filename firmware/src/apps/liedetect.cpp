#include "liedetect.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <esp_random.h>

#include <math.h>

#include "../app.h"
#include "../audio/io.h"
#include "../theme.h"

namespace apps {
namespace liedetect {

namespace {

enum class State { IDLE, MEASURING, ANALYZING, VERDICT };
enum class Hint { NONE, TRUTH, LIE };

constexpr uint32_t RED = 0xE5484D;

// Polygraph sprite geometry.
constexpr int TRACE_X = 8;
constexpr int TRACE_Y = theme::CONTENT_TOP + 26;
constexpr int TRACE_W = theme::SCREEN_W - 16;
constexpr int TRACE_H = 56;

constexpr size_t   CHUNK_SAMPLES   = 800;   // 50 ms @ 16 kHz
constexpr uint32_t ANALYZE_MS      = 3200;
constexpr uint32_t MIN_MEASURE_MS  = 800;

State    g_state = State::IDLE;
Hint     g_hint  = Hint::NONE;
bool     g_verdict_lie = false;
bool     g_wait_release = true;

M5Canvas* g_trace = nullptr;
int16_t   g_chunk[CHUNK_SAMPLES];

uint32_t g_state_ms  = 0;
uint32_t g_frame_ms  = 0;
float    g_phase     = 0.0f;
float    g_prev_y    = TRACE_H / 2;
float    g_bpm       = 72.0f;

// Stats collected while measuring — feed the "random" verdict.
uint32_t g_chunks    = 0;
float    g_rms_sum   = 0.0f;
float    g_rms_sq    = 0.0f;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

float frand() { return (float)(esp_random() & 0xffff) / 65535.0f; }

void paint_hint_pixel() {
    // The operator's only feedback: one pixel, practically invisible.
    uint32_t c = theme::IVORY;
    if (g_hint == Hint::TRUTH) c = 0x2A4A2A;
    if (g_hint == Hint::LIE)   c = 0x4A2A2A;
    M5.Display.drawPixel(theme::SCREEN_W - 2, theme::SCREEN_H - 2, to565(c));
}

void paint_title(const char* sub) {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.drawString("Lie Detector", 8, theme::CONTENT_TOP + 4);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString(sub, theme::SCREEN_W - 8, theme::CONTENT_TOP + 8);
    M5.Display.setTextDatum(top_left);
}

void paint_footer(const char* text) {
    M5.Display.fillRect(0, theme::SCREEN_H - 12, theme::SCREEN_W - 4, 12,
                        to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString(text, 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
    paint_hint_pixel();
}

void reset_trace() {
    if (!g_trace) return;
    g_trace->fillSprite(to565(theme::IVORY));
    // Faint grid, like chart paper.
    for (int x = 0; x < TRACE_W; x += 12)
        g_trace->drawFastVLine(x, 0, TRACE_H, to565(theme::LIGHT_GRAY));
    g_trace->drawFastHLine(0, TRACE_H / 2, TRACE_W, to565(theme::LIGHT_GRAY));
    g_prev_y = TRACE_H / 2;
}

void paint_idle() {
    paint_title("POLYGRAPH v2.3");
    reset_trace();
    if (g_trace) g_trace->pushSprite(TRACE_X, TRACE_Y);

    M5.Display.setTextDatum(top_center);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(1.4f);
    M5.Display.drawString("Hold ENTER and answer", theme::SCREEN_W / 2,
                          TRACE_Y + TRACE_H + 6);
    M5.Display.setTextDatum(top_left);
    paint_footer("`  back");
}

void paint_bpm() {
    char buf[24];
    snprintf(buf, sizeof(buf), "PULSE %3d", (int)g_bpm);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(1.4f);
    M5.Display.drawString(buf, 8, TRACE_Y + TRACE_H + 6);

    // Pulsing heart dot.
    bool beat = fmodf(millis() * g_bpm / 60000.0f, 1.0f) < 0.25f;
    M5.Display.fillCircle(theme::SCREEN_W - 20, TRACE_Y + TRACE_H + 13, 5,
                          to565(beat ? RED : theme::LIGHT_GRAY));
}

void start_measuring() {
    g_state = State::MEASURING;
    g_state_ms = millis();
    g_frame_ms = 0;
    g_chunks = 0;
    g_rms_sum = g_rms_sq = 0.0f;
    g_bpm = 68.0f + frand() * 10.0f;
    paint_title("RECORDING");
    reset_trace();
    paint_footer("release ENTER when done");
    audio::mic_open();
}

void beep(uint16_t freq, uint32_t ms) {
    M5.Speaker.tone(freq, ms);
    uint32_t t0 = millis();
    while (M5.Speaker.isPlaying() && millis() - t0 < ms + 50) delay(1);
}

void decide_verdict() {
    if (g_hint == Hint::TRUTH) { g_verdict_lie = false; return; }
    if (g_hint == Hint::LIE)   { g_verdict_lie = true;  return; }
    // No hint: coin flip, nudged by loudness and "voice tremor" (variance of
    // the RMS across chunks). Pure theatre.
    float p_lie = 0.5f;
    if (g_chunks > 0) {
        float mean = g_rms_sum / g_chunks;
        float var  = g_rms_sq / g_chunks - mean * mean;
        float sd   = var > 0 ? sqrtf(var) : 0.0f;
        p_lie += (sd / (mean + 1.0f) - 0.6f) * 0.3f;
        if (p_lie < 0.2f) p_lie = 0.2f;
        if (p_lie > 0.8f) p_lie = 0.8f;
    }
    g_verdict_lie = frand() < p_lie;
}

void start_analyzing() {
    audio::mic_close();
    g_state = State::ANALYZING;
    g_state_ms = millis();
    paint_title("ANALYZING");
    if (g_trace) g_trace->pushSprite(TRACE_X, TRACE_Y);
    paint_footer("");
}

const char* const STEPS[] = {
    "Voice stress analysis...",
    "Micro-tremor detection...",
    "Pupil dilation scan...",
    "Cross-checking Wikipedia...",
    "Consulting the oracle...",
};
constexpr int N_STEPS = sizeof(STEPS) / sizeof(STEPS[0]);

void tick_analyzing() {
    uint32_t el = millis() - g_state_ms;
    if (el >= ANALYZE_MS) {
        decide_verdict();
        g_state = State::VERDICT;
        g_state_ms = millis();

        uint32_t bg = g_verdict_lie ? RED : 0x1E7A3C;
        M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W,
                            theme::CONTENT_H, to565(bg));
        M5.Display.setTextDatum(middle_center);
        M5.Display.setTextColor(to565(0xFFFFFF), to565(bg));
        M5.Display.setTextSize(4);
        M5.Display.drawString(g_verdict_lie ? "LIE!" : "TRUTH",
                              theme::SCREEN_W / 2, theme::CONTENT_TOP + 48);
        M5.Display.setTextSize(1.2f);
        char conf[32];
        snprintf(conf, sizeof(conf), "confidence %d.%d%%",
                 90 + (int)(esp_random() % 10), (int)(esp_random() % 10));
        M5.Display.drawString(conf, theme::SCREEN_W / 2, theme::CONTENT_TOP + 84);
        M5.Display.setTextSize(1);
        M5.Display.setTextDatum(bottom_left);
        M5.Display.drawString("any key  again", 4, theme::SCREEN_H - 2);
        M5.Display.setTextDatum(top_left);

        audio::begin_stream();
        if (g_verdict_lie) {
            for (int i = 0; i < 3; ++i) { beep(220, 180); delay(60); }
        } else {
            beep(660, 120); beep(880, 120); beep(1320, 220);
        }
        audio::silence_codec();

        g_hint = Hint::NONE;   // one-shot
        g_wait_release = true;
        return;
    }

    if (g_frame_ms == 0 || millis() - g_frame_ms > 120) {
        g_frame_ms = millis();
        int step = (int)(el * N_STEPS / ANALYZE_MS);
        const int y = TRACE_Y + TRACE_H + 4;
        M5.Display.fillRect(0, y, theme::SCREEN_W, 22, to565(theme::IVORY));
        M5.Display.setTextDatum(top_left);
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1);
        M5.Display.drawString(STEPS[step], 8, y);
        int w = (int)((TRACE_W) * el / ANALYZE_MS);
        M5.Display.drawRect(TRACE_X, y + 12, TRACE_W, 8, to565(theme::MID_GRAY));
        M5.Display.fillRect(TRACE_X + 1, y + 13, w > 2 ? w - 2 : 0, 6,
                            to565(theme::ORANGE));
    }
}

void tick_measuring(bool enter_held) {
    if (!enter_held && millis() - g_state_ms >= MIN_MEASURE_MS) {
        start_analyzing();
        return;
    }
    // One trace step per 50 ms chunk, regardless of how fast loop() spins.
    if (g_frame_ms != 0 && millis() - g_frame_ms < 50) return;
    g_frame_ms = millis();

    size_t got = audio::mic_read_chunk(g_chunk, CHUNK_SAMPLES);
    float rms = got ? (float)audio::last_level_rms() : 0.0f;
    g_chunks++;
    g_rms_sum += rms;
    g_rms_sq  += rms * rms;

    // Pulse drifts up while talking, settles in silence.
    float target = 72.0f + fminf(rms / 60.0f, 50.0f);
    g_bpm += (target - g_bpm) * 0.08f;

    if (g_trace) {
        // Needle position: mic level + a slow "breathing" wave + jitter.
        g_phase += 0.35f;
        float amp = fminf(rms / 3000.0f, 1.0f);
        float y = TRACE_H / 2
                - sinf(g_phase) * (4.0f + amp * 20.0f)
                - (frand() - 0.5f) * (2.0f + amp * 22.0f);
        if (y < 2) y = 2;
        if (y > TRACE_H - 3) y = TRACE_H - 3;

        constexpr int STEP = 3;
        g_trace->scroll(-STEP, 0);
        g_trace->fillRect(TRACE_W - STEP, 0, STEP, TRACE_H, to565(theme::IVORY));
        g_trace->drawPixel(TRACE_W - 1, TRACE_H / 2, to565(theme::LIGHT_GRAY));
        g_trace->drawLine(TRACE_W - 1 - STEP, (int)g_prev_y, TRACE_W - 1, (int)y,
                          to565(RED));
        g_trace->drawLine(TRACE_W - 1 - STEP, (int)g_prev_y + 1, TRACE_W - 1,
                          (int)y + 1, to565(RED));
        g_prev_y = y;
        g_trace->pushSprite(TRACE_X, TRACE_Y);
    }
    paint_bpm();
}

void leave() {
    if (g_state == State::MEASURING) audio::mic_close();
    audio::silence_codec();
    if (g_trace) {
        g_trace->deleteSprite();
        delete g_trace;
        g_trace = nullptr;
    }
    app::goto_screen(app::Screen::LAUNCHER);
}

}  // namespace

void enter() {
    g_state = State::IDLE;
    g_hint = Hint::NONE;
    g_wait_release = true;   // swallow the ENTER that opened this screen
    if (!g_trace) {
        g_trace = new M5Canvas(&M5.Display);
        g_trace->setColorDepth(16);
        if (!g_trace->createSprite(TRACE_W, TRACE_H)) {
            Serial.printf("[liedetect] sprite alloc failed\n");
            delete g_trace;
            g_trace = nullptr;
        }
    }
    paint_idle();
}

void tick() {
    auto& ks = M5Cardputer.Keyboard.keysState();
    const bool any_key = !M5Cardputer.Keyboard.keyList().empty();

    // Secret operator keys work in every state except the verdict screen.
    if (M5Cardputer.Keyboard.isChange() && any_key && g_state != State::VERDICT) {
        for (char c : ks.word) {
            if (c == 'z' || c == 'Z') { g_hint = Hint::TRUTH; paint_hint_pixel(); }
            if (c == 'x' || c == 'X') { g_hint = Hint::LIE;   paint_hint_pixel(); }
            if (c == '`' && g_state == State::IDLE) { leave(); return; }
        }
    }

    if (g_wait_release) {
        if (!any_key) g_wait_release = false;
        return;
    }

    switch (g_state) {
        case State::IDLE:
            if (ks.enter) start_measuring();
            break;
        case State::MEASURING:
            tick_measuring(ks.enter);
            break;
        case State::ANALYZING:
            tick_analyzing();
            break;
        case State::VERDICT:
            if (any_key) {
                g_state = State::IDLE;
                g_wait_release = true;
                paint_idle();
            }
            break;
    }
}

}  // namespace liedetect
}  // namespace apps
