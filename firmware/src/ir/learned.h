#pragma once

#include <stdint.h>

namespace ir {

// One stored IR command, in IRremoteESP8266 terms (decode_type_t + value +
// bit count). Shared by the IR Prank (learning from a receiver) and the IR
// Remote (assigning buttons from the SD-card database).
struct Learned {
    int16_t  type  = -1;   // decode_type_t, -1 = empty
    uint16_t bits  = 0;
    uint64_t value = 0;
};

// Prank slots, indexed like irprank's Cmd enum.
constexpr int N_SLOTS = 3;
extern const char* const SLOT_NAMES[N_SLOTS];   // "Vol down", "Vol up", "Mute"

// NVS namespace "irlearn": keys s0..s2 hold the slots, "pin" the receiver
// pin index. Missing / malformed entries leave the slot empty.
void load_slots(Learned out[N_SLOTS], uint8_t* rx_pin_idx);
void save_slots(const Learned in[N_SLOTS], uint8_t rx_pin_idx);
void save_slot(int slot, const Learned& l);

}  // namespace ir
