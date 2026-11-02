#pragma once

#include <stdint.h>

#include "flipper.h"

namespace ir {
namespace db {

// Read-only access to the Flipper-IRDB blob embedded in the firmware
// (firmware/assets/irdb.bin, built by tools/irdb_pack.py — see the layout
// documented there). Everything is read straight from memory-mapped flash;
// nothing is copied into RAM except a raw signal that is about to be sent.

bool ok();                         // blob present and header valid

// Node 0 is the root folder. A node is either a folder (children are
// nodes) or a device (children are signals).
uint32_t    node_count();
const char* node_name(uint32_t node);
bool        node_is_device(uint32_t node);
uint32_t    node_first(uint32_t node);   // first child node / signal index
uint16_t    node_count_children(uint32_t node);

const char* signal_name(uint32_t sig);

// Decode a signal into the same structure the .ir parser produces, so the
// existing ir::send / ir::to_learned work unchanged.
bool load_signal(uint32_t sig, Signal& out);

}  // namespace db
}  // namespace ir
