#pragma once

namespace apps {
namespace radio {

// Internet radio screen. Connects to one of a small hardcoded station
// list (HTTP MP3 streams), decodes via libhelix-mp3 wrapped by the
// `schreibfaul1/ESP32-audioI2S` library, and pushes PCM straight at the
// ES8311 codec / NS4150 amp. Keys: W/S = prev/next station, OK =
// play/pause, `` ` `` = back to launcher.
void enter();
void tick();
void leave();   // stop decoder/I2S before another screen claims the codec

}  // namespace radio
}  // namespace apps
