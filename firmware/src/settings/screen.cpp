#include "screen.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "../app.h"
#include "../audio/io.h"
#include "../theme.h"
#include "store.h"

namespace settings {

namespace {

enum class Section { MENU, WIFI_SCAN, WIFI_PASS, BLUETOOTH, VOLUME,
                     BRIGHTNESS, ABOUT };

struct Row {
    const char* label;
    Section target;
};

constexpr Row ROWS[] = {
    {"WiFi",       Section::WIFI_SCAN},
    {"Bluetooth",  Section::BLUETOOTH},
    {"Volume",     Section::VOLUME},
    {"Brightness", Section::BRIGHTNESS},
    {"About",      Section::ABOUT},
};
constexpr int N_ROWS = sizeof(ROWS) / sizeof(ROWS[0]);

// Min brightness clamp — accidentally going to 0% would render the display
// black and the device unrecoverable without flashing. 10% (~25/255) is
// still readable in a dim room.
constexpr uint8_t MIN_BRIGHTNESS_PCT = 10;

void apply_display_brightness(uint8_t pct) {
    if (pct < MIN_BRIGHTNESS_PCT) pct = MIN_BRIGHTNESS_PCT;
    M5.Display.setBrightness((uint8_t)((uint16_t)pct * 255 / 100));
}

Section g_section = Section::MENU;
int g_sel = 0;

// WiFi state
int g_scan_count = 0;
int g_scan_sel = 0;
int g_scan_scroll = 0;
bool g_scan_in_progress = false;
char g_pass_buf[65] = "";
int  g_pass_len = 0;
char g_picked_ssid[33] = "";
char g_status_line[64] = "";

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void fill_content(uint32_t bg) {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H, to565(bg));
}

void paint_menu() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("Settings", 8, theme::CONTENT_TOP + 4);

