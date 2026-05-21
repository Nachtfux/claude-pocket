#include "weather.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <string>

#include "../app.h"
#include "../theme.h"
#include "../ui/burst_frames.h"
#include "../ui/spark.h"

namespace apps {
namespace weather {

namespace {

enum class State { LOADING, READY, ERROR };

struct Day {
    int code = 0;
    int tmin = 0;
    int tmax = 0;
    char label[8] = {0};
};

struct Data {
    char  city[32]    = {0};
    float lat         = 0.0f;
    float lon         = 0.0f;
    float temp_now    = 0.0f;
    int   code_now    = 0;
    Day   forecast[3] = {};
};

State    g_state = State::LOADING;
Data     g_data;
char     g_error[64]   = {0};
M5Canvas g_spark(&M5.Display);
bool     g_spark_ready = false;
float    g_anim_phase  = 0.0f;
uint32_t g_last_anim_ms = 0;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// Open-Meteo weather code → short label. Buckets are taken from the WMO
// table at https://open-meteo.com/en/docs (current_weather.weathercode).
const char* code_to_label(int code) {
    if (code == 0)                       return "Clear";
    if (code >= 1  && code <= 3)         return "Cloudy";
    if (code == 45 || code == 48)        return "Fog";
    if (code >= 51 && code <= 67)        return "Rain";
    if (code >= 71 && code <= 77)        return "Snow";
    if (code >= 80 && code <= 82)        return "Showers";
    if (code >= 95)                      return "Storm";
    return "—";
}

// Compute weekday abbreviation ("Mon".."Sun") from an ISO-8601 yyyy-mm-dd
// date string using Zeller's congruence. Avoids dragging in a full date
// library for what is one date per forecast row.
void weekday_abbrev(const char* iso_date, char* out, size_t out_n) {
    if (!iso_date || strlen(iso_date) < 10) { strncpy(out, "?", out_n); return; }
    int y = atoi(iso_date);
    int m = atoi(iso_date + 5);
    int d = atoi(iso_date + 8);
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + 13 * (m + 1) / 5 + K + K/4 + J/4 + 5*J) % 7;
    // Zeller: 0=Sat,1=Sun,2=Mon,...,6=Fri
    static const char* days[] = { "Sat","Sun","Mon","Tue","Wed","Thu","Fri" };
    strncpy(out, days[h], out_n);
    out[out_n - 1] = '\0';
}

// Decode an HTTP chunked-transfer-encoded payload in place. open-meteo
// (Cloudflare edge) responds with chunked even for short JSON; without
// stripping the "HEX\r\n…data…\r\n" framing ArduinoJson rejects the body
// as invalid input.
std::string decode_chunked(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        size_t crlf = in.find("\r\n", pos);
        if (crlf == std::string::npos) break;
        std::string hex = in.substr(pos, crlf - pos);
        // Trim chunk extensions after a ';' or whitespace.
        size_t semi = hex.find_first_of("; \t");
        if (semi != std::string::npos) hex.resize(semi);
        size_t sz = strtoul(hex.c_str(), nullptr, 16);
        if (sz == 0) break;
        pos = crlf + 2;
        if (pos + sz > in.size()) break;
        out.append(in, pos, sz);
        pos += sz + 2;     // skip data + trailing \r\n
    }
    return out;
}

// Read an HTTP response from `c`, splitting headers from body, and append
// the body to `body`. If the response uses Transfer-Encoding: chunked the
// chunked framing is decoded out before returning. Returns true if the
// header terminator was seen.
template <class Client>
bool http_read_body(Client& c, std::string& body, uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    std::string headers;
    headers.reserve(512);
    bool header_done = false;
    bool chunked     = false;
    std::string raw_body;   // pre-decode body if chunked
    while (millis() < deadline) {
        int n = c.available();
        if (n <= 0) {
            if (!c.connected() && c.available() == 0) break;
            delay(5);
            continue;
        }
        char tmp[256];
        int r = c.read((uint8_t*)tmp, n > (int)sizeof(tmp) ? (int)sizeof(tmp) : n);
        if (r <= 0) continue;
        if (!header_done) {
            headers.append(tmp, r);
            size_t e = headers.find("\r\n\r\n");
            if (e != std::string::npos) {
                std::string hdrs_lower = headers.substr(0, e);
                for (auto& cc : hdrs_lower) cc = (char)tolower(cc);
                chunked = hdrs_lower.find("transfer-encoding: chunked")
                          != std::string::npos;
                if (headers.size() > e + 4) {
                    if (chunked) raw_body.append(headers.data() + e + 4,
                                                 headers.size() - e - 4);
                    else         body.append(headers.data() + e + 4,
                                             headers.size() - e - 4);
                }
                headers.clear();
                headers.shrink_to_fit();
                header_done = true;
            }
        } else {
            if (chunked) raw_body.append(tmp, r);
            else         body.append(tmp, r);
        }
    }
    if (chunked) body.append(decode_chunked(raw_body));
    return header_done;
}

bool fetch_location() {
    WiFiClient c;
    c.setTimeout(8);
    if (!c.connect("ip-api.com", 80, 5000)) {
        Serial.printf("[wx] ip-api connect failed\n");
        return false;
    }
    c.print("GET /json HTTP/1.1\r\n"
            "Host: ip-api.com\r\n"
            "User-Agent: claude-pocket/0.1\r\n"
            "Connection: close\r\n\r\n");
    std::string body;
    body.reserve(1024);
    bool ok = http_read_body(c, body, 8000);
    c.stop();
    Serial.printf("[wx] ip-api ok=%d, body=%u bytes\n",
                  (int)ok, (unsigned)body.size());
    if (!ok || body.empty()) return false;
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err != DeserializationError::Ok) {
        Serial.printf("[wx] ip-api json err: %s, body=%.200s\n",
                      err.c_str(), body.c_str());
        return false;
    }
    const char* city = doc["city"];
    if (!city || !*city) {
        Serial.printf("[wx] ip-api no city, body=%.200s\n", body.c_str());
        return false;
    }
    strncpy(g_data.city, city, sizeof(g_data.city) - 1);
    g_data.city[sizeof(g_data.city) - 1] = '\0';
    g_data.lat = doc["lat"].as<float>();
    g_data.lon = doc["lon"].as<float>();
    Serial.printf("[wx] city=%s lat=%.3f lon=%.3f\n",
                  g_data.city, g_data.lat, g_data.lon);
    return true;
}

