#include <Arduino.h>

#include "app.h"

void setup() {
    Serial.begin(115200);
    app::init();
}

void loop() {
    app::loop();
}
