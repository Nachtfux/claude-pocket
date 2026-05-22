#include "io.h"

#include <M5Unified.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "../settings/store.h"

namespace audio {

namespace {
uint16_t g_last_rms = 0;

// Streaming PCM playback state. The ring is a byte buffer because TLS
// records arrive at arbitrary boundaries and may straddle a sample edge;
// downstream we re-align via `g_carry`.
// 16 KB ring (~340 ms of audio at 24 kHz) keeps the feeder fed through
// flash-read and playRaw-queue jitter without underruns. The bigger ring
// was a relief once the TTS body started downloading to a file first
// instead of streaming straight to the speaker.
constexpr size_t  STREAM_RING_BYTES = 16 * 1024;
RingbufHandle_t   g_stream_ring     = nullptr;
TaskHandle_t      g_feeder_task     = nullptr;
volatile bool     g_stream_running  = false;
uint32_t          g_stream_rate     = 24000;
int16_t*          g_pool_buf        = nullptr;
size_t            g_pool_samples    = 0;
void (*g_stream_tick)()             = nullptr;

void feeder_task(void*) {
    // M5.Speaker.playRaw stores a POINTER (not a copy). Reusing one buffer
    // overwrote samples the codec was still streaming → audible doubling.
    // We slice the caller-provided pool (g_shared_buf in pocket.cpp — the
    // mic recording buffer, idle during playback) into N rotating slabs.
    // No new .bss; mbedTLS heap budget for the next STT call is untouched.
    constexpr size_t BUF_SAMPLES = 4096;
    int num_bufs = (int)(g_pool_samples / BUF_SAMPLES);
    if (num_bufs < 2) num_bufs = 2;
    if (num_bufs > 16) num_bufs = 16;
    int next_buf = 0;

    // Pre-buffer: hold off the first playRaw until the ring has ~250 ms of
    // audio queued. Without this the very first pull is whatever tiny push
    // happened first (often just 256 samples = 10 ms), the speaker DMA
    // immediately under-runs, and the first syllable of every reply comes
    // out clipped or swallowed.
    constexpr size_t PREBUFFER_BYTES = 12 * 1024;
    while (g_stream_ring && g_stream_running) {
        size_t used = STREAM_RING_BYTES - xRingbufferGetCurFreeSize(g_stream_ring);
        if (used >= PREBUFFER_BYTES) break;
        delay(5);
    }

    while (g_stream_running || (g_stream_ring && xRingbufferGetCurFreeSize(g_stream_ring) < STREAM_RING_BYTES)) {
        if (!g_stream_ring) break;
        size_t got = 0;
        int16_t* out = g_pool_buf + (size_t)next_buf * BUF_SAMPLES;
        void* p = xRingbufferReceiveUpTo(g_stream_ring, &got,
                                         pdMS_TO_TICKS(100), BUF_SAMPLES * 2);
        if (!p || got == 0) {
            if (p) vRingbufferReturnItem(g_stream_ring, p);
            continue;
        }
        size_t samples = got / 2;
        memcpy(out, p, samples * 2);
        vRingbufferReturnItem(g_stream_ring, p);
        next_buf = (next_buf + 1) % num_bufs;
        if (samples > 0) {
            // Retry until the speaker accepts this slab. playRaw returns
            // false when its internal play-job queue is full; without the
            // retry loop most slabs were dropped and the playback came
            // out as garbled fragments. taskYIELD() instead of delay(2)
            // because the queue typically frees within microseconds and
            // a 2 ms sleep was audible as a gap. channel=0 pins every
            // slab to the same mixer slot so they play sequentially —
            // channel=-1 (auto) mixed hundreds of overlapping clips.
            uint32_t t0 = millis();
            while (!M5.Speaker.playRaw(out, samples, g_stream_rate,
                                       /*stereo=*/false, /*repeat=*/1,
                                       /*channel=*/0)) {
                if (millis() - t0 > 5000) {
                    Serial.printf("[feeder] playRaw stuck >5 s, dropping slab\n");
                    break;
                }
                taskYIELD();
            }
        }
    }
    g_feeder_task = nullptr;
    vTaskDelete(nullptr);
}

void compute_rms(const int16_t* buf, size_t n) {
    if (n == 0) { g_last_rms = 0; return; }
    int64_t sum_mean = 0;
    for (size_t i = 0; i < n; ++i) sum_mean += buf[i];
    int32_t mean = (int32_t)(sum_mean / (int64_t)n);
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t d = (int32_t)buf[i] - mean;
        sum_sq += (uint32_t)(d * d);
    }
    g_last_rms = (uint16_t)sqrtf((float)(sum_sq / n));
}
}  // namespace

void set_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    M5.Speaker.setVolume((uint8_t)(pct * 255 / 100));
}

void begin() {
    // Apply the persisted Settings volume once and start in mic-side state.
    // The mic and speaker share the ES8311 codec and cannot be active at the
    // same time; each transition tears one side down before bringing the
    // other up.
    set_volume(settings::store().volume_pct);
    silence_codec();
}

void silence_codec() {
    while (M5.Mic.isRecording())   delay(1);
    M5.Mic.end();
    while (M5.Speaker.isPlaying()) delay(1);
    M5.Speaker.end();
}