bool fetch_weather() {
    WiFiClientSecure c;
    c.setInsecure();
    c.setHandshakeTimeout(10);
    c.setTimeout(15);
    if (!c.connect("api.open-meteo.com", 443, 12000)) {
        Serial.printf("[wx] open-meteo connect failed\n");
        return false;
    }
    // Build the request URL separately — fitting GET line + headers into
    // one 256-byte snprintf truncated the request and the server hung up
    // before responding.
    char query[192];
    snprintf(query, sizeof(query),
             "/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current_weather=true"
             "&daily=weathercode,temperature_2m_max,temperature_2m_min"
             "&forecast_days=3&timezone=auto",
             g_data.lat, g_data.lon);
    c.print("GET ");
    c.print(query);
    c.print(" HTTP/1.1\r\n"
            "Host: api.open-meteo.com\r\n"
            "User-Agent: claude-pocket/0.1\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n");
    std::string body;
    body.reserve(2048);
    bool ok = http_read_body(c, body, 15000);
    c.stop();
    Serial.printf("[wx] open-meteo ok=%d, body=%u bytes\n",
                  (int)ok, (unsigned)body.size());
    if (!ok || body.empty()) {
        Serial.printf("[wx] open-meteo empty body, partial=%.200s\n",
                      body.c_str());
        return false;
    }
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err != DeserializationError::Ok) {
        Serial.printf("[wx] open-meteo json err: %s, body=%.300s\n",
                      err.c_str(), body.c_str());
        return false;
    }
    auto cur = doc["current_weather"];
    g_data.temp_now = cur["temperature"].as<float>();
    g_data.code_now = cur["weathercode"].as<int>();
    auto daily = doc["daily"];
    for (int i = 0; i < 3; ++i) {
        g_data.forecast[i].code = daily["weathercode"][i].as<int>();
        g_data.forecast[i].tmax = (int)daily["temperature_2m_max"][i].as<float>();
        g_data.forecast[i].tmin = (int)daily["temperature_2m_min"][i].as<float>();
        const char* date = daily["time"][i];
        weekday_abbrev(date, g_data.forecast[i].label,
                       sizeof(g_data.forecast[i].label));
    }
    return true;
}

