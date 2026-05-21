#pragma once

#include <M5GFX.h>

namespace ui {

// Persistent 14-px strip across the top: WiFi · BT · volume · battery · clock.
// Stateless — pulls everything from runtime globals each draw call.
class StatusBar {
public:
    static void draw(M5GFX& display);
};

}  // namespace ui
