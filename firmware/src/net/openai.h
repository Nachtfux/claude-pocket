#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>

namespace net {

// Transcribe a LittleFS-stored 16 kHz mono int16 PCM file via OpenAI Whisper.
// `path` points at the raw little-endian samples — we wrap them in a WAV
// header at upload time. `sample_count` is the total number of samples in
// the file (file size / 2). Streaming the file means we don't have to keep
// the whole recording in RAM, so the only practical length cap is the
// LittleFS partition size and Whisper's request timeout.
std::string transcribe(const char* path, size_t sample_count);

// HTTP status code from the last transcribe() call. -1 on transport failure,
// otherwise the server's response code. Used by the UI to show error context
// without keeping a stateful struct in the public API.
int last_transcribe_status();

// HTTP status code from the last tts_stream() call. -1 on transport failure.
int last_tts_status();

// Download a full TTS response to a LittleFS file, then return. Talks
// straight to api.openai.com:443 (no proxy). The body lands in `file_path`
// as raw little-endian int16 PCM at 24 kHz mono. Caller plays it back
// after the call returns — this decouples the speaker from the network
// completely, so chunk-gap stalls in mbedTLS can't underrun playback.
//
// `tick` (optional) is called repeatedly during the body read so the UI
// can keep an animation running while the main loop is blocked here.
//
// Returns true and stores the PCM byte count in `*out_bytes` on success.
bool tts_to_file(const char* text, const char* file_path, size_t* out_bytes,
                 void (*tick)() = nullptr);

}  // namespace net