void draw_spark(int x, int y) {
    if (!g_spark_ready) {
        ui::Spark::create(g_spark);
        g_spark_ready = true;
    }
    int idx = ((int)g_anim_phase) % ui::burst::N_FRAMES;
    ui::Spark::draw_frame(g_spark, idx, theme::IVORY);
    g_spark.pushSprite(x, y);
}

void paint_loading() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    draw_spark(theme::SCREEN_W / 2 - 24, theme::CONTENT_TOP + 14);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Loading weather...", theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 78);
}

void paint_error() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.5f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(g_error, theme::SCREEN_W / 2, theme::CONTENT_TOP + 24);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("R retry   `  back", 4, theme::SCREEN_H - 2);
}

void paint_ready() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    // Top row — animated spark on the left, city + condition next to it.
    draw_spark(4, theme::CONTENT_TOP + 6);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.5f);
    M5.Display.drawString(g_data.city, 64, theme::CONTENT_TOP + 6);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1.2f);
    M5.Display.drawString(code_to_label(g_data.code_now), 64,
                          theme::CONTENT_TOP + 26);

    // Hero current temperature in Orange, right of the spark.
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%d°", (int)g_data.temp_now);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(3.0f);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(temp_buf, theme::SCREEN_W - 8,
                          theme::CONTENT_TOP + 6);

    // Divider line so the forecast row feels like its own block.
    int fy_top = theme::CONTENT_TOP + 60;
    M5.Display.drawFastHLine(8, fy_top - 2, theme::SCREEN_W - 16,
                             to565(theme::LIGHT_GRAY));

    // 3-day forecast row.
    const int cols = 3;
    const int col_w = (theme::SCREEN_W - 16) / cols;
    M5.Display.setTextDatum(top_center);
    for (int i = 0; i < cols; ++i) {
        int cx = 8 + col_w * i + col_w / 2;
        // weekday
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.2f);
        M5.Display.drawString(g_data.forecast[i].label, cx, fy_top + 4);
        // condition label
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1);
        M5.Display.drawString(code_to_label(g_data.forecast[i].code),
                              cx, fy_top + 20);
        // hi/lo, hi in DARK, lo in MID_GRAY
        char hl[16];
        snprintf(hl, sizeof(hl), "%d°/%d°", g_data.forecast[i].tmax,
                 g_data.forecast[i].tmin);
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.2f);
        M5.Display.drawString(hl, cx, fy_top + 32);
    }

    // Footer hint
    M5.Display.setTextDatum(bottom_left);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.drawString("R refresh   `  back", 4, theme::SCREEN_H - 2);
}

void refresh() {
    g_state = State::LOADING;
    paint_loading();
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(g_error, "No WiFi", sizeof(g_error));
        g_state = State::ERROR;
        paint_error();
        return;
    }
    if (!fetch_location()) {
        strncpy(g_error, "Location lookup failed", sizeof(g_error));
        g_state = State::ERROR;
        paint_error();
        return;
    }
    if (!fetch_weather()) {
        strncpy(g_error, "Weather fetch failed", sizeof(g_error));
        g_state = State::ERROR;
        paint_error();
        return;
    }
    g_state = State::READY;
    paint_ready();
}

}  // namespace

void enter() {
    refresh();
}

void tick() {
    // Spark animation — same calm 150 ms / frame rhythm as the launcher.
    uint32_t now = millis();
    if (g_state != State::ERROR && now - g_last_anim_ms >= 150) {
        g_last_anim_ms = now;
        g_anim_phase += 1.0f;
        if (g_state == State::READY) {
            draw_spark(4, theme::CONTENT_TOP + 6);
        } else if (g_state == State::LOADING) {
            draw_spark(theme::SCREEN_W / 2 - 24, theme::CONTENT_TOP + 14);
        }
    }

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();
    for (auto k : status.word) {
        if (k == '`') {
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if (k == 'r' || k == 'R') {
            refresh();
            return;
        }
    }
}

}  // namespace weather
}  // namespace apps
