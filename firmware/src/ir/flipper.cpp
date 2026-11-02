#include "flipper.h"

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>

#include <ctype.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace ir {

void Signal::release() {
    delete[] timings;
    timings = nullptr;
    n_timings = 0;
}

namespace {

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// Read one line into `buf` (CR/LF stripped). Returns false at EOF with
// nothing read. Overlong lines are truncated — only "data:" lines get long
// and those are parsed by stream_numbers() instead.
bool read_line(File& f, char* buf, size_t n) {
    size_t len = 0;
    int c = -1;
    while ((c = f.read()) >= 0) {
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 < n) buf[len++] = (char)c;
    }
    buf[len] = 0;
    return c >= 0 || len > 0;
}

// "07 00 00 00" → 0x00000007 (bytes are little-endian in the file).
uint32_t parse_le_bytes(const char* s) {
    uint32_t v = 0;
    int shift = 0;
    while (*s && shift < 32) {
        while (*s == ' ') ++s;
        if (!isxdigit((unsigned char)*s)) break;
        char* end = nullptr;
        unsigned long b = strtoul(s, &end, 16);
        v |= (uint32_t)(b & 0xff) << shift;
        shift += 8;
        s = end;
    }
    return v;
}

// Parse the numbers of a "data:" line straight from the file, so a 3 KB
// line never has to sit in RAM as text. Consumes up to and including '\n'.
bool stream_numbers(File& f, Signal& out) {
    out.release();
    out.timings = new (std::nothrow) uint16_t[MAX_RAW];
    if (!out.timings) return false;
    uint32_t cur = 0;
    bool in_num = false, overflow = false;
    int c;
    while ((c = f.read()) >= 0 && c != '\n') {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (uint32_t)(c - '0');
            in_num = true;
        } else if (in_num) {
            if (out.n_timings < MAX_RAW)
                out.timings[out.n_timings++] = cur > 0xffff ? 0xffff : (uint16_t)cur;
            else
                overflow = true;
            cur = 0;
            in_num = false;
        }
    }
    if (in_num) {
        if (out.n_timings < MAX_RAW)
            out.timings[out.n_timings++] = cur > 0xffff ? 0xffff : (uint16_t)cur;
        else
            overflow = true;
    }
    if (overflow) out.release();
    return !overflow && out.n_timings > 0;
}

const char* value_of(const char* line, const char* key) {
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0) return nullptr;
    const char* v = line + k;
    while (*v == ' ') ++v;
    return v;
}

// ---------------------------------------------------------------------------
// Encoding — byte layouts mirror the Flipper Zero firmware's encoders
// (lib/infrared/encoder_decoder/*), converted to IRremoteESP8266's
// MSB-first convention by bit-reversing each transmitted byte.
// ---------------------------------------------------------------------------

uint8_t rev8(uint8_t b) { return (uint8_t)reverseBits(b, 8); }

// Bytes as sent on the wire (LSB first) → IRremoteESP8266 value.
uint64_t wire_bytes_to_value(const uint8_t* d, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | rev8(d[i]);
    return v;
}

bool is(const Signal& s, const char* p) { return strcmp(s.protocol, p) == 0; }

