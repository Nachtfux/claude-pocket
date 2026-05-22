#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr int      CHANNELS    = 1;

// Setup mic + speaker through M5Unified's ES8311 driver. Idempotent.
void begin();

// Tear down both sides of the ES8311 + NS4150 path so the device runs
// truly silent between sessions. M5.Speaker.end()/M5.Mic.end() each only
// touch their own half — when something else (like the radio's direct
// I2S writes) leaves the codec mid-stream, you'll hear a low hiss until
// this is called.
void silence_codec();

// Apply the cached settings::store volume.
void set_volume(uint8_t pct);

// Chunked recording for VAD-driven capture. mic_open() prepares the ES8311;
// mic_read_chunk() reads up to `max_samples` (capped internally to ~100 ms);
// mic_close() releases the codec. Between open/close, last_level_rms()
// reflects the most recent chunk.
void mic_open();
size_t mic_read_chunk(int16_t* buf, size_t max_samples);
void mic_close();

// One-shot playback used by the TTS path. Block until the I2S DMA has drained
// the entire buffer, then return. Caller must keep `buf` alive for the
// duration of this call. Wrap with begin_stream()/end_stream() to toggle the
// shared mic/speaker I2S pins.
void begin_stream();
void play_blocking(const int16_t* buf, size_t samples, uint32_t sample_rate);
void end_stream();

// Streaming playback for the TTS path. stream_begin() opens the speaker side
// of the codec, allocates a 16 KB ring buffer, and spawns a feeder task that
// pulls PCM bytes from the ring and pushes them to M5.Speaker.playRaw in
// small slices. The caller can then push int16 mono PCM into the ring with
// stream_push() at whatever speed the network delivers.
//
// pool_buf / pool_samples is a *caller-owned* scratch area that the feeder
// slices into per-slab playback buffers. M5.Speaker.playRaw stores a
// pointer to the audio, not a copy, so we have to rotate through several
// buffers so the next slab doesn't overwrite samples that are still being
// streamed (that overlap is what produced the audible doubling/echo).
// Pass a region you're not using during playback — the mic recording
// buffer is ideal because mic + speaker never run together on this codec.
void stream_begin(uint32_t sample_rate, int16_t* pool_buf, size_t pool_samples);
void stream_push(const uint8_t* bytes, size_t n);
void stream_end();

// Optional callback invoked from stream_end()'s drain loop (~every 20 ms)
// while the speaker is finishing its queue. Useful for keeping a UI
// animated during TTS playback, since the main loop is blocked through
// stream_end. Pass nullptr to clear.
void set_stream_tick(void (*cb)());

// Instantaneous mic level for VAD and waveform visualization. Returns RMS of
// the most recent record buffer in [0, 32767].
uint16_t last_level_rms();

}  // namespace audio
