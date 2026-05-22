#include "pocket.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <math.h>

#include <string>
#include <vector>

#include "../app.h"
#include "../audio/io.h"
#include "../net/anthropic.h"
#include "../net/openai.h"
#include "../settings/store.h"
#include "../theme.h"
#include "../ui/burst_frames.h"
#include "../ui/spark.h"

namespace pocket {

namespace {

enum class State { IDLE, LISTENING, THINKING, SPEAKING, ERROR };

State g_state = State::IDLE;
M5Canvas g_spark(&M5.Display);
bool g_spark_ready = false;
float g_anim_phase = 0.0f;
uint32_t g_last_anim_ms = 0;
uint32_t g_last_chunk_ms = 0;
char g_status[160]    = "Press OK to talk";
char g_transcript[160] = "";
char g_reply[256]      = "";
std::string g_history_json = "[]";

// VAD parameters tuned for typical handheld use: silence below RMS=400 for
// roughly 800 ms ends the utterance; hard cap at 10 s so the device never
// hangs on background noise.
constexpr uint16_t SILENCE_RMS_THRESHOLD = 400;
constexpr int      SILENCE_CHUNKS_TO_END = 25;    // ~2.5 s (chunks ~100 ms)
constexpr int      CHUNK_SAMPLES         = 1600;  // 100 ms @ 16 kHz
// 30 s ceiling, way over what SRAM-only would allow. The mic now writes
// directly to LittleFS (/rec.pcm), so the only limit is the spiffs
// partition size (~1.5 MB = ~47 s) and how long the upload can run
// before Whisper times the request out (~60 s).
constexpr uint32_t MAX_RECORD_SAMPLES    = 16000 * 30;
constexpr uint32_t MIN_RECORD_SAMPLES    = 16000;        // 1 s minimum
constexpr const char* REC_FILE_PATH      = "/rec.pcm";

// 32 KB in .bss — only the audio playback pool lives here now. The mic
// no longer accumulates in RAM, so we don't need 128 KB any more. Heap
// recovers by ~96 KB → mbedTLS has plenty for the next TLS handshake.
constexpr uint32_t SHARED_SAMPLES        = 16384;
static int16_t g_shared_buf[SHARED_SAMPLES];

// Per-chunk working buffer for mic_read_chunk → file write. Only one chunk
// (100 ms / ~3.2 KB) needs to live in SRAM at a time.
static int16_t g_chunk_buf[CHUNK_SAMPLES];

File   g_rec_file;
size_t g_pcm_pos = 0;  // sample count for VAD min / max checks, file-tracked
int      g_silent_chunks = 0;
bool     g_heard_speech = false;   // don't end on silence before the user has spoken
int      g_warmup_chunks_left = 0; // ignore mic input for first N chunks — the
                                   // OK-key release pulse otherwise triggered
                                   // g_heard_speech immediately and the VAD
                                   // ended the recording after 1 s.
bool     g_armed = false;          // require key release before next trigger
uint16_t g_last_rms = 0;           // for on-screen meter
int      g_reply_scroll = 0;       // line offset into wrapped reply
uint32_t g_last_scroll_ms = 0;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// LittleFS handles the recording storage now; no in-RAM buffer check needed.

void paint_chrome() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
}

void paint_labels() {
    // Layout: top header strip (state banner), spark on the left, status/text
    // panel on the right. The mic-level dot lives at the bottom right.
    const int header_h = 18;
    const int body_y  = theme::CONTENT_TOP + header_h;
    const int body_h  = theme::SCREEN_H - body_y - 10;
    const int spark_x = 4;
    const int spark_y = body_y + 4;
    const int text_x  = spark_x + ui::Spark::SIZE + 6;
    const int text_w  = theme::SCREEN_W - text_x - 4;

    bool busy = (g_state == State::LISTENING ||
                 g_state == State::THINKING  ||
                 g_state == State::SPEAKING);

    // Header banner — bigger text during active states so "Denke nach…" is
    // legible across the room.
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, header_h, to565(theme::IVORY));
    M5.Display.setTextColor(busy ? to565(theme::ORANGE) : to565(theme::DARK),
                            to565(theme::IVORY));
    M5.Display.setTextSize(busy ? 1.6f : 1.0f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(g_status, theme::SCREEN_W / 2, theme::CONTENT_TOP + 2);

    // Text panel right of the spark. Manual word-wrap because M5GFX's
    // built-in setTextWrap wraps to display edge, not the panel boundary —
    // wrapped text would otherwise crawl under the spark.
    M5.Display.fillRect(text_x, body_y, text_w, body_h, to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    auto wrap = [&](const char* txt, float text_size, std::vector<std::string>& out) {
        out.clear();
        if (!txt || !*txt) return;
        const int char_w = (int)(6.0f * text_size);
        size_t max_chars = (size_t)(text_w / char_w);
        if (max_chars < 8) max_chars = 8;
        std::string cur, word;
        std::string s = txt;
        s += ' ';
        for (char c : s) {
            if (c == ' ' || c == '\n') {
                if (!word.empty()) {
                    if (cur.empty()) cur = word;
                    else if (cur.size() + 1 + word.size() <= max_chars) {
                        cur += ' '; cur += word;
                    } else {
                        out.push_back(cur); cur = word;
                    }
                    word.clear();
                }
                if (c == '\n' && !cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                word += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
    };

    // Transcript shows only while the status is "Understanding..." — the
    // brief window between STT returning and Claude starting to stream,
    // so the user can read what was actually heard. Once we move to
    // "Thinking..."/"Loading speech..."/"Speaking..." the transcript
    // hides and Claude's reply gets the full body height.
    std::vector<std::string> tlines, rlines;
    wrap(g_reply, 1.5f, rlines);
    bool show_transcript = (strncmp(g_status, "Understanding", 13) == 0);
    if (show_transcript) wrap(g_transcript, 1.5f, tlines);

    int y = body_y + 2;
    if (!tlines.empty()) {
        const int tline_px = (int)(8.0f * 1.5f) + 2;
        const int max_tlines = 2;
        int show = (int)tlines.size();
        if (show > max_tlines) show = max_tlines;
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1.5f);
        for (int i = 0; i < show; ++i) {
            std::string line = tlines[i];
            if (i == show - 1 && (int)tlines.size() > show) line += " ...";
            M5.Display.drawString(line.c_str(), text_x, y);
            y += tline_px;
        }
    }

    if (!rlines.empty()) {
        const int reply_size_px = (int)(8.0f * 1.5f) + 2;
        const int reply_room = (body_y + body_h) - y - 2;
        const int visible_lines = reply_room / reply_size_px;
        const int lines_to_show = visible_lines > 0 ? visible_lines : 1;

        int total = (int)rlines.size();
        int max_scroll = total - lines_to_show;
        if (max_scroll < 0) max_scroll = 0;
        // Clamp + cycle the auto-scroll offset (advanced in anim_tick).
        int off = g_reply_scroll;
        if (off > max_scroll) { off = 0; g_reply_scroll = 0; }

        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.5f);
        for (int i = 0; i < lines_to_show && off + i < total; ++i) {
            M5.Display.drawString(rlines[off + i].c_str(), text_x, y);
            y += reply_size_px;
        }
    }

    // Mic level pip in the bottom-right corner.
    int pip_r = 3 + (int)((g_last_rms / 8000.0f) * 5);
    if (pip_r > 8) pip_r = 8;
    M5.Display.fillRect(theme::SCREEN_W - 20, theme::SCREEN_H - 18, 20, 18,
                       to565(theme::IVORY));
    uint16_t pip_c = (g_state == State::LISTENING) ? to565(theme::BLUE)
                                                   : to565(theme::MID_GRAY);
    M5.Display.fillCircle(theme::SCREEN_W - 10, theme::SCREEN_H - 9, pip_r, pip_c);

    // Footer hint.
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("`  back", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void redraw() {
    paint_chrome();
    paint_labels();
}

void anim_tick() {
    int interval_ms;
    switch (g_state) {
        case State::IDLE:      interval_ms = 150; break;
        case State::LISTENING: interval_ms = 90;  break;
        case State::THINKING:  interval_ms = 50;  break;
        case State::SPEAKING:  interval_ms = 80;  break;
        case State::ERROR:     interval_ms = 0;   break;
    }
    uint32_t now = millis();
    if (interval_ms == 0 || now - g_last_anim_ms < (uint32_t)interval_ms) return;
    g_last_anim_ms = now;
    g_anim_phase += 1.0f;

    if (!g_spark_ready) {
        ui::Spark::create(g_spark);
        g_spark_ready = true;
    }
    int idx = (g_state == State::ERROR) ? 0 : ((int)g_anim_phase) % ui::burst::N_FRAMES;
    ui::Spark::draw_frame(g_spark, idx, theme::IVORY);
    // Spark lives at the left edge so the status banner above + text panel
    // to the right can both stay legible.
    g_spark.pushSprite(4, theme::CONTENT_TOP + 22);
}

void set_state(State s, const char* status) {
    g_state = s;
    if (status) {
        strncpy(g_status, status, sizeof(g_status) - 1);
        g_status[sizeof(g_status) - 1] = '\0';
    }
    redraw();
    // Force the spark to paint right now too. anim_tick normally handles it
    // from the main loop, but our pipeline blocks the loop during TTS
    // playback — without this kick the spark only appeared after the
    // audio had finished and the loop resumed.
    g_last_anim_ms = 0;
    anim_tick();
}

void start_listening() {
    g_pcm_pos = 0;
    g_silent_chunks = 0;
    g_heard_speech = false;
    g_warmup_chunks_left = 5;   // ~500 ms grace before VAD starts watching
    if (g_rec_file) g_rec_file.close();
    g_rec_file = LittleFS.open(REC_FILE_PATH, "w");
    if (!g_rec_file) {
        Serial.printf("[pocket] failed to open %s for write\n", REC_FILE_PATH);
        set_state(State::ERROR, "Recording file error");
        return;
    }
    audio::mic_open();
    set_state(State::LISTENING, "Speak now - OK to stop");
}

void run_pipeline() {
    if (WiFi.status() != WL_CONNECTED) {
        set_state(State::ERROR, "No WiFi");
        return;
    }
    set_state(State::THINKING, "Understanding...");
    std::string text = net::transcribe(REC_FILE_PATH, g_pcm_pos);
    if (text.empty()) {
        char st[48];
        snprintf(st, sizeof(st), "Whisper error (%d)", net::last_transcribe_status());
        set_state(State::ERROR, st);
        return;
    }
    strncpy(g_transcript, text.c_str(), sizeof(g_transcript) - 1);
    g_transcript[sizeof(g_transcript) - 1] = '\0';
    g_reply[0] = '\0';
    g_reply_scroll = 0;
    // Keep the 128 KB record buffer allocated across runs — re-malloc'ing it
    // each pipeline fragments the heap badly enough that the next allocation
    // fails. TLS calls are serialized so peak usage stays in budget.
    g_pcm_pos = 0;

    set_state(State::THINKING, "Thinking...");
    Serial.printf("[pocket] pre-claude heap=%u, text_len=%u, hist=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)text.size(),
                  (unsigned)g_history_json.size());

    auto on_token = [&](const std::string& s) {
        size_t cur = strlen(g_reply);
        size_t left = sizeof(g_reply) - 1 - cur;
        size_t take = s.size() < left ? s.size() : left;
        memcpy(g_reply + cur, s.data(), take);
        g_reply[cur + take] = '\0';
        paint_labels();
    };
    // Buffer the full reply first, then TTS it once — opening a second TLS
    // context while Claude's is still open is too tight on 320 KB SRAM.
    std::string full = net::claude_stream(text.c_str(), g_history_json,
                                          on_token,
                                          [](const std::string&){});
    Serial.printf("[pocket] claude returned %u chars, heap=%u\n",
                  (unsigned)full.size(), (unsigned)ESP.getFreeHeap());
    if (full.empty()) {
        set_state(State::ERROR, "Claude didn't respond");
        return;
    }

    // Append this turn to the conversation history so the next question
    // arrives with context — without this, each follow-up was treated as
    // a fresh chat and Claude couldn't reference what it just said.
    {
        JsonDocument hist;
        if (deserializeJson(hist, g_history_json) != DeserializationError::Ok ||
            !hist.is<JsonArray>()) {
            hist.clear();
            hist.to<JsonArray>();
        }
        JsonArray arr = hist.as<JsonArray>();
        JsonObject u = arr.add<JsonObject>();
        u["role"] = "user";
        u["content"] = text;
        JsonObject a = arr.add<JsonObject>();
        a["role"] = "assistant";
        a["content"] = full;
        // Cap to the last 10 messages (5 user + 5 assistant) so we don't
        // outgrow either Claude's context budget or our SRAM as the chat
        // continues.
        while (arr.size() > 10) arr.remove(0);
        g_history_json.clear();
        serializeJson(arr, g_history_json);
    }

    // Discard the recording data; the shared buffer now hosts TTS PCM.
    g_pcm_pos = 0;

    // Speak the reply automatically — no extra OK press needed. The text
    // already shows on screen via the streaming on_token callback, so the
    // user can read along while it's spoken. Two screen states cover the
    // wait: "Loading speech..." while the download blocks the main loop,
    // then "Speaking..." once playback actually starts.
    set_state(State::SPEAKING, "Loading speech...");

    // Free the recording's blocks before the TTS download grabs flash.
    // /rec.pcm (~160 KB) + /tts.pcm (up to ~1.4 MB) can otherwise blow
    // the 1.5 MB LittleFS partition and lfs_alloc divide-by-zeros.
    LittleFS.remove(REC_FILE_PATH);

    // Stage 1: download the entire TTS body to LittleFS. Talks straight to
    // OpenAI — no proxy. Read loop is byte-counted and ignores transient
    // empty reads, so mbedTLS chunk-gaps don't truncate the download. Pass
    // anim_tick as the tick callback so the spark keeps moving while the
    // main loop is parked inside tts_to_file.
    constexpr const char* TTS_FILE = "/tts.pcm";
    size_t tts_bytes = 0;
    bool tts_ok = net::tts_to_file(g_reply, TTS_FILE, &tts_bytes, &anim_tick);
    if (!tts_ok || tts_bytes == 0) {
        char st[64];
        snprintf(st, sizeof(st), "TTS error (%d)", net::last_tts_status());
        set_state(State::ERROR, st);
        return;
    }

    // Stage 2: play the file. The speaker reads off the flash one slab at
    // a time, so no network jitter can underrun it. Keep the spark
    // animated through the blocking drain via set_stream_tick().
    set_state(State::SPEAKING, "Speaking...");
    audio::set_stream_tick(&anim_tick);
    audio::stream_begin(24000, g_shared_buf, SHARED_SAMPLES);
    File f = LittleFS.open(TTS_FILE, "r");
    if (f) {
        // .bss buffer — loopTask only has an 8 KB stack and we're already
        // a few function frames deep here.
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
    // Hard-silence both halves of the codec so the amp doesn't keep
    // emitting low-level hiss between turns.
    audio::silence_codec();
    set_state(State::IDLE, "OK for next question");
}

void stop_and_process() {
    audio::mic_close();
    if (g_rec_file) {
        g_rec_file.close();
    }
    if (g_pcm_pos < MIN_RECORD_SAMPLES) {
        set_state(State::IDLE, "Too short - press OK again");
        return;
    }
    run_pipeline();
}

}  // namespace

void enter() {
    audio::begin();
    audio::set_volume(settings::store().volume_pct);
    g_state = State::IDLE;
    g_pcm_pos = 0;
    g_armed = false;
    g_silent_chunks = 0;
    g_last_rms = 0;
    g_last_anim_ms = millis();
    g_last_chunk_ms = millis();
    g_history_json = "[]";   // fresh chat on every entry to Pocket screen
    strncpy(g_status, "Press OK to talk", sizeof(g_status));
    g_transcript[0] = '\0';
    g_reply[0] = '\0';
    g_reply_scroll = 0;
    g_last_scroll_ms = millis();
    redraw();
}

void tick() {
    anim_tick();

    // Auto-advance reply scroll while idle or waiting-to-speak so long replies
    // cycle on screen.
    uint32_t now = millis();
    if (g_state == State::IDLE && g_reply[0] && now - g_last_scroll_ms > 1800) {
        g_last_scroll_ms = now;
        g_reply_scroll++;
        paint_labels();
    }

    auto status = M5Cardputer.Keyboard.keysState();
    bool held = status.enter || status.space;
    if (!held) g_armed = true;  // require a release before the next trigger

    // ` (backtick) = back. Any other key = log it (so we can see what the
    // keyboard actually delivers), and 't'/'T'/'0' = manual speaker test
    // tone independent of any pipeline / mic session, so we can isolate
    // whether the speaker itself is alive.
    for (auto k : status.word) {
        if (k == '`') {
            if (g_state == State::LISTENING) audio::mic_close();
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
    }

    // Tap-to-toggle: a fresh press while idle starts recording; another fresh
    // press while listening stops it. From ERROR, OK clears back to IDLE so
    // the user can retry without rebooting. g_armed prevents a single press
    // from counting twice across the IDLE → LISTENING boundary.
    if (held && g_armed) {
        g_armed = false;
        if (g_state == State::IDLE) {
            start_listening();
        } else if (g_state == State::LISTENING) {
            stop_and_process();
        } else if (g_state == State::ERROR) {
            g_transcript[0] = '\0';
            g_reply[0] = '\0';
            set_state(State::IDLE, "Press OK to talk");
        }
    }

    // While listening, pull a chunk every ~100 ms and do energy-based VAD.
    if (g_state == State::LISTENING) {
        uint32_t now = millis();
        if (now - g_last_chunk_ms >= 100 && g_pcm_pos + CHUNK_SAMPLES < MAX_RECORD_SAMPLES) {
            g_last_chunk_ms = now;
            size_t got = audio::mic_read_chunk(g_chunk_buf, CHUNK_SAMPLES);
            if (got > 0) {
                if (g_rec_file) g_rec_file.write((const uint8_t*)g_chunk_buf, got * 2);
                g_pcm_pos += got;
                g_last_rms = audio::last_level_rms();
                // Discard the first few chunks — the OK-key release thump
                // otherwise registers as "speech" before the user has said
                // anything, and the VAD then ends recording at the 1 s
                // minimum with nothing useful captured.
                if (g_warmup_chunks_left > 0) {
                    g_warmup_chunks_left--;
                    paint_labels();
                    return;
                }
                if (g_last_rms < SILENCE_RMS_THRESHOLD) {
                    g_silent_chunks++;
                } else {
                    g_silent_chunks = 0;
                    g_heard_speech = true;
                }
                paint_labels();
                if (g_heard_speech &&
                    g_pcm_pos >= MIN_RECORD_SAMPLES &&
                    g_silent_chunks >= SILENCE_CHUNKS_TO_END) {
                    stop_and_process();
                    return;
                }
                if (g_pcm_pos >= MAX_RECORD_SAMPLES) {
                    stop_and_process();
                    return;
                }
            }
        }
    }
}

}  // namespace pocket
