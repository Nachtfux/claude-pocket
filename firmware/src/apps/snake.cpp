#include "snake.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <stdlib.h>

#include "../app.h"
#include "../theme.h"

namespace apps {
namespace snake {

namespace {

// 240x110 play area, 10 px cells → 24x11 grid.
constexpr int CELL = 10;
constexpr int COLS = 24;
constexpr int ROWS = 11;
constexpr int PLAY_Y0 = theme::CONTENT_TOP + 8;

struct Pt { int8_t x, y; };

Pt body[COLS * ROWS];
int  len = 0;
Pt   food = {0, 0};
int8_t dx = 1, dy = 0;
int8_t pending_dx = 1, pending_dy = 0;
uint32_t last_step_ms = 0;
uint32_t step_interval_ms = 200;
bool game_over = false;
enum class Mode { MENU, PLAYING };
Mode mode = Mode::MENU;
int menu_idx = 1;        // 0=easy, 1=normal, 2=heavy
const uint32_t SPEEDS[3] = {220, 140, 80};
const char* SPEED_LABELS[3] = {"Easy", "Normal", "Heavy"};

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void spawn_food() {
    while (true) {
        food.x = random(COLS);
        food.y = random(ROWS);
        bool clash = false;
        for (int i = 0; i < len; ++i) {
            if (body[i].x == food.x && body[i].y == food.y) { clash = true; break; }
        }
        if (!clash) return;
    }
}

void paint_full() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.drawRect(0, PLAY_Y0 - 1, COLS * CELL + 2, ROWS * CELL + 2,
                        to565(theme::LIGHT_GRAY));
    for (int i = 0; i < len; ++i) {
        M5.Display.fillRect(body[i].x * CELL + 1, PLAY_Y0 + body[i].y * CELL + 1,
                            CELL - 2, CELL - 2, to565(theme::ORANGE));
    }
    M5.Display.fillRect(food.x * CELL + 2, PLAY_Y0 + food.y * CELL + 2,
                        CELL - 4, CELL - 4, to565(theme::GREEN));
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("WASD to move, ESC back", 4, theme::SCREEN_H - 2);
    if (game_over) {
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(2);
        M5.Display.setTextDatum(middle_center);
        M5.Display.drawString("Game Over", theme::SCREEN_W / 2, theme::SCREEN_H / 2);
    }
}

void reset() {
    len = 3;
    body[0] = {COLS / 2, ROWS / 2};
    body[1] = {COLS / 2 - 1, ROWS / 2};
    body[2] = {COLS / 2 - 2, ROWS / 2};
    dx = 1; dy = 0; pending_dx = 1; pending_dy = 0;
    game_over = false;
    spawn_food();
    paint_full();
}

void paint_menu() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                       to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Snake", theme::SCREEN_W / 2, theme::CONTENT_TOP + 8);

    M5.Display.setTextSize(1.5f);
    int y = theme::CONTENT_TOP + 40;
    for (int i = 0; i < 3; ++i) {
        bool selected = (i == menu_idx);
        M5.Display.setTextColor(
            selected ? to565(theme::ORANGE) : to565(theme::MID_GRAY),
            to565(theme::IVORY));
        char line[32];
        snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", SPEED_LABELS[i]);
        M5.Display.drawString(line, theme::SCREEN_W / 2, y);
        y += 22;
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextDatum(bottom_center);
    M5.Display.drawString("W/S or 1-3 select, OK to start",
                          theme::SCREEN_W / 2, theme::SCREEN_H - 2);
}

void start_game(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    menu_idx = idx;
    step_interval_ms = SPEEDS[idx];
    mode = Mode::PLAYING;
    reset();
    last_step_ms = millis();
}

}  // namespace

void enter() {
    randomSeed(esp_random());
    mode = Mode::MENU;
    paint_menu();
}

void tick() {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto status = M5Cardputer.Keyboard.keysState();
        if (mode == Mode::MENU) {
            if (status.enter) { start_game(menu_idx); return; }
            for (auto k : status.word) {
                if (k == 'w' || k == ';') {
                    menu_idx = (menu_idx + 2) % 3;
                    paint_menu();
                } else if (k == 's' || k == '.') {
                    menu_idx = (menu_idx + 1) % 3;
                    paint_menu();
                } else if (k == '1') { start_game(0); return; }
                else if (k == '2') { start_game(1); return; }
                else if (k == '3') { start_game(2); return; }
                else if (k == 'q' || k == '`') {
                    app::goto_screen(app::Screen::LAUNCHER); return;
                }
            }
            return;
        }
        if (status.enter && game_over) { reset(); }
        for (auto k : status.word) {
            if ((k == 'w' || k == ';') && dy == 0) { pending_dx = 0; pending_dy = -1; }
            else if ((k == 's' || k == '.') && dy == 0) { pending_dx = 0; pending_dy = 1; }
            else if ((k == 'a' || k == ',') && dx == 0) { pending_dx = -1; pending_dy = 0; }
            else if ((k == 'd' || k == '/') && dx == 0) { pending_dx = 1; pending_dy = 0; }
            else if (k == 'q' || k == '`') {
                mode = Mode::MENU;
                paint_menu();
                return;
            }
        }
    }

    if (mode == Mode::MENU || game_over) return;

    uint32_t now = millis();
    if (now - last_step_ms < step_interval_ms) return;
    last_step_ms = now;

    dx = pending_dx; dy = pending_dy;
    int nx = body[0].x + dx;
    int ny = body[0].y + dy;

    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) { game_over = true; paint_full(); return; }
    for (int i = 0; i < len; ++i) {
        if (body[i].x == nx && body[i].y == ny) { game_over = true; paint_full(); return; }
    }

    bool ate = (nx == food.x && ny == food.y);
    if (!ate) {
        for (int i = len - 1; i > 0; --i) body[i] = body[i - 1];
    } else {
        for (int i = len; i > 0; --i) body[i] = body[i - 1];
        ++len;
        spawn_food();
    }
    body[0] = {(int8_t)nx, (int8_t)ny};
    paint_full();
}

}  // namespace snake
}  // namespace apps
