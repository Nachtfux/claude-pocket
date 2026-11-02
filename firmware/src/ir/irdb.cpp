#include "irdb.h"

#include <new>
#include <string.h>

// Provided by `board_build.embed_files = assets/irdb.bin` in platformio.ini.
extern const uint8_t irdb_start[] asm("_binary_assets_irdb_bin_start");
extern const uint8_t irdb_end[]   asm("_binary_assets_irdb_bin_end");

namespace ir {
namespace db {

namespace {

constexpr size_t   HEADER_SIZE = 40;
constexpr size_t   NODE_SIZE   = 12;
constexpr size_t   SIG_SIZE    = 11;
constexpr uint16_t NODE_DEVICE = 1;

// Order must match PROTOS in tools/irdb_pack.py (0 = raw).
const char* const PROTO_NAMES[] = {
    "", "NEC", "NECext", "Samsung32", "Kaseikyo", "SIRC",
    "SIRC15", "SIRC20", "RC5", "RC5X", "RC6",
};
constexpr uint8_t N_PROTOS = sizeof(PROTO_NAMES) / sizeof(PROTO_NAMES[0]);

struct Header {
    uint32_t version, n_nodes, n_signals, n_names;
    uint32_t off_nodes, off_signals, off_names, off_strings, off_raw;
};

// Flash-mapped data may be unaligned for multi-byte reads — go via memcpy.
uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

size_t blob_size() { return (size_t)(irdb_end - irdb_start); }

const Header& header() {
    static Header h;
    static bool loaded = false;
    if (!loaded) {
        memset(&h, 0, sizeof(h));
        if (blob_size() >= HEADER_SIZE && memcmp(irdb_start, "IRDB", 4) == 0)
            memcpy(&h, irdb_start + 4, sizeof(h));
        loaded = true;
    }
    return h;
}

const uint8_t* node_ptr(uint32_t n) {
    return irdb_start + header().off_nodes + (size_t)n * NODE_SIZE;
}
const uint8_t* sig_ptr(uint32_t s) {
    return irdb_start + header().off_signals + (size_t)s * SIG_SIZE;
}
const char* str_at(uint32_t off) {
    return (const char*)(irdb_start + header().off_strings + off);
}

}  // namespace

bool ok() {
    const Header& h = header();
    return h.version == 1 && h.n_nodes > 0 && h.off_raw <= blob_size();
}

uint32_t node_count() { return ok() ? header().n_nodes : 0; }

const char* node_name(uint32_t n) {
    if (n >= node_count()) return "";
    return str_at(rd32(node_ptr(n)));
}

bool node_is_device(uint32_t n) {
    if (n >= node_count()) return false;
    return rd16(node_ptr(n) + 10) & NODE_DEVICE;
}

uint32_t node_first(uint32_t n) {
    if (n >= node_count()) return 0;
    return rd32(node_ptr(n) + 4);
}

uint16_t node_count_children(uint32_t n) {
    if (n >= node_count()) return 0;
    return rd16(node_ptr(n) + 8);
}

const char* signal_name(uint32_t s) {
    const Header& h = header();
    if (!ok() || s >= h.n_signals) return "";
    const uint16_t idx = rd16(sig_ptr(s));
    if (idx >= h.n_names) return "";
    return str_at(rd32(irdb_start + h.off_names + (size_t)idx * 4));
}

bool load_signal(uint32_t s, Signal& out) {
    const Header& h = header();
    out.release();
    if (!ok() || s >= h.n_signals) return false;
    const uint8_t* p = sig_ptr(s);
    const uint8_t proto = p[2];
    const uint32_t a = rd32(p + 3), c = rd32(p + 7);

    if (proto != 0) {
        if (proto >= N_PROTOS) return false;
        out.raw = false;
        strncpy(out.protocol, PROTO_NAMES[proto], sizeof(out.protocol) - 1);
        out.protocol[sizeof(out.protocol) - 1] = 0;
        out.address = a;
        out.command = c;
        return true;
    }

    // Raw record: u16 n, u8 k, u8 bits, k x u16 levels, packed indices.
    out.raw = true;
    out.protocol[0] = 0;
    out.freq = c ? c : 38000;
    const uint8_t* r = irdb_start + h.off_raw + a;
    if (r + 4 > irdb_end) return false;
    const uint16_t n = rd16(r);
    const uint8_t  k = r[2], bits = r[3];
    if (n == 0 || n > MAX_RAW) return false;

    out.timings = new (std::nothrow) uint16_t[n];
    if (!out.timings) return false;

    if (k == 0) {   // stored verbatim
        const uint8_t* v = r + 4;
        if (v + 2 * n > irdb_end) { out.release(); return false; }
        for (uint16_t i = 0; i < n; ++i) out.timings[i] = rd16(v + 2 * i);
    } else {
        const uint8_t* levels = r + 4;
        const uint8_t* stream = levels + 2 * k;
        if (stream + ((uint32_t)n * bits + 7) / 8 > irdb_end) {
            out.release();
            return false;
        }
        uint32_t acc = 0;
        int nacc = 0;
        const uint32_t mask = (1u << bits) - 1;
        for (uint16_t i = 0; i < n; ++i) {
            while (nacc < bits) {
                acc |= (uint32_t)(*stream++) << nacc;
                nacc += 8;
            }
            uint32_t li = acc & mask;
            acc >>= bits;
            nacc -= bits;
            if (li >= k) li = k - 1;
            out.timings[i] = rd16(levels + 2 * li);
        }
    }
    out.n_timings = n;
    return true;
}

}  // namespace db
}  // namespace ir
