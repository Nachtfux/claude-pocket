#include "openai.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "../../config.h"
#include "../audio/wav.h"
#include "../keys.h"
#include "http.h"

namespace net {

namespace {
constexpr const char* HOST = "api.openai.com";
constexpr uint16_t PORT = 443;
constexpr const char* BOUNDARY = "----ClaudePocketBoundary7Z";

int g_last_status = -1;
int g_last_tts_status = -1;
}  // namespace
int last_transcribe_status() { return g_last_status; }
int last_tts_status() { return g_last_tts_status; }
namespace {

// Build the fixed multipart prefix + suffix around the WAV payload.
std::string multipart_prefix() {
    std::string s;
    s.reserve(512);
    s += "--"; s += BOUNDARY; s += "\r\n";
    s += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    s += STT_MODEL;
    s += "\r\n--"; s += BOUNDARY; s += "\r\n";
    if (sizeof(STT_LANGUAGE) > 1) {
        s += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
        s += STT_LANGUAGE;
        s += "\r\n--"; s += BOUNDARY; s += "\r\n";
    }
    s += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n";
    s += "json";
    s += "\r\n--"; s += BOUNDARY; s += "\r\n";
    s += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
    s += "Content-Type: audio/wav\r\n\r\n";
    return s;
}

std::string multipart_suffix() {
    std::string s = "\r\n--";
    s += BOUNDARY;
    s += "--\r\n";
    return s;
}

}  // namespace

std::string transcribe(const char* path, size_t sample_count) {
    audio::WavHeader hdr;
    audio::make_wav_header(hdr, sample_count, 16000);

    std::string prefix = multipart_prefix();
    std::string suffix = multipart_suffix();
    const size_t pcm_bytes = sample_count * 2;
    const size_t content_length = prefix.size() + sizeof(hdr.bytes) + pcm_bytes + suffix.size();

    char headers[512];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Content-Type: multipart/form-data; boundary=%s\r\n",
             app::keys::openai_key(), BOUNDARY);

    std::string body;
    body.reserve(2048);

    auto write_all = [](WiFiClientSecure& c, const uint8_t* data, size_t len) -> bool {
        size_t off = 0;
        unsigned t_start = millis();
        while (off < len) {
            size_t want = len - off;
            if (want > 1024) want = 1024;
            size_t w = c.write(data + off, want);
            if (w == 0) {
                if (millis() - t_start > 30000) {
                    Serial.printf("[http] write timed out at %u/%u\n", off, len);
                    return false;
                }
                delay(5);
                continue;
            }
            off += w;
        }
        return true;
    };

    int status = post_streaming(HOST, PORT, "/v1/audio/transcriptions",
                                headers, content_length,
                                [&](WiFiClientSecure& c) {
                                    write_all(c, (const uint8_t*)prefix.data(), prefix.size());
                                    write_all(c, hdr.bytes, sizeof(hdr.bytes));
                                    // Stream the PCM out of LittleFS in 1 KB
                                    // chunks. This keeps the upload's working
                                    // memory tiny (~1 KB instead of the full
                                    // ~160 KB body buffer) so mbedTLS isn't
                                    // forced to drain a giant send window
                                    // under heap pressure.
                                    File f = LittleFS.open(path, "r");
                                    if (!f) {
                                        Serial.printf("[stt] open(%s) failed\n", path);
                                    } else {
                                        static uint8_t fbuf[1024];  // .bss, off the loopTask stack
                                        size_t sent = 0;
                                        while (sent < pcm_bytes) {
                                            size_t want = pcm_bytes - sent;
                                            if (want > sizeof(fbuf)) want = sizeof(fbuf);
                                            int r = f.read(fbuf, want);
                                            if (r <= 0) break;
                                            if (!write_all(c, fbuf, (size_t)r)) break;
                                            sent += (size_t)r;
                                        }
                                        f.close();
                                    }
                                    write_all(c, (const uint8_t*)suffix.data(), suffix.size());
                                },
                                [&](const uint8_t* buf, size_t n) {
                                    body.append((const char*)buf, n);
                                });
    g_last_status = status;
    if (status != 200) {
        Serial.printf("[stt] status=%d, body=%.300s\n", status, body.c_str());
        return "";
    }

    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err != DeserializationError::Ok) {
        Serial.printf("[stt] json parse err: %s\n", err.c_str());
        return "";
    }
    return doc["text"].as<std::string>();
}

