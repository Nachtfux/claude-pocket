#include "wav.h"

#include <string.h>

namespace audio {

namespace {
void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
void put_u16_le(uint8_t* p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
}  // namespace

void make_wav_header(WavHeader& out, uint32_t sample_count, uint32_t sample_rate) {
    uint8_t* b = out.bytes;
    const uint32_t data_bytes = sample_count * 2;
    memcpy(b, "RIFF", 4);
    put_u32_le(b + 4, 36 + data_bytes);
    memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    put_u32_le(b + 16, 16);          // fmt chunk size
    put_u16_le(b + 20, 1);           // PCM
    put_u16_le(b + 22, 1);           // channels
    put_u32_le(b + 24, sample_rate);
    put_u32_le(b + 28, sample_rate * 2);  // byte rate (mono, 16-bit)
    put_u16_le(b + 32, 2);           // block align
    put_u16_le(b + 34, 16);          // bits per sample
    memcpy(b + 36, "data", 4);
    put_u32_le(b + 40, data_bytes);
}

int skip_wav_header(const uint8_t* buf, size_t len, uint32_t* pcm_bytes_out) {
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t chunk_size = buf[off + 4] | (buf[off + 5] << 8)
                            | (buf[off + 6] << 16) | (buf[off + 7] << 24);
        if (memcmp(buf + off, "data", 4) == 0) {
            if (pcm_bytes_out) *pcm_bytes_out = chunk_size;
            return (int)(off + 8);
        }
        off += 8 + chunk_size;
    }
    return -1;
}

}  // namespace audio
