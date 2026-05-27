#pragma once

// Runtime API-key + WiFi credential storage. Lets us ship a "Burner" build
// of the firmware whose binary contains no secrets — the user drops a
// /keys.txt onto the microSD card and the device imports it into NVS on
// first boot. Subsequent boots read from NVS directly; the SD card can be
// removed.
//
// Fallback: when NVS is empty, the accessor functions return the macros
// from config.h. That preserves the dev workflow (compile-time keys) while
// enabling a public Burner image.

namespace app {
namespace keys {

// Mount the SD card, look for /keys.txt, parse KEY=VALUE lines into NVS.
// Returns the number of keys imported (0 if no card, no file, or no
// recognized keys). Non-blocking, safe to call on every boot.
int import_from_sd();

// Force a re-read of the NVS-backed values into the cache. Cheap.
void reload();

// Accessors — return NVS value if set, otherwise the compile-time fallback
// from config.h. Returned pointer is valid until the next reload().
const char* anthropic_key();
const char* openai_key();
const char* wifi_ssid();
const char* wifi_pass();

}  // namespace keys
}  // namespace app