// ----------------------------------------------------------------------------
// tts_to_file — download the entire TTS response to LittleFS, then return.
//
// Decouples the network from the speaker. Earlier streaming designs played
// straight off the socket and underran whenever mbedTLS hit a gap between
// records; here the speaker reads the file once the download is complete,
// so playback can't starve regardless of how the network paced bytes.
//
// Talks plain TLS to api.openai.com:443 with setInsecure(). The
// shrunk g_shared_buf has freed ~100 KB of heap, so mbedTLS no longer
// has to wedge its send window on this chip. The read loop is byte-counted
// and ignores transient `available()==0` rather than treating it as EOF,
// which was the other half of the old stall.
bool tts_to_file(const char* text, const char* file_path, size_t* out_bytes,
                 void (*tick)()) {
    if (out_bytes) *out_bytes = 0;

    JsonDocument req;
    req["model"] = TTS_MODEL;
    req["voice"] = TTS_VOICE;
    req["input"] = text;
    req["response_format"] = TTS_FORMAT;
    std::string body;
    serializeJson(req, body);

    WiFiClientSecure c;
    c.setInsecure();
    c.setHandshakeTimeout(15);

    if (!c.connect("api.openai.com", 443, 15000)) {
        Serial.printf("[tts] connect failed\n");
        g_last_tts_status = -1;
        return false;
    }

    c.printf("POST /v1/audio/speech HTTP/1.1\r\n"
             "Host: api.openai.com\r\n"
             "Authorization: Bearer %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n"
             "\r\n",
             app::keys::openai_key(), (unsigned)body.size());
    size_t written = 0;
    while (written < body.size()) {
        size_t w = c.write((const uint8_t*)body.data() + written,
                           body.size() - written);
        if (w == 0) { delay(2); continue; }
        written += w;
    }
    c.flush();

    // Read response headers up to "\r\n\r\n"; parse status,
    // Content-Length and Transfer-Encoding.
    std::string headers;
    headers.reserve(1024);
    uint32_t header_deadline = millis() + 20000;
    while (millis() < header_deadline) {
        int n = c.available();
        if (n <= 0) {
            if (!c.connected() && c.available() == 0) break;
            delay(5);
            continue;
        }
        char tmp[128];
        int r = c.read((uint8_t*)tmp, n > (int)sizeof(tmp) ? (int)sizeof(tmp) : n);
        if (r <= 0) continue;
        headers.append(tmp, r);
        if (headers.find("\r\n\r\n") != std::string::npos) break;
    }
    int status = 0;
    {
        size_t sp = headers.find(' ');
        if (sp != std::string::npos && headers.size() > sp + 4)
            status = atoi(headers.substr(sp + 1, 3).c_str());
    }
    g_last_tts_status = status;
    size_t hdr_end = headers.find("\r\n\r\n");
    std::string hdrs_lower = (hdr_end == std::string::npos)
                              ? headers
                              : headers.substr(0, hdr_end);
    for (auto& cc : hdrs_lower) cc = (char)tolower(cc);
    bool chunked = hdrs_lower.find("transfer-encoding: chunked") != std::string::npos;
    long content_length = -1;
    {
        size_t pos = hdrs_lower.find("content-length:");
        if (pos != std::string::npos) {
            content_length = strtol(hdrs_lower.c_str() + pos + 15, nullptr, 10);
        }
    }
    if (status != 200) {
        std::string err;
        uint32_t to = millis() + 3000;
        while (millis() < to && err.size() < 512) {
            int n = c.available();
            if (n <= 0) { if (!c.connected()) break; delay(5); continue; }
            char x;
            if (c.read((uint8_t*)&x, 1) == 1) err += x;
        }
        Serial.printf("[tts] status=%d, body=%.300s\n", status, err.c_str());
        c.stop();
        return false;
    }

    // Anything in `headers` past the "\r\n\r\n" separator belongs to the
    // body — hand it off to the parser before we start fresh reads.
    std::string body_prefix;
    if (hdr_end != std::string::npos && headers.size() > hdr_end + 4) {
        body_prefix = headers.substr(hdr_end + 4);
    }
    headers.clear();
    headers.shrink_to_fit();

    File f = LittleFS.open(file_path, "w");
    if (!f) {
        Serial.printf("[tts] open(%s) for write failed\n", file_path);
        c.stop();
        return false;
    }

    // ---------- body read ----------
    // Two code paths:
    //  * Content-Length / Connection: close (no chunking): read raw bytes
    //    until we've received content_length, or until the socket really
    //    closes (with no buffered data left).
    //  * Transfer-Encoding: chunked: tiny state machine to strip the
    //    "hex-size\r\n…data…\r\n" framing.
    // In both cases the loop ignores transient available()==0 — we wait
    // up to IDLE_MS of *no incoming bytes at all* before giving up. The
    // old code treated 0-read as EOF and bailed at the first record gap.
    size_t total_pcm = 0;
    bool   partition_full = false;
    uint32_t t_body_start = millis();
    uint32_t last_data_ms = millis();
    constexpr uint32_t IDLE_MS = 30000;            // 30 s of dead silence → bail
    constexpr uint32_t OVERALL_BUDGET_MS = 180000; // 3 min hard cap

    auto write_pcm = [&](const uint8_t* data, size_t n) {
        if (n == 0 || partition_full) return;
        size_t w = f.write(data, n);
        if (w < n) {
            // LittleFS partition is full (default_8MB.csv ships ~1.5 MB,
            // which is ~31 s of 24 kHz mono PCM). Stop trying to write —
            // the partial file we have will still play back what fits.
            Serial.printf("[tts] partition full at %u bytes (wrote %u of %u)\n",
                          (unsigned)total_pcm, (unsigned)w, (unsigned)n);
            partition_full = true;
            total_pcm += w;
        } else {
            total_pcm += w;
        }
        last_data_ms = millis();
    };

    if (!body_prefix.empty()) {
        if (chunked) {
            // Process prefix through the chunked machine — done below.
        } else {
            write_pcm((const uint8_t*)body_prefix.data(), body_prefix.size());
        }
    }

    if (chunked) {
        enum CH { CH_SIZE, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF,
                  CH_TRAIL_CR, CH_TRAIL_LF, CH_DONE };
        CH        state         = CH_SIZE;
        size_t    chunk_left    = 0;
        std::string size_buf;

        auto feed = [&](const uint8_t* data, size_t n) {
            size_t i = 0;
            while (i < n && state != CH_DONE) {
                if (state == CH_DATA) {
                    size_t take = n - i;
                    if (take > chunk_left) take = chunk_left;
                    write_pcm(data + i, take);
                    i += take;
                    chunk_left -= take;
                    if (chunk_left == 0) state = CH_DATA_CR;
                    continue;
                }
                uint8_t b = data[i++];
                switch (state) {
                    case CH_SIZE:
                        if (b == '\r') state = CH_SIZE_LF;
                        else if (b != ';' && b != ' ') size_buf += (char)b;
                        break;
                    case CH_SIZE_LF:
                        if (b == '\n') {
                            chunk_left = strtoul(size_buf.c_str(), nullptr, 16);
                            size_buf.clear();
                            state = (chunk_left == 0) ? CH_TRAIL_CR : CH_DATA;
                        }
                        break;
                    case CH_DATA_CR: if (b == '\r') state = CH_DATA_LF; break;
                    case CH_DATA_LF: if (b == '\n') state = CH_SIZE;    break;
                    case CH_TRAIL_CR: if (b == '\r') state = CH_TRAIL_LF; break;
                    case CH_TRAIL_LF: if (b == '\n') state = CH_DONE;    break;
                    default: break;
                }
            }
        };

        if (!body_prefix.empty()) {
            feed((const uint8_t*)body_prefix.data(), body_prefix.size());
        }

        static uint8_t buf[1024];   // .bss; the loopTask stack is only 8 KB
        uint32_t last_tick_ms = millis();
        while (state != CH_DONE && millis() - t_body_start < OVERALL_BUDGET_MS) {
            if (tick && millis() - last_tick_ms >= 80) {
                last_tick_ms = millis();
                tick();
            }
            int n = c.available();
            if (n <= 0) {
                if (!c.connected() && c.available() == 0) break;
                if (millis() - last_data_ms > IDLE_MS) {
                    Serial.printf("[tts] idle stall, giving up at %u bytes\n",
                                  (unsigned)total_pcm);
                    break;
                }
                delay(5);
                continue;
            }
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = c.read(buf, n);
            if (r <= 0) continue;
            feed(buf, (size_t)r);
        }
    } else {
        // Identity-encoded body. We trust Content-Length if present;
        // otherwise we read until the socket closes.
        static uint8_t buf[1024];   // .bss; see chunked branch above
        uint32_t last_tick_ms = millis();
        while (millis() - t_body_start < OVERALL_BUDGET_MS) {
            if (tick && millis() - last_tick_ms >= 80) {
                last_tick_ms = millis();
                tick();
            }
            if (content_length >= 0 && (long)total_pcm >= content_length) break;
            int n = c.available();
            if (n <= 0) {
                if (!c.connected() && c.available() == 0) break;
                if (millis() - last_data_ms > IDLE_MS) {
                    Serial.printf("[tts] idle stall, giving up at %u bytes\n",
                                  (unsigned)total_pcm);
                    break;
                }
                delay(5);
                continue;
            }
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = c.read(buf, n);
            if (r <= 0) continue;
            write_pcm(buf, (size_t)r);
        }
    }

    f.close();
    c.stop();
    Serial.printf("[tts] downloaded %u bytes in %u ms%s\n",
                  (unsigned)total_pcm,
                  (unsigned)(millis() - t_body_start),
                  partition_full ? " (TRUNCATED — partition full)" : "");
    if (out_bytes) *out_bytes = total_pcm;
    return total_pcm > 0;
}

}  // namespace net
