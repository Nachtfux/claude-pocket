#pragma once

#include <FS.h>
#include <stdint.h>

#include "learned.h"

class IRsend;

namespace ir {

// One button out of a Flipper Zero ".ir" file (the format used by the
// Flipper-IRDB, github.com/Lucaslhm/Flipper-IRDB — CC0).
//
//   name: Vol_dn                name: Power
//   type: parsed                type: raw
//   protocol: Samsung32         frequency: 38000
//   address: 07 00 00 00        duty_cycle: 0.330000
//   command: 0B 00 00 00        data: 9024 4512 579 552 ...
struct Signal {
    bool     raw      = false;
    char     protocol[12] = {0};
    uint32_t address  = 0;
    uint32_t command  = 0;
    uint32_t freq     = 38000;
    uint16_t* timings = nullptr;   // raw only, owned (free with release())
    uint16_t n_timings = 0;

    void release();
    ~Signal() { release(); }
};

// Max raw timings we keep (2 KB). Longer captures — almost only A/C
// remotes — are rejected rather than truncated.
constexpr uint16_t MAX_RAW = 1024;

// Read the signal body that follows a "name:" line. `f` must be positioned
// right after that line; reading stops at the next "#" separator, the next
// "name:" line or EOF.
bool read_signal(File& f, Signal& out);

// Transmit. Returns false for protocols we can't encode (RCA, Pioneer,
// NEC42) so the UI can say so.
bool send(IRsend& tx, const Signal& s);

// Convert a parsed signal to the compact form the IR Prank stores. Raw and
// unsupported signals return false.
bool to_learned(const Signal& s, Learned& out);

// True if `send` knows the protocol (parsed) or the signal is raw.
bool supported(const Signal& s);

}  // namespace ir
