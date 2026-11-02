#pragma once

namespace apps {
namespace irprank {

// IR Prank — for your own TV. Sends volume / mute codes through the
// Cardputer's IR LED (GPIO44) at random intervals: the volume creeps down
// (or up) one step at a time, or the TV "ghost-mutes" for a few seconds.
//
// Setup screen: brand, mode, interval and step limit; T fires a test
// command so you can check the TV reacts. ENTER arms it and blanks the
// display (stealth); any key wakes it, ENTER / ` stops.
void enter();
void tick();

}  // namespace irprank
}  // namespace apps