    // 5 entries × 18 px = 90 px after a 26 px header — fits the 119 px
    // content area without scrolling.
    const int list_y = theme::CONTENT_TOP + 26;
    const int row_h  = 18;
    for (int i = 0; i < N_ROWS; ++i) {
        int ry = list_y + i * row_h;
        bool sel = (i == g_sel);
        if (sel) {
            M5.Display.fillRoundRect(4, ry - 1, theme::SCREEN_W - 8, row_h - 2, 4,
                                     to565(theme::LIGHT_GRAY));
            M5.Display.fillRect(4, ry - 1, 3, row_h - 2, to565(theme::ORANGE));
        }
        M5.Display.setTextColor(to565(theme::DARK),
                                sel ? to565(theme::LIGHT_GRAY) : to565(theme::IVORY));
        M5.Display.setTextSize(1.2f);
        M5.Display.drawString(ROWS[i].label, 12, ry + 1);

        const char* hint = "";
        char tmp[40];
        if (ROWS[i].target == Section::WIFI_SCAN) {
            hint = WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "disconnected";
        } else if (ROWS[i].target == Section::BLUETOOTH) {
            hint = store().bluetooth_on ? "on" : "off";
        } else if (ROWS[i].target == Section::VOLUME) {
            snprintf(tmp, sizeof(tmp), "%d%%", store().volume_pct);
            hint = tmp;
        } else if (ROWS[i].target == Section::BRIGHTNESS) {
            snprintf(tmp, sizeof(tmp), "%d%%", store().brightness_pct);
            hint = tmp;
        }
        M5.Display.setTextColor(to565(theme::MID_GRAY),
                                sel ? to565(theme::LIGHT_GRAY) : to565(theme::IVORY));
        M5.Display.setTextDatum(top_right);
        M5.Display.drawString(hint, theme::SCREEN_W - 12, ry + 1);
        M5.Display.setTextDatum(top_left);
    }

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("ESC back   ENTER select", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_volume() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("Volume", 8, theme::CONTENT_TOP + 4);

    int pct = store().volume_pct;
    const int bar_x = 16;
    const int bar_y = theme::CONTENT_TOP + 56;
    const int bar_w = theme::SCREEN_W - 32;
    const int bar_h = 14;
    M5.Display.drawRoundRect(bar_x, bar_y, bar_w, bar_h, 6, to565(theme::DARK));
    M5.Display.fillRoundRect(bar_x + 2, bar_y + 2, (bar_w - 4) * pct / 100, bar_h - 4, 5,
                             to565(theme::ORANGE));

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d%%", pct);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(tmp, theme::SCREEN_W / 2, bar_y + bar_h + 6);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("<- ->  ENTER save  ESC", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_brightness() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("Brightness", 8, theme::CONTENT_TOP + 4);

    int pct = store().brightness_pct;
    const int bar_x = 16;
    const int bar_y = theme::CONTENT_TOP + 56;
    const int bar_w = theme::SCREEN_W - 32;
    const int bar_h = 14;
    M5.Display.drawRoundRect(bar_x, bar_y, bar_w, bar_h, 6, to565(theme::DARK));
    M5.Display.fillRoundRect(bar_x + 2, bar_y + 2, (bar_w - 4) * pct / 100, bar_h - 4, 5,
                             to565(theme::ORANGE));

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d%%", pct);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(tmp, theme::SCREEN_W / 2, bar_y + bar_h + 6);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("<- ->  ENTER save  ESC", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_about() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("About", 8, theme::CONTENT_TOP + 4);

    M5.Display.setTextSize(1);
    int y = theme::CONTENT_TOP + 26;
    char val[48];

    auto row = [&](const char* label, const char* value) {
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.drawString(label, 8, y);
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.drawString(value, 80, y);
        y += 11;
    };

    row("Firmware", CLAUDE_POCKET_VERSION);

    snprintf(val, sizeof(val), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
    row("Heap free", val);

    uint32_t up_s = millis() / 1000;
    if (up_s >= 3600) snprintf(val, sizeof(val), "%uh %um",
                               (unsigned)(up_s / 3600), (unsigned)((up_s / 60) % 60));
    else              snprintf(val, sizeof(val), "%um %us",
                               (unsigned)(up_s / 60), (unsigned)(up_s % 60));
    row("Uptime", val);

    if (WiFi.status() == WL_CONNECTED) {
        row("IP", WiFi.localIP().toString().c_str());
        snprintf(val, sizeof(val), "%d dBm", WiFi.RSSI());
        row("WiFi RSSI", val);
    } else {
        row("WiFi", "disconnected");
    }
    row("MAC", WiFi.macAddress().c_str());

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("`  back", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_bluetooth() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("Bluetooth", 8, theme::CONTENT_TOP + 4);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(store().bluetooth_on ? theme::GREEN : theme::MID_GRAY),
                            to565(theme::IVORY));
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(store().bluetooth_on ? "ON" : "OFF",
                          theme::SCREEN_W / 2, theme::CONTENT_TOP + 60);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("ENTER toggle   ESC back", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_wifi_scan() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("WiFi", 8, theme::CONTENT_TOP + 4);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));

    if (g_scan_in_progress) {
        M5.Display.setTextDatum(middle_center);
        M5.Display.drawString("Scanning networks...", theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 60);
        M5.Display.setTextDatum(top_left);
        return;
    }
    if (g_scan_count <= 0) {
        M5.Display.setTextDatum(middle_center);
        M5.Display.drawString("No networks found",
                              theme::SCREEN_W / 2, theme::CONTENT_TOP + 50);
        M5.Display.drawString("R = rescan   ESC = back",
                              theme::SCREEN_W / 2, theme::CONTENT_TOP + 70);
        M5.Display.setTextDatum(top_left);
        return;
    }

    const int list_y = theme::CONTENT_TOP + 28;
    const int row_h = 14;
    const int visible = 6;
    for (int i = 0; i < visible && (g_scan_scroll + i) < g_scan_count; ++i) {
        int idx = g_scan_scroll + i;
        int ry = list_y + i * row_h;
        bool sel = (idx == g_scan_sel);
        uint16_t bg = sel ? to565(theme::LIGHT_GRAY) : to565(theme::IVORY);
        M5.Display.fillRect(4, ry, theme::SCREEN_W - 8, row_h, bg);
        if (sel) M5.Display.fillRect(4, ry, 3, row_h, to565(theme::ORANGE));
        char line[40];
        snprintf(line, sizeof(line), "%s  %ddBm%s",
                 WiFi.SSID(idx).c_str(),
                 WiFi.RSSI(idx),
                 WiFi.encryptionType(idx) == WIFI_AUTH_OPEN ? "" : " *");
        M5.Display.setTextColor(to565(theme::DARK), bg);
        M5.Display.drawString(line, 10, ry + 2);
    }

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("ENTER select  R rescan  ESC back", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void paint_pass_entry() {
    fill_content(theme::IVORY);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("Password for", 8, theme::CONTENT_TOP + 4);
    M5.Display.setTextSize(2);
    M5.Display.drawString(g_picked_ssid, 8, theme::CONTENT_TOP + 16);

    char masked[68];
    int n = g_pass_len < 64 ? g_pass_len : 64;
    for (int i = 0; i < n; ++i) masked[i] = '*';
    masked[n] = '_';
    masked[n + 1] = '\0';

    M5.Display.setTextSize(1);
    M5.Display.fillRoundRect(8, theme::CONTENT_TOP + 60, theme::SCREEN_W - 16, 16, 3,
                             to565(theme::LIGHT_GRAY));
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::LIGHT_GRAY));
    M5.Display.drawString(masked, 12, theme::CONTENT_TOP + 64);

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.drawString(g_status_line, 8, theme::CONTENT_TOP + 86);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("ENTER connect   BACKSPACE   ESC", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void start_scan() {
    g_scan_in_progress = true;
    paint_wifi_scan();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(/*wifioff=*/false);
    g_scan_count = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    g_scan_in_progress = false;
    g_scan_sel = 0;
    g_scan_scroll = 0;
    paint_wifi_scan();
}

void try_connect() {
    snprintf(g_status_line, sizeof(g_status_line), "Connecting...");
    paint_pass_entry();
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_picked_ssid, g_pass_buf);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        remember_network(g_picked_ssid, g_pass_buf);
        save();
        snprintf(g_status_line, sizeof(g_status_line), "Connected: %s",
                 WiFi.localIP().toString().c_str());
        paint_pass_entry();
        delay(900);
        g_section = Section::MENU;
        paint_menu();
    } else {
        snprintf(g_status_line, sizeof(g_status_line), "Failed.");
        paint_pass_entry();
    }
}

void handle_menu(char k) {
    if (k == ';' || k == 'w') { g_sel = (g_sel - 1 + N_ROWS) % N_ROWS; paint_menu(); }
    else if (k == '.' || k == 's') { g_sel = (g_sel + 1) % N_ROWS; paint_menu(); }
    else if (k == '\r' || k == ' ') {
        g_section = ROWS[g_sel].target;
        if (g_section == Section::WIFI_SCAN)        start_scan();
        else if (g_section == Section::BLUETOOTH)   paint_bluetooth();
        else if (g_section == Section::VOLUME)      paint_volume();
        else if (g_section == Section::BRIGHTNESS)  paint_brightness();
        else if (g_section == Section::ABOUT)       paint_about();
    } else if (k == 0x1b) {
        app::goto_screen(app::Screen::LAUNCHER);
    }
}

void handle_volume(char k) {
    if (k == ',' || k == 'a') { if (store().volume_pct >= 5) store().volume_pct -= 5;
                                audio::set_volume(store().volume_pct); paint_volume(); }
    else if (k == '/' || k == 'd') { if (store().volume_pct <= 95) store().volume_pct += 5;
                                     audio::set_volume(store().volume_pct); paint_volume(); }
    else if (k == '\r') { save(); g_section = Section::MENU; paint_menu(); }
    else if (k == 0x1b)  { load();  // discard
                           g_section = Section::MENU; paint_menu(); }
}

void handle_brightness(char k) {
    if (k == ',' || k == 'a') {
        int p = store().brightness_pct;
        p = (p >= 5) ? p - 5 : 0;
        if (p < (int)MIN_BRIGHTNESS_PCT) p = (int)MIN_BRIGHTNESS_PCT;
        store().brightness_pct = (uint8_t)p;
        apply_display_brightness((uint8_t)p);
        paint_brightness();
    } else if (k == '/' || k == 'd') {
        int p = store().brightness_pct;
        p = (p <= 95) ? p + 5 : 100;
        store().brightness_pct = (uint8_t)p;
        apply_display_brightness((uint8_t)p);
        paint_brightness();
    } else if (k == '\r') {
        save();
        g_section = Section::MENU;
        paint_menu();
    } else if (k == 0x1b) {
        load();   // discard live preview
        apply_display_brightness(store().brightness_pct);
        g_section = Section::MENU;
        paint_menu();
    }
}

void handle_about(char k) {
    if (k == 0x1b || k == '\r') {
        g_section = Section::MENU;
        paint_menu();
    }
}

void handle_bluetooth(char k) {
    if (k == '\r' || k == ' ') {
        store().bluetooth_on = !store().bluetooth_on;
        save();
        paint_bluetooth();
    } else if (k == 0x1b) {
        g_section = Section::MENU; paint_menu();
    }
}

void handle_wifi_scan(char k) {
    if (g_scan_count <= 0) {
        if (k == 'r') start_scan();
        else if (k == 0x1b) { g_section = Section::MENU; paint_menu(); }
        return;
    }
    if (k == ';' || k == 'w') { g_scan_sel = (g_scan_sel - 1 + g_scan_count) % g_scan_count;
                                if (g_scan_sel < g_scan_scroll) g_scan_scroll = g_scan_sel;
                                paint_wifi_scan(); }
    else if (k == '.' || k == 's') { g_scan_sel = (g_scan_sel + 1) % g_scan_count;
                                     if (g_scan_sel - g_scan_scroll >= 6) g_scan_scroll = g_scan_sel - 5;
                                     paint_wifi_scan(); }
    else if (k == 'r') start_scan();
    else if (k == '\r' || k == ' ') {
        strncpy(g_picked_ssid, WiFi.SSID(g_scan_sel).c_str(), sizeof(g_picked_ssid) - 1);
        g_picked_ssid[sizeof(g_picked_ssid) - 1] = '\0';
        // Known network? Skip the password prompt entirely — we already
        // have credentials that worked before. Otherwise fall through to
        // the prompt screen.
        std::string saved = password_for(g_picked_ssid);
        if (!saved.empty()) {
            strncpy(g_pass_buf, saved.c_str(), sizeof(g_pass_buf) - 1);
            g_pass_buf[sizeof(g_pass_buf) - 1] = '\0';
            g_pass_len = (int)saved.size();
            g_status_line[0] = '\0';
            g_section = Section::WIFI_PASS;
            try_connect();
        } else {
            g_pass_len = 0; g_pass_buf[0] = '\0';
            g_status_line[0] = '\0';
            g_section = Section::WIFI_PASS;
            paint_pass_entry();
        }
    } else if (k == 0x1b) {
        g_section = Section::MENU; paint_menu();
    }
}

void handle_pass(char k) {
    if (k == '\r') {
        try_connect();
    } else if (k == 0x1b) {
        g_section = Section::WIFI_SCAN; paint_wifi_scan();
    } else if (k == 0x08 || k == 127) {
        if (g_pass_len > 0) g_pass_buf[--g_pass_len] = '\0';
        paint_pass_entry();
    } else if (k >= 0x20 && k < 0x7f && g_pass_len < 63) {
        g_pass_buf[g_pass_len++] = k;
        g_pass_buf[g_pass_len] = '\0';
        paint_pass_entry();
    }
}

}  // namespace

void enter() {
    g_section = Section::MENU;
    g_sel = 0;
    paint_menu();
}

void tick() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();

    // Translate the bool flags M5Cardputer exposes for special keys into
    // synthetic ASCII codes so the per-section handlers below stay simple.
    // (` is used in place of ESC since Cardputer-Adv has no dedicated ESC.)
    auto dispatch = [&](char k) {
        switch (g_section) {
            case Section::MENU:       handle_menu(k);       break;
            case Section::WIFI_SCAN:  handle_wifi_scan(k);  break;
            case Section::WIFI_PASS:  handle_pass(k);       break;
            case Section::BLUETOOTH:  handle_bluetooth(k);  break;
            case Section::VOLUME:     handle_volume(k);     break;
            case Section::BRIGHTNESS: handle_brightness(k); break;
            case Section::ABOUT:      handle_about(k);      break;
        }
    };
    if (status.enter)              dispatch('\r');
    if (status.del)                dispatch(0x08);
    for (auto k : status.word) {
        if (k == '`') dispatch(0x1b);  // backtick maps to "back"
        else          dispatch(k);
    }
}

}  // namespace settings
