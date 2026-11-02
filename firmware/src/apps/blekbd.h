#pragma once

namespace apps {
namespace blekbd {

// BLE Keyboard — turns the Cardputer into a Bluetooth HID keyboard for a
// computer, tablet or phone. The NimBLE stack only runs while this screen is
// open; leaving it tears the stack down completely so the voice flows get
// their TLS heap back (no PSRAM — see docs/SPEC.md §12.2).
//
// Every key is forwarded to the host, so the app is left with the G0 button
// on the side of the device (or Fn + Opt as a keyboard fallback).
void enter();
void tick();

}  // namespace blekbd
}  // namespace apps
