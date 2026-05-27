#include "app.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "apps/buddy.h"
#include "apps/radio.h"
#include "apps/orbit.h"
#include "apps/snake.h"
#include "apps/translate.h"
#include "apps/weather.h"
#include "audio/io.h"
#include "pocket/pocket.h"
#include "settings/screen.h"
#include "settings/store.h"
#include "theme.h"
#include "ui/launcher.h"
#include "ui/status_bar.h"

namespace app {

namespace {
Screen g_current = Screen::LAUNCHER;
Screen g_next = Screen::LAUNCHER;
bool   g_transition_pending = true;  // force first paint
uint32_t g_last_status_ms = 0;
}  // namespace

void init() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, /*kb=*/true);
    // M5Cardputer's begin() already sets rotation=1 and a sensible color depth
    // for the ST7789V2; calling setRotation/setColorDepth/invertDisplay here
    // corrupts the panel state and renders dark-gray as pink. Touch only
    // brightness and the fill color.
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(M5.Display.color888(0x1f, 0x1f, 0x1f));

    settings::load();
    // Apply persisted display brightness immediately so the splash screen
    // already comes up at the user's preferred level. Clamp to a minimum
    // of 10% so an accidental 0% doesn't render the device a black brick.
    {
        uint8_t pct = settings::store().brightness_pct;
        if (pct < 10) pct = 10;
        M5.Display.setBrightness((uint8_t)((uint16_t)pct * 255 / 100));
    }

    // Mount LittleFS for staging mic recordings — keeps the PCM out of
    // SRAM so we can record longer than the ~4 s the in-RAM buffer used
    // to allow. Auto-formats on first boot.
    if (!LittleFS.begin(/*formatOnFail=*/true)) {
        Serial.printf("[boot] LittleFS mount failed\n");
    } else {
        // One-shot cleanup of stale per-app PCM files from earlier
        // firmware versions. Every voice flow now shares /rec.pcm and
        // /tts.pcm — these older paths only wasted partition space.
        const char* stale[] = {
            "/buddy.pcm", "/buddy_tts.pcm",
            "/translate.pcm", "/translate_tts.pcm",
        };
        for (auto* p : stale) LittleFS.remove(p);
        Serial.printf("[boot] LittleFS mounted, total=%u, used=%u\n",
                      (unsigned)LittleFS.totalBytes(),
                      (unsigned)LittleFS.usedBytes());
    }

    // Silence the speaker side at boot: M5.begin leaves the ES8311 in a
    // state where it produces audible hiss on the Cardputer-Adv until either
    // a tone is played or Speaker.end() takes ownership.
    audio::begin();

    // Roam: try every previously-connected network at boot. We start with
    // `wifi_ssid` (the most recent), then walk `known_networks` in order if
    // it doesn't come up. Each attempt gets ~8 s — enough for typical home
    // / office routers, short enough that we don't sit on the boot screen
    // for half a minute if none of them are in range.
    auto try_connect_quick = [](const char* ssid, const char* pass) -> bool {
        if (!ssid || !*ssid) return false;
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid, pass);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
            delay(150);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[boot] WiFi: connected to %s\n", ssid);
            return true;
        }
        WiFi.disconnect(/*wifioff=*/false);
        return false;
    };

    const auto& s = settings::store();
    if (!try_connect_quick(s.wifi_ssid.c_str(), s.wifi_pass.c_str())) {
        for (auto& net : s.known_networks) {
            if (net.ssid == s.wifi_ssid) continue;   // already tried
            if (try_connect_quick(net.ssid.c_str(), net.pass.c_str())) {
                // Promote this one to "last used" so we try it first next
                // boot. Cheap: the in-memory list reorders too.
                settings::remember_network(net.ssid, net.pass);
                settings::save();
                break;
            }
        }
    }
}

void goto_screen(Screen s) {
    g_next = s;
    g_transition_pending = true;
}

Screen current() { return g_current; }

void loop() {
    M5Cardputer.update();

    if (g_transition_pending) {
        g_current = g_next;
        g_transition_pending = false;
        M5.Display.fillScreen(M5.Display.color888(0xfa, 0xf9, 0xf5));
        ui::StatusBar::draw(M5.Display);
        switch (g_current) {
            case Screen::LAUNCHER:  ui::Launcher::enter();    break;
            case Screen::POCKET:    pocket::enter();          break;
            case Screen::BUDDY:     apps::buddy::enter();     break;
            case Screen::SNAKE:     apps::snake::enter();     break;
            case Screen::ORBIT:     apps::orbit::enter();     break;
            case Screen::WEATHER:   apps::weather::enter();   break;
            case Screen::RADIO:     apps::radio::enter();     break;
            case Screen::TRANSLATE: apps::translate::enter(); break;
            case Screen::SETTINGS:  settings::enter();        break;
        }
    }

    switch (g_current) {
        case Screen::LAUNCHER:  ui::Launcher::tick();    break;
        case Screen::POCKET:    pocket::tick();          break;
        case Screen::BUDDY:     apps::buddy::tick();     break;
        case Screen::SNAKE:     apps::snake::tick();     break;
        case Screen::ORBIT:     apps::orbit::tick();     break;
        case Screen::WEATHER:   apps::weather::tick();   break;
        case Screen::RADIO:     apps::radio::tick();     break;
        case Screen::TRANSLATE: apps::translate::tick(); break;
        case Screen::SETTINGS:  settings::tick();        break;
    }

    uint32_t now = millis();
    if (now - g_last_status_ms > 1000) {
        ui::StatusBar::draw(M5.Display);
        g_last_status_ms = now;
    }
}

}  // namespace app
