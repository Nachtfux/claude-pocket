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
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W,
                       theme::CONTENT_H, to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    // Title
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.drawString("Claude Buddy", 8, theme::CONTENT_TOP + 4);

    // Description — the BLE companion to Claude.app's Hardware Buddy.
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    int y = theme::CONTENT_TOP + 28;
    M5.Display.drawString("BLE pairing with Claude.app's", 8, y);
    M5.Display.drawString("Hardware Buddy.  Approve and", 8, y + 11);
    M5.Display.drawString("deny actions wirelessly.", 8, y + 22);

    // Pointer to the original.
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.drawString("Source / port reference:", 8, y + 42);
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.drawString("github.com/moremas/", 8, y + 53);
    M5.Display.drawString("build-with-claude (buddy/)", 8, y + 64);

    // Footer hint
    M5.Display.setTextDatum(bottom_left);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.drawString("`  back", 4, theme::SCREEN_H - 2);
}

}  // namespace

void enter() { paint(); }

void tick() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();
    for (auto k : status.word) {
        if (k == '`') {
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
    }
}

}  // namespace buddy
}  // namespace apps