bool encode(const Signal& s, Learned& out) {
    const uint32_t a = s.address, c = s.command;

    if (is(s, "NEC") || is(s, "NECext")) {
        uint8_t d[4];
        if (is(s, "NEC")) {
            d[0] = (uint8_t)a;  d[1] = (uint8_t)~a;
            d[2] = (uint8_t)c;  d[3] = (uint8_t)~c;
        } else {
            d[0] = (uint8_t)a;  d[1] = (uint8_t)(a >> 8);
            d[2] = (uint8_t)c;  d[3] = (uint8_t)(c >> 8);
        }
        out = {(int16_t)decode_type_t::NEC, 32, wire_bytes_to_value(d, 4)};
        return true;
    }
    if (is(s, "Samsung32")) {
        const uint8_t d[4] = {(uint8_t)a, (uint8_t)a, (uint8_t)c, (uint8_t)~c};
        out = {(int16_t)decode_type_t::SAMSUNG, 32, wire_bytes_to_value(d, 4)};
        return true;
    }
    if (is(s, "Kaseikyo")) {
        const uint8_t  id     = (a >> 24) & 3;
        const uint16_t vendor = (a >> 8) & 0xffff;
        const uint8_t  genre1 = (a >> 4) & 0xf;
        const uint8_t  genre2 = a & 0xf;
        uint8_t d[6];
        d[0] = vendor & 0xff;
        d[1] = vendor >> 8;
        uint8_t vp = d[0] ^ d[1];
        vp = (vp & 0xf) ^ (vp >> 4);
        d[2] = (vp & 0xf) | (genre1 << 4);
        d[3] = (genre2 & 0xf) | ((uint8_t)(c & 0xf) << 4);
        d[4] = (id << 6) | (uint8_t)(c >> 4);
        d[5] = d[2] ^ d[3] ^ d[4];
        out = {(int16_t)decode_type_t::PANASONIC, 48, wire_bytes_to_value(d, 6)};
        return true;
    }
    if (is(s, "SIRC") || is(s, "SIRC15") || is(s, "SIRC20")) {
        uint16_t nbits;
        uint32_t data = c & 0x7f;
        if (is(s, "SIRC"))        { data |= (a & 0x1f) << 7;   nbits = 12; }
        else if (is(s, "SIRC15")) { data |= (a & 0xff) << 7;   nbits = 15; }
        else                      { data |= (a & 0x1fff) << 7; nbits = 20; }
        out = {(int16_t)decode_type_t::SONY, nbits, reverseBits(data, nbits)};
        return true;
    }
    if (is(s, "RC5") || is(s, "RC5X")) {
        // RC5X = second start bit cleared = command bit 6 set.
        uint8_t cmd = (uint8_t)(c & 0x3f);
        if (is(s, "RC5X")) cmd |= 0x40;
        // Same layout as IRsend::encodeRC5X: S2 | toggle | 5-bit addr | 6-bit cmd.
        const uint64_t v = ((uint64_t)((cmd >> 6) & 1) << (kRC5XBits - 1)) |
                           ((a & 0x1f) << 6) | (cmd & 0x3f);
        out = {(int16_t)decode_type_t::RC5X, kRC5XBits, v};
        return true;
    }
    if (is(s, "RC6")) {
        // Same layout as IRsend::encodeRC6 for mode 0: addr << 8 | cmd.
        out = {(int16_t)decode_type_t::RC6, kRC6Mode0Bits,
               ((uint64_t)(a & 0xff) << 8) | (c & 0xff)};
        return true;
    }
    return false;   // RCA, Pioneer, NEC42, NEC42ext
}

bool g_rc_toggle = false;   // RC5/RC6 toggle bit, flipped per press

}  // namespace

bool read_signal(File& f, Signal& out) {
    out.release();
    out.raw = false;
    out.protocol[0] = 0;
    out.address = out.command = 0;
    out.freq = 38000;
    bool have_type = false;

    char line[96];
    while (true) {
        const size_t pos = f.position();
        // Peek the key without buffering a potentially huge data line.
        char head[6] = {0};
        size_t got = f.read((uint8_t*)head, 5);
        if (got == 0) break;
        if (got == 5 && strncmp(head, "data:", 5) == 0) {
            if (!stream_numbers(f, out)) return false;
            continue;
        }
        f.seek(pos);
        if (!read_line(f, line, sizeof(line))) break;
        if (line[0] == '#') break;
        if (strncmp(line, "name:", 5) == 0) { f.seek(pos); break; }

        const char* v;
        if ((v = value_of(line, "type:"))) {
            out.raw = strncmp(v, "raw", 3) == 0;
            have_type = true;
        } else if ((v = value_of(line, "protocol:"))) {
            strncpy(out.protocol, v, sizeof(out.protocol) - 1);
            out.protocol[sizeof(out.protocol) - 1] = 0;
        } else if ((v = value_of(line, "address:"))) {
            out.address = parse_le_bytes(v);
        } else if ((v = value_of(line, "command:"))) {
            out.command = parse_le_bytes(v);
        } else if ((v = value_of(line, "frequency:"))) {
            out.freq = (uint32_t)strtoul(v, nullptr, 10);
        }
    }
    if (!have_type) return false;
    return out.raw ? out.n_timings > 0 : out.protocol[0] != 0;
}

bool supported(const Signal& s) {
    if (s.raw) return s.n_timings > 0;
    Learned dummy;
    return encode(s, dummy);
}

bool to_learned(const Signal& s, Learned& out) {
    if (s.raw) return false;
    return encode(s, out);
}

bool send(IRsend& tx, const Signal& s) {
    if (s.raw) {
        if (!s.n_timings) return false;
        tx.sendRaw(s.timings, s.n_timings, (uint16_t)(s.freq ? s.freq : 38000));
        return true;
    }
    Learned l;
    if (!encode(s, l)) return false;
    const decode_type_t t = (decode_type_t)l.type;
    uint64_t v = l.value;
    if (t == decode_type_t::RC5X) {
        if (g_rc_toggle) v = tx.toggleRC5(v);
        g_rc_toggle = !g_rc_toggle;
    } else if (t == decode_type_t::RC6) {
        if (g_rc_toggle) v = tx.toggleRC6(v, kRC6Mode0Bits);
        g_rc_toggle = !g_rc_toggle;
    }
    // Sony devices only act after three identical frames.
    const uint16_t repeat = (t == decode_type_t::SONY) ? 2 : kNoRepeat;
    return tx.send(t, v, l.bits, repeat);
}

}  // namespace ir
