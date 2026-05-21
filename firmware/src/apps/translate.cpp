#include "translate.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <string>

#include "../app.h"
#include "../audio/io.h"
#include "../net/anthropic.h"
#include "../net/openai.h"
#include "../theme.h"
#include "../ui/burst_frames.h"
#include "../ui/spark.h"

namespace apps {
namespace translate {

namespace {

enum class State {
    IDLE, LISTENING, TRANSCRIBING, TRANSLATING, SPEAKING, DONE, ERROR
};

struct Lang {
    const char* code;    // ISO 639-1 (currently unused — Whisper auto-detects)
    const char* label;   // 2-char display tag
    const char* name;    // full name for the Claude prompt
};

constexpr Lang LANGS[] = {
    {"de", "DE", "German"},
    {"en", "EN", "English"},
};
constexpr int N_LANGS = sizeof(LANGS) / sizeof(LANGS[0]);

// Mic / VAD constants — mirror Pocket so the behaviour feels identical.
constexpr uint16_t SILENCE_RMS_THRESHOLD = 400;
constexpr int      SILENCE_CHUNKS_TO_END = 25;
constexpr int      CHUNK_SAMPLES         = 1600;
constexpr uint32_t MAX_RECORD_SAMPLES    = 16000 * 30;
constexpr uint32_t MIN_RECORD_SAMPLES    = 16000;
// Shared with Pocket and Buddy. The 1.5 MB LittleFS partition can't hold
// concurrent per-app copies (a 30 s TTS reply alone is 1.4 MB), so all
// voice flows reuse the same two paths.
constexpr const char* REC_FILE_PATH      = "/rec.pcm";
constexpr const char* TTS_FILE_PATH      = "/tts.pcm";

// Audio playback slab pool — lives in .bss; only used during TTS.
constexpr uint32_t POOL_SAMPLES = 16384;
static int16_t     g_pool_buf[POOL_SAMPLES];
static int16_t     g_chunk_buf[CHUNK_SAMPLES];

int      g_src = 0;          // index into LANGS — DE by default
int      g_dst = 1;          // EN
State    g_state = State::IDLE;
char     g_source_text[256] = {0};
char     g_target_text[512] = {0};
char     g_status[64]       = "Press OK to speak";

File     g_rec_file;
size_t   g_pcm_pos = 0;
int      g_silent_chunks = 0;
bool     g_heard_speech = false;
int      g_warmup_chunks_left = 0;
uint32_t g_last_chunk_ms = 0;

M5Canvas g_spark(&M5.Display);
bool     g_spark_ready = false;
float    g_anim_phase  = 0.0f;
uint32_t g_last_anim_ms = 0;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void draw_spark(int x, int y) {
    if (!g_spark_ready) { ui::Spark::create(g_spark); g_spark_ready = true; }
    int idx = ((int)g_anim_phase) % ui::burst::N_FRAMES;
    ui::Spark::draw_frame(g_spark, idx, theme::IVORY);
    g_spark.pushSprite(x, y);
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    // Direction pill: "DE → EN" right under the status bar.
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    char dir[16];
    snprintf(dir, sizeof(dir), "%s -> %s", LANGS[g_src].label, LANGS[g_dst].label);
    M5.Display.drawString(dir, 8, theme::CONTENT_TOP + 4);

    // Spark anchor in the corner — same animation hook as the other apps.
    draw_spark(theme::SCREEN_W - 52, theme::CONTENT_TOP + 2);

    // Status line in yellow (matches the transient-feedback convention
    // we set in the Radio screen).
    M5.Display.setTextColor(to565(theme::YELLOW), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString(g_status, 8, theme::CONTENT_TOP + 24);

    // Source text (gray, small) and target text (orange, larger).
    int y = theme::CONTENT_TOP + 38;
    if (g_source_text[0]) {
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1);
        M5.Display.drawString(g_source_text, 8, y);
        y += 14;
    }
    if (g_target_text[0]) {
        M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
        M5.Display.setTextSize(1.6f);
        // Naive two-line wrap on word boundary. 11 px per glyph at 1.6×.
        int max_chars = (theme::SCREEN_W - 16) / 11;
        if (max_chars < 14) max_chars = 14;
        std::string s = g_target_text;
        size_t cut = (s.size() > (size_t)max_chars)
                     ? s.rfind(' ', (size_t)max_chars)
                     : std::string::npos;
        if (cut == std::string::npos || cut < 8) cut = s.size();
        std::string l1 = s.substr(0, cut);
        std::string l2 = cut < s.size() ? s.substr(cut + 1) : std::string();
        if (l2.size() > (size_t)max_chars) l2 = l2.substr(0, max_chars - 1) + "...";
        M5.Display.drawString(l1.c_str(), 8, y);
        if (!l2.empty()) M5.Display.drawString(l2.c_str(), 8, y + 18);
    }

    // Footer hint
    M5.Display.setTextDatum(bottom_left);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString("OK speak   L swap   `  back", 4, theme::SCREEN_H - 2);
}

void set_state(State s, const char* status) {
    g_state = s;
    if (status) {
        strncpy(g_status, status, sizeof(g_status) - 1);
        g_status[sizeof(g_status) - 1] = '\0';
    }
    paint();
}

void start_listening() {
    g_source_text[0] = '\0';
    g_target_text[0] = '\0';
    g_pcm_pos = 0;
    g_silent_chunks = 0;
    g_heard_speech = false;
    g_warmup_chunks_left = 5;
    if (g_rec_file) g_rec_file.close();
    g_rec_file = LittleFS.open(REC_FILE_PATH, "w");
    if (!g_rec_file) {
        set_state(State::ERROR, "Storage error");
        return;
    }
    audio::mic_open();
    set_state(State::LISTENING, "Listening...");
}

void anim_tick_local() {
    uint32_t now = millis();
    if (now - g_last_anim_ms < 80) return;
    g_last_anim_ms = now;
    g_anim_phase += 1.0f;
    draw_spark(theme::SCREEN_W - 52, theme::CONTENT_TOP + 2);
}

void translate_and_speak() {
    if (WiFi.status() != WL_CONNECTED) {
        set_state(State::ERROR, "No WiFi");
        return;
    }
    // System prompt: pure translation engine, no chatter, no commentary.
    char sys[256];
    snprintf(sys, sizeof(sys),
        "You are a translation engine. Translate the user's text from %s "
        "to %s. Reply with ONLY the translation, in natural fluent %s. "
        "No quotes, no explanation, no preface, no apologies.",
        LANGS[g_src].name, LANGS[g_dst].name, LANGS[g_dst].name);

    set_state(State::TRANSLATING, "Translating...");
    g_target_text[0] = '\0';

    auto on_token = [](const std::string& s) {
        size_t cur = strlen(g_target_text);
        size_t left = sizeof(g_target_text) - 1 - cur;
        size_t take = s.size() < left ? s.size() : left;
        memcpy(g_target_text + cur, s.data(), take);
        g_target_text[cur + take] = '\0';
        paint();
    };
    std::string full = net::claude_stream(g_source_text, std::string("[]"),
                                          on_token, [](const std::string&){},
                                          sys);
    if (full.empty()) {
        set_state(State::ERROR, "Translation failed");
        return;
    }

    set_state(State::SPEAKING, "Speaking...");
    size_t tts_bytes = 0;
    bool ok = net::tts_to_file(g_target_text, TTS_FILE_PATH, &tts_bytes,
                               &anim_tick_local);
    if (ok && tts_bytes > 0) {
        audio::set_stream_tick(&anim_tick_local);
        audio::stream_begin(24000, g_pool_buf, POOL_SAMPLES);
        File f = LittleFS.open(TTS_FILE_PATH, "r");
        if (f) {
            static uint8_t buf[2048];
            while (f.available()) {
                int n = f.read(buf, sizeof(buf));
                if (n <= 0) break;
                audio::stream_push(buf, (size_t)n);
            }
            f.close();
        }
        audio::stream_end();
        audio::set_stream_tick(nullptr);
    }
    set_state(State::DONE, "OK for another");
}

void transcribe_and_translate() {
    audio::mic_close();
    if (g_rec_file) g_rec_file.close();
    if (g_pcm_pos < MIN_RECORD_SAMPLES) {
        set_state(State::IDLE, "Too short - try again");
        return;
    }
    set_state(State::TRANSCRIBING, "Transcribing...");
    std::string text = net::transcribe(REC_FILE_PATH, g_pcm_pos);
    if (text.empty()) {
        set_state(State::ERROR, "Whisper error");
        return;
    }
    strncpy(g_source_text, text.c_str(), sizeof(g_source_text) - 1);
    g_source_text[sizeof(g_source_text) - 1] = '\0';
    // Free the recording's blocks before tts_to_file starts writing —
    // /rec.pcm (~160 KB) + /tts.pcm (up to 1.4 MB) can otherwise exceed
    // the 1.5 MB LittleFS partition and the next write divide-by-zeros
    // inside lfs_alloc.
    LittleFS.remove(REC_FILE_PATH);
    paint();
    translate_and_speak();
}

}  // namespace

void enter() {
    g_state = State::IDLE;
    g_source_text[0] = '\0';
    g_target_text[0] = '\0';
    strncpy(g_status, "Press OK to speak", sizeof(g_status));
    paint();
}

void tick() {
    uint32_t now = millis();

    // Spark animates whenever the device is doing network work.
    bool busy = (g_state == State::TRANSCRIBING ||
                 g_state == State::TRANSLATING ||
                 g_state == State::SPEAKING    ||
                 g_state == State::LISTENING);
    if (busy && now - g_last_anim_ms >= 120) {
        g_last_anim_ms = now;
        g_anim_phase += 1.0f;
        draw_spark(theme::SCREEN_W - 52, theme::CONTENT_TOP + 2);
    }

    // Mic capture loop — same VAD shape as Pocket / Buddy.
    if (g_state == State::LISTENING) {
        if (now - g_last_chunk_ms >= 100 &&
            g_pcm_pos + CHUNK_SAMPLES < MAX_RECORD_SAMPLES) {
            g_last_chunk_ms = now;
            size_t got = audio::mic_read_chunk(g_chunk_buf, CHUNK_SAMPLES);
            if (got > 0) {
                if (g_rec_file) g_rec_file.write((const uint8_t*)g_chunk_buf, got * 2);
                g_pcm_pos += got;
                uint16_t rms = audio::last_level_rms();
                if (g_warmup_chunks_left > 0) {
                    g_warmup_chunks_left--;
                } else {
                    if (rms < SILENCE_RMS_THRESHOLD) g_silent_chunks++;
                    else                              { g_silent_chunks = 0;
                                                        g_heard_speech = true; }
                    if (g_heard_speech &&
                        g_pcm_pos >= MIN_RECORD_SAMPLES &&
                        g_silent_chunks >= SILENCE_CHUNKS_TO_END) {
                        transcribe_and_translate();
                        return;
                    }
                    if (g_pcm_pos >= MAX_RECORD_SAMPLES) {
                        transcribe_and_translate();
                        return;
                    }
                }
            }
        }
    }

    // Keyboard
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();
    if (status.enter || status.space) {
        if (g_state == State::IDLE || g_state == State::DONE ||
            g_state == State::ERROR) {
            start_listening();
            return;
        }
        if (g_state == State::LISTENING) {
            // Manual stop: act as if VAD just detected silence. The
            // existing min-length guard inside transcribe_and_translate
            // still rejects clips below MIN_RECORD_SAMPLES.
            transcribe_and_translate();
            return;
        }
    }
    for (auto k : status.word) {
        if (k == '`') {
            if (g_state == State::LISTENING) audio::mic_close();
            if (g_rec_file) g_rec_file.close();
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if ((k == 'l' || k == 'L') &&
            (g_state == State::IDLE || g_state == State::DONE ||
             g_state == State::ERROR)) {
            int tmp = g_src; g_src = g_dst; g_dst = tmp;
            g_source_text[0] = '\0';
            g_target_text[0] = '\0';
            set_state(State::IDLE, "Press OK to speak");
            return;
        }
    }
}

}  // namespace translate
}  // namespace apps
