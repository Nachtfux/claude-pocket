#include "radio.h"

#include <Audio.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "../app.h"
#include "../audio/io.h"
#include "../settings/store.h"
#include "../theme.h"

using settings::store;
using settings::save;

namespace apps {
namespace radio {

namespace {

struct Station {
    const char* name;
    const char* url;
};

// User-curated station list. URLs verified against the radio-browser.info
// catalogue (de1.api.radio-browser.info). HTTP MP3 where available — the
// Audio library handles HTTPS+AAC too, but plain HTTP MP3 is friendliest
// on this no-PSRAM chip. AIDA only publishes an HTTPS AAC stream.
// AIDA Radio was originally in this list but only ships an HE-AAC stream
// (with SBR). The libhelix-aac decoder needs ~50 KB of contiguous heap
// at init, which we don't have after mbedTLS finishes its TLS-1.2
// handshake. Without PSRAM there's no obvious workaround; tried, OOM'd,
// removed. If AIDA ever publishes an MP3 mirror we can put it back.
constexpr Station STATIONS[] = {
    // User picks (Germany / DACH)
    {"Antenne 1 Heilbronn",   "http://stream.antenne1.de/a1hn/livestream2.mp3"},
    {"Antenne Bayern Chill",  "http://mp3channels.webradio.antenne.de/chillout"},
    {"Charivari",             "http://rs5.stream24.net/stream"},
    {"Inselradio Mallorca",   "http://inselradiomallorca.stream06.radiohost.de/inselradiomallorca-live_mp3-128"},
    // Popular worldwide — all MP3 HTTP so the libhelix-mp3 path can decode
    // them on this no-PSRAM chip. URLs picked from radio-browser's top
    // click counts and double-checked manually.
    {"1LIVE",                 "http://wdr-1live-live.icecast.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
    {"BBC World Service",     "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
    {"NPR News",              "http://npr-ice.streamguys1.com/live.mp3"},
    {"KEXP 90.3 Seattle",     "http://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
    {"Radio Paradise",        "http://stream.radioparadise.com/mp3-128"},
    {"SomaFM Groove Salad",   "http://ice1.somafm.com/groovesalad-128-mp3"},
};
constexpr int N_STATIONS = sizeof(STATIONS) / sizeof(STATIONS[0]);

// ES8311 I2S pins per Cardputer-Adv silkscreen / SPEC.
constexpr int I2S_BCLK = 41;
constexpr int I2S_LRCK = 43;
constexpr int I2S_DOUT = 42;

Audio  g_audio;
int    g_sel        = 0;
bool   g_playing    = false;
bool   g_codec_done = false;
char   g_status_line[64] = {0};

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void ensure_codec_configured() {
    if (g_codec_done) return;
    Serial.printf("[radio] codec setup begin, heap=%u\n",
                  (unsigned)ESP.getFreeHeap());
    // Make absolutely sure nobody else still owns the I2S port — Pocket
    // / Translator might have left M5.Speaker or M5.Mic half-installed
    // and a second Audio-library install_driver() on the same port
    // hard-faults the CPU. silence_codec() ends both halves, then we let
    // M5.Speaker do its I2C codec config one more time and release I2S.
    audio::silence_codec();
    delay(50);
    M5.Speaker.begin();
    delay(20);
    M5.Speaker.end();
    delay(50);
    g_audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
    g_codec_done = true;
    Serial.printf("[radio] codec setup done, heap=%u\n",
                  (unsigned)ESP.getFreeHeap());
}

void apply_volume() {
    int vol_pct = store().volume_pct;
    // Audio library wants 0..21 (its own log curve).
    int v = (vol_pct * 21) / 100;
    if (v > 21) v = 21;
    g_audio.setVolume(v);
}

void bump_volume(int delta_pct) {
    int v = (int)store().volume_pct + delta_pct;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    store().volume_pct = (uint8_t)v;
    save();
    if (g_playing) apply_volume();
    snprintf(g_status_line, sizeof(g_status_line), "Volume %d%%", v);
}

void start_current() {
    apply_volume();
    snprintf(g_status_line, sizeof(g_status_line), "Connecting...");
    Serial.printf("[radio] connecting %s — heap=%u\n",
                  STATIONS[g_sel].url, (unsigned)ESP.getFreeHeap());
    bool ok = g_audio.connecttohost(STATIONS[g_sel].url);
    Serial.printf("[radio] connecttohost returned %d, heap=%u\n",
                  (int)ok, (unsigned)ESP.getFreeHeap());
    g_playing = true;
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));

    // Station name — big, centered, dark text.
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.8f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(STATIONS[g_sel].name, theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 14);

    // Play state — orange when active, gray when stopped.
    M5.Display.setTextColor(to565(g_playing ? theme::ORANGE : theme::MID_GRAY),
                            to565(theme::IVORY));
    M5.Display.setTextSize(1.5f);
    M5.Display.drawString(g_playing ? "▶ Playing" : "■ Stopped",
                          theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 40);

    // Optional status line ("Connecting...", "Volume 75%", etc.) — yellow
    // so transient value changes pop against the cream-on-dark UI.
    if (g_status_line[0]) {
        M5.Display.setTextColor(to565(theme::YELLOW), to565(theme::IVORY));
        M5.Display.setTextSize(1.2f);
        M5.Display.drawString(g_status_line, theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 64);
    }

    // Position pill (1/5, 2/5, …) at the bottom-center.
    char pos[16];
    snprintf(pos, sizeof(pos), "%d / %d", g_sel + 1, N_STATIONS);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString(pos, theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 84);

    // Footer hint.
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("W/S stn  </>  vol  OK play  `  back",
                          4, theme::SCREEN_H - 2);
}

}  // namespace

void enter() {
    if (WiFi.status() != WL_CONNECTED) {
        M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W,
                            theme::CONTENT_H, to565(theme::IVORY));
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.5f);
        M5.Display.setTextDatum(top_center);
        M5.Display.drawString("No WiFi", theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 30);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1);
        M5.Display.drawString("`  back", theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 60);
        return;
    }
    ensure_codec_configured();
    paint();
}

