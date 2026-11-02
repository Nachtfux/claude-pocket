#pragma once

namespace apps {
namespace irremote {

// IR Remote — universal remote backed by the Flipper-IRDB, packed into the
// firmware image (tools/irdb_pack.py → assets/irdb.bin, ~1.9k devices).
// Browse category → brand → device → button, ENTER sends (hold to repeat).
//
// TAB then 1/2/3 stores the selected button as the IR Prank's
// Vol down / Vol up / Mute slot, so any device in the DB can be pranked
// via the prank's "Learned" brand.
void enter();
void tick();

}  // namespace irremote
}  // namespace apps
