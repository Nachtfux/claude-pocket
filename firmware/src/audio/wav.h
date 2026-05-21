#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio {

// Fixed-shape WAV header for 16-bit mono PCM. 44 bytes.
struct WavHeader {
    uint8_t bytes[44];
};

// Build a minimal RIFF/WAV header for `sample_count` int16 mono samples at
// `sample_rate`. Total file size = 44 + sample_count * 2.
void make_wav_header(WavHeader& out, uint32_t sample_count, uint32_t sample_rate);

// Locate the start of the PCM data within an incoming WAV stream. Returns the
// number of bytes consumed (the offset of the first PCM byte), or -1 if the
// header is incomplete. Sets *pcm_bytes to the chunk size of the data section.
int skip_wav_header(const uint8_t* buf, size_t len, uint32_t* pcm_bytes_out);

}  // namespace audio