void leave() {
    if (g_playing) {
        g_audio.stopSong();
        g_playing = false;
    }
    g_status_line[0] = '\0';
    // stopSong() halts the decoder but leaves the codec + amp powered,
    // which hisses on the NS4150 side. silence_codec() takes both halves
    // of the ES8311 down so the speaker is truly quiet on the launcher.
    audio::silence_codec();
    // Next time we enter Radio we'll need to reconfigure the codec.
    g_codec_done = false;
}

void tick() {
    // Pump the MP3 decoder + I2S writer; this is what actually plays audio.
    if (g_playing) g_audio.loop();

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();
    // OK / Enter / Space sit on dedicated boolean flags, NOT in the
    // printable `word` vector — the launcher handles them the same way.
    if (status.enter || status.space) {
        if (g_playing) {
            g_audio.stopSong();
            g_playing = false;
            g_status_line[0] = '\0';
        } else {
            start_current();
        }
        paint();
        return;
    }
    for (auto k : status.word) {
        if (k == '`') {
            leave();
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if (k == 'w' || k == ';') {        // up arrow → prev station
            bool was = g_playing;
            if (g_playing) { g_audio.stopSong(); g_playing = false; }
            g_sel = (g_sel - 1 + N_STATIONS) % N_STATIONS;
            paint();
            if (was) start_current();
            paint();
        } else if (k == 's' || k == '.') {  // down arrow → next station
            bool was = g_playing;
            if (g_playing) { g_audio.stopSong(); g_playing = false; }
            g_sel = (g_sel + 1) % N_STATIONS;
            paint();
            if (was) start_current();
            paint();
        } else if (k == ',' || k == 'a') {  // left arrow → volume down
            bump_volume(-5);
            paint();
        } else if (k == '/' || k == 'd') {  // right arrow → volume up
            bump_volume(+5);
            paint();
        }
    }
}

}  // namespace radio
}  // namespace apps

// Weak callbacks the Audio library calls back into from the streaming
// pipeline. We just echo them to serial — useful for diagnosing why a
// station refuses to play. Plain C++ linkage matching the library's
// own declarations in Audio.h.
void audio_info(const char* info) {
    if (info) Serial.printf("[radio] %s\n", info);
}
void audio_showstation(const char* info) {
    if (info) Serial.printf("[radio] station=%s\n", info);
}
void audio_showstreamtitle(const char* info) {
    if (info) Serial.printf("[radio] title=%s\n", info);
}
void audio_eof_stream(const char* info) {
    Serial.printf("[radio] eof: %s\n", info ? info : "(null)");
}
