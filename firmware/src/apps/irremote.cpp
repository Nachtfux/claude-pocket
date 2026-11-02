#include "irremote.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

#include <algorithm>
#include <ctype.h>
#include <stdarg.h>
#include <vector>

#include "../app.h"
#include "../ir/flipper.h"
#include "../ir/irdb.h"
#include "../ir/learned.h"
#include "../theme.h"

namespace apps {
namespace irremote {

namespace {

constexpr uint16_t IR_PIN = 44;

constexpr int    VISIBLE_ROWS = 6;
constexpr int    ROW_H        = 15;
constexpr int    LIST_Y       = theme::CONTENT_TOP + 15;
constexpr size_t NAME_CHARS   = 27;

// Where we are in the tree. `g_node` is a folder (rows = child nodes) or a
// device (rows = signals). Everything is read from flash on demand, so the
// only RAM in use is this stack and one decoded signal.
struct Level {
    uint32_t node;
    int      sel;
    int      scroll;
};
std::vector<Level> g_stack;      // parents, for going back up
uint32_t g_node = 0;
int      g_sel = 0, g_scroll = 0;

IRsend g_tx(IR_PIN);

bool     g_assign_pending = false;
char     g_msg[48] = {0};
uint32_t g_msg_color = theme::MID_GRAY;
uint32_t g_msg_ms = 0;

// Hold-to-repeat for ENTER on a button.
ir::Signal g_cached;
int64_t    g_cached_sig = -1;
uint32_t   g_enter_down_ms = 0, g_last_repeat_ms = 0;
bool       g_enter_was_down = false;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void set_msg(uint32_t color, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
    va_end(ap);
    g_msg_color = color;
    g_msg_ms = millis();
}

bool in_device() { return ir::db::node_is_device(g_node); }
int  row_count() { return ir::db::node_count_children(g_node); }

// Name of row i of the current level.
const char* row_name(int i) {
    const uint32_t first = ir::db::node_first(g_node);
    return in_device() ? ir::db::signal_name(first + i)
                       : ir::db::node_name(first + i);
}
bool row_is_folder(int i) {
    return !in_device() && !ir::db::node_is_device(ir::db::node_first(g_node) + i);
}

String pretty(const char* s, size_t max_chars) {
    String n = s;
    n.replace('_', ' ');
    if (n.length() > max_chars) n = n.substring(0, max_chars - 2) + "..";
    return n;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void clamp_scroll() {
    const int n = row_count();
    if (g_sel >= n) g_sel = n ? n - 1 : 0;
    if (g_sel < 0) g_sel = 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + VISIBLE_ROWS) g_scroll = g_sel - VISIBLE_ROWS + 1;
    if (g_scroll < 0) g_scroll = 0;
}

void paint_footer() {
    M5.Display.fillRect(0, theme::SCREEN_H - 12, theme::SCREEN_W, 12,
                        to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    if (g_msg[0] && millis() - g_msg_ms < 4000) {
        M5.Display.setTextColor(to565(g_msg_color), to565(theme::IVORY));
        M5.Display.drawString(g_msg, 4, theme::SCREEN_H - 2);
    } else {
        g_msg[0] = 0;
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.drawString(in_device()
                                  ? "ENTER send  TAB prank  a-z jump  ` back"
                                  : ";/. move  ENTER open  a-z jump  ` back",
                              4, theme::SCREEN_H - 2);
    }
    M5.Display.setTextDatum(top_left);
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    if (!ir::db::ok()) {
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.6f);
        M5.Display.drawString("IR Remote", 8, theme::CONTENT_TOP + 4);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
        M5.Display.drawString("IR database missing from firmware.", 8,
                              theme::CONTENT_TOP + 32);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.drawString("Run tools/irdb_pack.py and rebuild.", 8,
                              theme::CONTENT_TOP + 46);
        M5.Display.setTextDatum(bottom_left);
        M5.Display.drawString("`  back", 4, theme::SCREEN_H - 2);
        M5.Display.setTextDatum(top_left);
        return;
    }

    // Breadcrumb: the last two levels ("TVs > Samsung"), trimmed from the
    // left so the deepest part stays visible.
    String crumb;
    if (g_node == 0) {
        crumb = "IR database";
    } else {
        if (g_stack.size() >= 2) {
            crumb = ir::db::node_name(g_stack.back().node);
            crumb += " > ";
        }
        crumb += ir::db::node_name(g_node);
        crumb.replace('_', ' ');
    }
    if (crumb.length() > 36) crumb = ".." + crumb.substring(crumb.length() - 34);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.drawString(crumb, 4, theme::CONTENT_TOP + 3);

    const int n = row_count();
    char count[16];
    snprintf(count, sizeof(count), "%d/%d", n ? g_sel + 1 : 0, n);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.drawString(count, theme::SCREEN_W - 4, theme::CONTENT_TOP + 3);
    M5.Display.setTextDatum(top_left);

    clamp_scroll();
    for (int r = 0; r < VISIBLE_ROWS; ++r) {
        const int i = g_scroll + r;
        if (i >= n) break;
        const int y = LIST_Y + r * ROW_H;
        const bool sel = i == g_sel;
        const bool folder = row_is_folder(i);
        const uint32_t bg = sel ? theme::LIGHT_GRAY : theme::IVORY;
        if (sel) M5.Display.fillRoundRect(2, y - 1, theme::SCREEN_W - 4, ROW_H, 3,
                                          to565(bg));
        M5.Display.setTextSize(1.3f);
        M5.Display.setTextColor(to565(folder ? theme::ORANGE : theme::DARK),
                                to565(bg));
        M5.Display.drawString(pretty(row_name(i), NAME_CHARS), 8, y + 1);
        if (folder) {
            M5.Display.setTextDatum(top_right);
            M5.Display.drawString(">", theme::SCREEN_W - 8, y + 1);
            M5.Display.setTextDatum(top_left);
        }
    }

    // Scrollbar when the list overflows.
    if (n > VISIBLE_ROWS) {
        const int track = VISIBLE_ROWS * ROW_H;
        int h = track * VISIBLE_ROWS / n;
        if (h < 6) h = 6;
        const int y = LIST_Y + (track - h) * g_scroll / (n - VISIBLE_ROWS);
        M5.Display.fillRect(theme::SCREEN_W - 2, LIST_Y, 2, track,
                            to565(theme::LIGHT_GRAY));
        M5.Display.fillRect(theme::SCREEN_W - 2, y, 2, h, to565(theme::MID_GRAY));
    }
    paint_footer();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void open_selected() {
    if (in_device() || row_count() == 0) return;
    g_stack.push_back({g_node, g_sel, g_scroll});
    g_node = ir::db::node_first(g_node) + g_sel;
    g_sel = g_scroll = 0;
    g_cached.release();
    g_cached_sig = -1;
    paint();
}

// Returns false when already at the root.
bool go_up() {
    if (g_stack.empty()) return false;
    const Level l = g_stack.back();
    g_stack.pop_back();
    g_node = l.node;
    g_sel = l.sel;
    g_scroll = l.scroll;
    g_cached.release();
    g_cached_sig = -1;
    paint();
    return true;
}

uint32_t selected_signal() { return ir::db::node_first(g_node) + g_sel; }

void send_selected(bool repeat) {
    if (!in_device() || row_count() == 0) return;
    const uint32_t sig = selected_signal();
    if (g_cached_sig != (int64_t)sig) {
        g_cached_sig = -1;
        if (!ir::db::load_signal(sig, g_cached)) {
            set_msg(theme::ORANGE, "Could not decode signal");
            paint_footer();
            return;
        }
        g_cached_sig = sig;
    }
    if (!ir::send(g_tx, g_cached)) {
        set_msg(theme::ORANGE, "Protocol %s not supported", g_cached.protocol);
        paint_footer();
        return;
    }
    if (!repeat) {
        set_msg(theme::GREEN, "Sent %s", pretty(row_name(g_sel), 30).c_str());
        paint_footer();
    }
}

void assign_slot(int slot) {
    ir::Signal s;
    if (!ir::db::load_signal(selected_signal(), s)) {
        set_msg(theme::ORANGE, "Could not decode signal");
        return;
    }
    ir::Learned l;
    if (!ir::to_learned(s, l)) {
        set_msg(theme::ORANGE, "Raw signals can't go to the prank");
        return;
    }
    ir::save_slot(slot, l);
    set_msg(theme::GREEN, "Prank %s = %s", ir::SLOT_NAMES[slot],
            pretty(row_name(g_sel), 18).c_str());
}

void jump_to_letter(char c) {
    const int n = row_count();
    c = (char)tolower(c);
    for (int k = 1; k <= n; ++k) {
        const int i = (g_sel + k) % n;
        if (tolower((unsigned char)row_name(i)[0]) == c) {
            g_sel = i;
            return;
        }
    }
}

void leave() {
    std::vector<Level>().swap(g_stack);
    g_cached.release();
    g_cached_sig = -1;
    app::goto_screen(app::Screen::LAUNCHER);
}

}  // namespace

void enter() {
    g_tx.begin();
    g_msg[0] = 0;
    g_assign_pending = false;
    g_stack.clear();
    g_node = 0;
    g_sel = g_scroll = 0;
    g_cached.release();
    g_cached_sig = -1;
    g_enter_was_down = true;   // swallow the ENTER that opened the app
    paint();
}

void tick() {
    auto& ks = M5Cardputer.Keyboard.keysState();
    const uint32_t now = millis();

    // ENTER: press = open/send, hold on a button = auto-repeat.
    if (ks.enter) {
        if (!g_enter_was_down) {
            g_enter_was_down = true;
            g_enter_down_ms = g_last_repeat_ms = now;
            if (in_device()) send_selected(false);
            else open_selected();
        } else if (in_device() && now - g_enter_down_ms > 500 &&
                   now - g_last_repeat_ms > 200) {
            g_last_repeat_ms = now;
            send_selected(true);
        }
        return;
    }
    g_enter_was_down = false;

    // Let the status message time out.
    if (g_msg[0] && now - g_msg_ms > 4000) paint_footer();

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;

    const char c = ks.word.empty() ? 0 : ks.word.front();
    const bool back = c == '`' || ks.del;   // del = Backspace in M5Cardputer 1.1.x

    if (!ir::db::ok()) {
        if (back) leave();
        return;
    }

    if (g_assign_pending) {
        g_assign_pending = false;
        if (c >= '1' && c <= '3') assign_slot(c - '1');
        else set_msg(theme::MID_GRAY, "Cancelled");
        paint_footer();
        return;
    }

    if (back) {
        if (!go_up()) leave();
        return;
    }

    if (ks.tab && in_device() && row_count() > 0) {
        g_assign_pending = true;
        set_msg(theme::YELLOW, "Prank slot: 1 Vol dn  2 Vol up  3 Mute");
        paint_footer();
        return;
    }

    const int n = row_count();
    if (!n) return;
    // ; . , / carry the arrow legends on the Cardputer keyboard (with or
    // without Fn held — the 1.1.x library has no Fn layer).
    if (c == ';')      g_sel = (g_sel - 1 + n) % n;
    else if (c == '.') g_sel = (g_sel + 1) % n;
    else if (c == ',') g_sel = std::max(0, g_sel - VISIBLE_ROWS);
    else if (c == '/') g_sel = std::min(n - 1, g_sel + VISIBLE_ROWS);
    else if (isalnum((unsigned char)c)) jump_to_letter(c);
    else return;
    paint();
}

}  // namespace irremote
}  // namespace apps
