#include "keys.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WString.h>

#include "../config.h"

namespace app {
namespace keys {

namespace {

// Cardputer microSD pins. The Adv uses the same TF-card wiring as the base
// Cardputer: CLK=40, MISO=39, MOSI=14, CS=12. If they ever change on a
// future hardware revision, SD.begin() will simply fail and we skip the
// import — non-fatal.
constexpr int SD_PIN_SCK  = 40;
constexpr int SD_PIN_MISO = 39;
constexpr int SD_PIN_MOSI = 14;
constexpr int SD_PIN_CS   = 12;

constexpr const char* NVS_NS    = "keys";
constexpr const char* KEYS_PATH = "/keys.txt";

String g_anthropic;
String g_openai;
String g_wifi_ssid;
String g_wifi_pass;
bool   g_loaded = false;

void load_from_nvs() {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) {
        g_loaded = true;
        return;
    }
    g_anthropic = p.getString("anthropic_key", "");
    g_openai    = p.getString("openai_key",    "");
    g_wifi_ssid = p.getString("wifi_ssid",     "");
    g_wifi_pass = p.getString("wifi_pass",     "");
    p.end();
    g_loaded = true;
}

// Trim ASCII whitespace + strip a single trailing \r (Windows line endings).
void trim_inplace(String& s) {
    while (s.length() > 0 && (s[0] == ' ' || s[0] == '\t')) s.remove(0, 1);
    while (s.length() > 0) {
        char c = s[s.length() - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s.remove(s.length() - 1, 1);
        else break;
    }
}

// Parse one "KEY=VALUE" line. Skips comments (#...) and empty lines.
// Returns true if a recognized key was applied to NVS.
bool apply_line(const String& raw, Preferences& p) {
    String line = raw;
    trim_inplace(line);
    if (line.length() == 0 || line[0] == '#') return false;

    int eq = line.indexOf('=');
    if (eq <= 0) return false;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    trim_inplace(key);
    trim_inplace(val);
    if (key.length() == 0 || val.length() == 0) return false;

    // Strip optional surrounding quotes — friendly for users who paste keys
    // verbatim from environment exports.
    if (val.length() >= 2) {
        char a = val[0];
        char b = val[val.length() - 1];
        if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
            val = val.substring(1, val.length() - 1);
        }
    }

    if      (key == "ANTHROPIC_API_KEY") p.putString("anthropic_key", val);
    else if (key == "OPENAI_API_KEY")    p.putString("openai_key",    val);
    else if (key == "WIFI_SSID")         p.putString("wifi_ssid",     val);
    else if (key == "WIFI_PASS")         p.putString("wifi_pass",     val);
    else {
        Serial.printf("[keys] unknown field, ignored: %s\n", key.c_str());
        return false;
    }
    return true;
}

}  // namespace

int import_from_sd() {
    SPIClass spi(HSPI);
    spi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);

    if (!SD.begin(SD_PIN_CS, spi, 4000000)) {
        Serial.println("[keys] no SD card or mount failed — skipping import");
        return 0;
    }
    if (!SD.exists(KEYS_PATH)) {
        Serial.println("[keys] SD mounted but /keys.txt not present — skipping");
        SD.end();
        return 0;
    }

    File f = SD.open(KEYS_PATH, FILE_READ);
    if (!f) {
        Serial.println("[keys] /keys.txt found but could not open");
        SD.end();
        return 0;
    }

    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[keys] NVS open for write failed");
        f.close();
        SD.end();
        return 0;
    }

    int applied = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (apply_line(line, p)) applied++;
    }
    p.end();
    f.close();
    SD.end();

    Serial.printf("[keys] imported %d field(s) from /keys.txt\n", applied);
    // Re-read into cache so the rest of boot picks up the new values.
    reload();
    return applied;
}

void reload() {
    g_loaded = false;
    load_from_nvs();
}

const char* anthropic_key() {
    if (!g_loaded) load_from_nvs();
    return g_anthropic.length() > 0 ? g_anthropic.c_str() : ANTHROPIC_API_KEY;
}

const char* openai_key() {
    if (!g_loaded) load_from_nvs();
    return g_openai.length() > 0 ? g_openai.c_str() : OPENAI_API_KEY;
}

const char* wifi_ssid() {
    if (!g_loaded) load_from_nvs();
    return g_wifi_ssid.length() > 0 ? g_wifi_ssid.c_str() : DEFAULT_WIFI_SSID;
}

const char* wifi_pass() {
    if (!g_loaded) load_from_nvs();
    return g_wifi_pass.length() > 0 ? g_wifi_pass.c_str() : DEFAULT_WIFI_PASS;
}

}  // namespace keys
}  // namespace app
