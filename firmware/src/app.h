#pragma once

namespace app {

enum class Screen {
    LAUNCHER,
    POCKET,
    BUDDY,
    SNAKE,
    WEATHER,
    RADIO,
    TRANSLATE,
    SETTINGS,
};

void init();
void loop();

// Screen routing — any screen can request a transition.
void goto_screen(Screen s);
Screen current();

}  // namespace app