void mic_open() {
    M5.Speaker.end();
    M5.Mic.begin();
    // Touching M5.Mic.config() to bump DMA buffers wrecked transcription —
    // it must reset some auto-detected board fields. Stick with the
    // library defaults and accept that occasional LittleFS sector erases
    // may cause a brief audio drop; in practice writes are fast enough
    // that this hasn't been noticeable.
}

size_t mic_read_chunk(int16_t* buf, size_t max_samples) {
    size_t want = max_samples;
    if (want > 1600) want = 1600;
    if (!M5.Mic.record(buf, want, SAMPLE_RATE, /*stereo=*/false)) return 0;
    compute_rms(buf, want);
    return want;
}

void mic_close() {
    while (M5.Mic.isRecording()) delay(1);
    M5.Mic.end();
}

void begin_stream() {
    while (M5.Mic.isRecording()) delay(1);
    M5.Mic.end();
    M5.Speaker.begin();
    set_volume(settings::store().volume_pct);
}

void play_blocking(const int16_t* buf, size_t samples, uint32_t sample_rate) {
    if (samples == 0) return;
    (void)M5.Speaker.playRaw(buf, samples, sample_rate, /*stereo=*/false,
                             /*repeat=*/1, /*channel=*/0);
    // Hard timeout — on Cardputer-Adv the speaker can wedge after a mic
    // session and isPlaying never goes false, so we cap how long we wait.
    uint32_t budget_ms = (samples * 1000U) / sample_rate + 1500U;
    if (budget_ms > 15000U) budget_ms = 15000U;
    uint32_t t0 = millis();
    while (M5.Speaker.isPlaying(0) && millis() - t0 < budget_ms) delay(5);
}

void end_stream() {
    while (M5.Speaker.isPlaying(0)) delay(1);
    M5.Speaker.end();
}

void stream_begin(uint32_t sample_rate, int16_t* pool_buf, size_t pool_samples) {
    g_pool_buf = pool_buf;
    g_pool_samples = pool_samples;
    while (M5.Mic.isRecording()) delay(1);
    M5.Mic.end();
    // Bump the I2S DMA buffers — default is 8×256 samples (~42 ms at
    // 48 kHz), which means any momentary gap in our playRaw enqueueing
    // drains the hardware and clicks audibly. 8×1024 samples (~170 ms)
    // gives the codec enough headroom to ride through retry/jitter.
    auto cfg = M5.Speaker.config();
    cfg.dma_buf_len   = 1024;
    cfg.dma_buf_count = 8;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
    set_volume(settings::store().volume_pct);

    g_stream_rate = sample_rate;
    g_stream_ring = xRingbufferCreate(STREAM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!g_stream_ring) {
        Serial.printf("[audio] ring buffer alloc failed\n");
        return;
    }
    g_stream_running = true;
    // Feeder runs higher than the default loop priority so it never starves
    // during heavy TLS work in the main task. 4 KB stack is plenty for the
    // tight memcpy + playRaw loop.
    xTaskCreatePinnedToCore(feeder_task, "pcm_feed", 4096, nullptr,
                            tskIDLE_PRIORITY + 3, &g_feeder_task,
                            /*core=*/1);
}

void stream_push(const uint8_t* bytes, size_t n) {
    if (!g_stream_ring || n == 0) return;
    // 10 s wait is long enough that we never drop a slab during a normal
    // playback (speaker drains a full ring in ~340 ms). The earlier 500 ms
    // ceiling silently truncated long replies whenever the ring stayed full
    // a little longer than expected.
    BaseType_t ok = xRingbufferSend(g_stream_ring, bytes, n, pdMS_TO_TICKS(10000));
    if (ok != pdTRUE) {
        Serial.printf("[push] DROPPED %u bytes — ring full, feeder stuck?\n",
                      (unsigned)n);
    }
}

void stream_end() {
    g_stream_running = false;
    // First wait for the feeder to push the last ring slabs into the
    // speaker's internal queue.
    uint32_t t0 = millis();
    while (g_feeder_task != nullptr && millis() - t0 < 10000) delay(20);
    // Wait for channel 0 (our streaming channel) to drain. Earlier code
    // exited as soon as isPlaying() returned 0, but the count flickers to
    // 0 momentarily between consecutive slabs as M5.Speaker pops one and
    // hasn't started the next yet — that cut off the last 500+ ms of
    // every reply. Require a continuous quiet window before quitting.
    uint32_t budget = 120000;   // covers long replies — was 30 s, which
                                // cut off the tail of multi-minute audio
    uint32_t quiet_start = 0;
    constexpr uint32_t REQUIRED_QUIET_MS = 600;
    while (millis() - t0 < budget) {
        bool playing = M5.Speaker.isPlaying(0);
        if (playing) {
            quiet_start = 0;
        } else {
            if (quiet_start == 0) quiet_start = millis();
            if (millis() - quiet_start >= REQUIRED_QUIET_MS) break;
        }
        if (g_stream_tick) g_stream_tick();
        delay(20);
    }
    if (g_stream_ring) {
        vRingbufferDelete(g_stream_ring);
        g_stream_ring = nullptr;
    }
    M5.Speaker.end();
}

uint16_t last_level_rms() { return g_last_rms; }

void set_stream_tick(void (*cb)()) { g_stream_tick = cb; }

}  // namespace audio
