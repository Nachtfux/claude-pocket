#include "status_bar.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <stdio.h>
#include <time.h>

#include "../settings/store.h"
#include "../theme.h"

namespace ui {

namespace {
uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void draw_wifi_glyph(M5GFX& d, int x, int y, bool connected) {
    const uint16_t fg = connected ? to565(theme::GREEN) : to565(theme::MID_GRAY);
    // Three stacked arcs, sized to ~10 px.
    d.drawArc(x + 5, y + 9, 8, 8, 225, 315, fg);
    d.drawArc(x + 5, y + 9, 5, 5, 225, 315, fg);
    d.fillCircle(x + 5, y + 9, 1, fg);
}

void draw_bt_glyph(M5GFX& d, int x, int y, bool on) {
    const uint16_t fg = on ? to565(theme::BLUE) : to565(theme::MID_GRAY);
    d.drawLine(x + 3, y + 2, x + 3, y + 12, fg);
    d.drawLine(x + 3, y + 2, x + 7, y + 6, fg);
    d.drawLine(x + 3, y + 12, x + 7, y + 8, fg);
    d.drawLine(x, y + 5, x + 7, y + 9, fg);
    d.drawLine(x + 7, y + 6, x, y + 9, fg);
}

void draw_volume_glyph(M5GFX& d, int x, int y, uint8_t pct) {
    const uint16_t fg = to565(theme::DARK);
    const uint16_t dim = to565(theme::LIGHT_GRAY);
    d.fillTriangle(x, y + 7, x + 4, y + 4, x + 4, y + 10, fg);
    d.fillRect(x + 4, y + 5, 2, 5, fg);
    if (pct > 33) d.drawLine(x + 8, y + 5, x + 9, y + 9, fg);
    else          d.drawLine(x + 8, y + 5, x + 9, y + 9, dim);
    if (pct > 66) d.drawLine(x + 10, y + 4, x + 11, y + 10, fg);
    else          d.drawLine(x + 10, y + 4, x + 11, y + 10, dim);
}

void draw_battery_glyph(M5GFX& d, int x, int y, int pct, bool charging) {
    const uint16_t fg = to565(theme::DARK);
    const uint16_t fill = charging ? to565(theme::GREEN)
                                   : (pct < 20 ? to565(theme::ORANGE) : to565(theme::DARK));
    d.drawRect(x, y + 3, 16, 8, fg);
    d.fillRect(x + 16, y + 5, 2, 4, fg);
    int filled = pct * 14 / 100;
    if (filled > 0) d.fillRect(x + 1, y + 4, filled, 6, fill);
}

}  // namespace

void StatusBar::draw(M5GFX& d) {
    d.fillRect(0, 0, theme::SCREEN_W, theme::STATUS_BAR_H, to565(theme::IVORY));
    d.drawFastHLine(0, theme::STATUS_BAR_H - 1, theme::SCREEN_W, to565(theme::LIGHT_GRAY));

    draw_wifi_glyph(d, 2, 0, WiFi.status() == WL_CONNECTED);
    draw_bt_glyph(d, 18, 0, settings::store().bluetooth_on);
    draw_volume_glyph(d, 32, 0, settings::store().volume_pct);

    int batt_pct = M5.Power.getBatteryLevel();
    bool charging = M5.Power.isCharging();
    draw_battery_glyph(d, theme::SCREEN_W - 22, 0, batt_pct, charging);

    char clock_buf[8] = "--:--";
    time_t now = time(nullptr);
    if (now > 1700000000) {  // year >= 2023, i.e. SNTP has fired
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    d.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    d.setTextSize(theme::FONT_BODY_SIZE);
    d.setTextDatum(top_center);
    d.drawString(clock_buf, theme::SCREEN_W / 2, 2);
}

}  // namespace ui
