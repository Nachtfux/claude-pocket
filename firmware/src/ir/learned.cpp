#include "learned.h"

#include <Preferences.h>

namespace ir {

const char* const SLOT_NAMES[N_SLOTS] = {"Vol down", "Vol up", "Mute"};

namespace {
constexpr const char* NS = "irlearn";
void slot_key(int i, char key[4]) {
    key[0] = 's';
    key[1] = (char)('0' + i);
    key[2] = 0;
}
}  // namespace

void load_slots(Learned out[N_SLOTS], uint8_t* rx_pin_idx) {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/true)) return;
    for (int i = 0; i < N_SLOTS; ++i) {
        char key[4];
        slot_key(i, key);
        if (p.getBytesLength(key) == sizeof(Learned))
            p.getBytes(key, &out[i], sizeof(Learned));
    }
    if (rx_pin_idx) *rx_pin_idx = p.getUChar("pin", 0);
    p.end();
}

void save_slots(const Learned in[N_SLOTS], uint8_t rx_pin_idx) {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    for (int i = 0; i < N_SLOTS; ++i) {
        char key[4];
        slot_key(i, key);
        p.putBytes(key, &in[i], sizeof(Learned));
    }
    p.putUChar("pin", rx_pin_idx);
    p.end();
}

void save_slot(int slot, const Learned& l) {
    if (slot < 0 || slot >= N_SLOTS) return;
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    char key[4];
    slot_key(slot, key);
    p.putBytes(key, &l, sizeof(Learned));
    p.end();
}

}  // namespace ir
