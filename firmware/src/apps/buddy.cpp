#include "buddy.h"

#include <M5Cardputer.h>
#include <M5Unified.h>

#include "../app.h"
#include "../theme.h"

namespace apps {
namespace buddy {

namespace {

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Claude Buddy", theme::SCREEN_W / 2, theme::CONTENT_TOP + 10);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.drawString("BLE companion (port planned)", theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 40);
    M5.Display.drawString("Lives in the MicroPython buddy", theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 56);
    M5.Display.drawString("ESC = back", theme::SCREEN_W / 2,
                          theme::CONTENT_TOP + 84);
}

}  // namespace

void enter() { paint(); }

void tick() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();
    for (auto k : status.word) {
        if (k == 'q' || k == '`') app::goto_screen(app::Screen::LAUNCHER);
    }
}

}  // namespace buddy
}  // namespace apps
