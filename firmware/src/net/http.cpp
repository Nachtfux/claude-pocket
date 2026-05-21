#include "http.h"

#include <Arduino.h>
#include <WiFi.h>

#include <string>

namespace net {

int post_streaming(const char* host,
                   uint16_t port,
                   const char* path,
                   const char* headers,
                   size_t content_length,
                   std::function<void(WiFiClientSecure&)> writer,
                   std::function<void(const uint8_t*, size_t)> on_chunk) {
    // Use a local WiFiClientSecure so all TLS state — including the mbedTLS
    // session, record buffers, and any heap allocations BearSSL holds onto —
    // is freed when this function returns. The previous static singleton kept
    // ~32 KB pinned across STT → Claude → TTS, and by the third handshake the
    // heap was too fragmented to find a contiguous block, leaving the call
    // hanging mid-handshake.
    WiFiClientSecure c;
    c.setInsecure();
    // Arduino-esp32 reuses the handshake timeout as the SSL read timeout,
    // so this also caps how long mbedtls_ssl_read may block per call.
    c.setHandshakeTimeout(3);    // seconds
    c.setTimeout(15);            // seconds — underlying socket recv timeout
    if (!c.connect(host, port, 15000)) {
        Serial.printf("[http] connect to %s:%u failed\n", host, (unsigned)port);
        return -1;
    }

    c.printf("POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "Content-Length: %u\r\n"
             "%s\r\n",
             path, host, (unsigned)content_length, headers);
    writer(c);
    c.flush();

    // Read in chunks. Buffer only until we find the end of HTTP headers, then
    // stream the body directly to on_chunk so large responses (e.g. 200 KB
    // TTS WAV) don't blow up the heap. When the response uses chunked
    // transfer-encoding, decode it inline so on_chunk sees clean payload.
    enum ChunkSt { CH_LEN, CH_LEN_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF,
                   CH_END_CR, CH_END_LF, CH_DONE };
    ChunkSt  chunk_state     = CH_LEN;
    uint32_t chunk_remaining = 0;
    std::string chunk_len_acc;
    bool chunked    = false;
    bool body_phase = false;

    auto feed_chunked = [&](const uint8_t* buf, size_t n) -> size_t {
        size_t emitted = 0;
        size_t i = 0;
        while (i < n && chunk_state != CH_DONE) {
            if (chunk_state == CH_DATA) {
                size_t avail = n - i;
                size_t take = avail < chunk_remaining ? avail : chunk_remaining;
                if (take > 0) { on_chunk(buf + i, take); emitted += take; }
                i += take;
                chunk_remaining -= take;
                if (chunk_remaining == 0) chunk_state = CH_DATA_CR;
                continue;
            }
            uint8_t b = buf[i++];
            switch (chunk_state) {
                case CH_LEN:
                    if (b == '\r') chunk_state = CH_LEN_LF;
                    else if (b != ';' && b != ' ') chunk_len_acc += (char)b;
                    break;
                case CH_LEN_LF:
                    if (b == '\n') {
                        chunk_remaining = strtoul(chunk_len_acc.c_str(), nullptr, 16);
                        chunk_len_acc.clear();
                        chunk_state = (chunk_remaining == 0) ? CH_END_CR : CH_DATA;
                    }
                    break;
                case CH_DATA_CR: if (b == '\r') chunk_state = CH_DATA_LF; break;
                case CH_DATA_LF: if (b == '\n') chunk_state = CH_LEN;     break;
                case CH_END_CR:  if (b == '\r') chunk_state = CH_END_LF;  break;
                case CH_END_LF:  if (b == '\n') chunk_state = CH_DONE;    break;
                default: break;
            }
        }
        return emitted;
    };

    std::string header_acc;
    header_acc.reserve(1024);
    int status = -1;
    static uint8_t rbuf[1024];   // .bss; loopTask stack is only 8 KB
    unsigned deadline = millis() + 30000;
    unsigned last_data_ms = millis();
    while (true) {
        unsigned now = millis();
        if (now >= deadline) {
            Serial.printf("[http] %s read deadline\n", path);
            break;
        }
        int n = c.available();
        if (n <= 0) {
            if (!c.connected()) break;        // peer closed cleanly
            if (now - last_data_ms > 25000) { // no bytes for 25 s
                Serial.printf("[http] %s idle stall\n", path);
                break;
            }
            delay(5);
            continue;
        }
        last_data_ms = now;
        if (n > (int)sizeof(rbuf)) n = sizeof(rbuf);
        int r = c.read(rbuf, n);
        if (r <= 0) { if (r < 0) break; continue; }
        if (!body_phase) {
            header_acc.append((const char*)rbuf, (size_t)r);
            size_t hdr_end = header_acc.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                int sp = header_acc.find(' ');
                if (sp > 0) status = atoi(header_acc.substr(sp + 1, 3).c_str());
                std::string hdrs_lower = header_acc.substr(0, hdr_end);
                for (auto& cc : hdrs_lower) cc = (char)tolower(cc);
                chunked = hdrs_lower.find("transfer-encoding: chunked") != std::string::npos;
                // Log non-2xx response headers in full so 302 / 4xx errors are
                // diagnosable without re-flashing. Limit to first 800 chars so
                // we never blow the serial buffer.
                if (status < 200 || status >= 300) {
                    Serial.printf("[http] %s %s -> %d, response headers:\n",
                                  path, host, status);
                    std::string preview = header_acc.substr(0, hdr_end);
                    if (preview.size() > 800) preview.resize(800);
                    Serial.printf("%s\n", preview.c_str());
                }
                size_t body_off = hdr_end + 4;
                if (body_off < header_acc.size()) {
                    const uint8_t* tail = (const uint8_t*)(header_acc.data() + body_off);
                    size_t tail_len = header_acc.size() - body_off;
                    if (chunked) feed_chunked(tail, tail_len);
                    else         on_chunk(tail, tail_len);
                }
                header_acc.clear();
                header_acc.shrink_to_fit();
                body_phase = true;
            }
        } else {
            if (chunked) feed_chunked(rbuf, (size_t)r);
            else         on_chunk(rbuf, (size_t)r);
        }
        if (chunked && chunk_state == CH_DONE) break;
    }
    c.stop();
    return status;
}

}  // namespace net
