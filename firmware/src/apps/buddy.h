#pragma once

namespace apps {
namespace buddy {

// Claude Buddy — placeholder for the BLE companion shipped with the
// Anthropic "Code with Claude" bundle (github.com/moremas/build-with-claude).
// The MicroPython original pairs with the Hardware Buddy in Claude.app
// over BLE and shows approve/deny activity. The port to this C++ build
// is open work; for now this screen documents the feature so attendees
// know where to look.
void enter();
void tick();

}  // namespace buddy
}  // namespace apps
