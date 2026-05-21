#pragma once

#include <WiFiClientSecure.h>

#include <functional>

namespace net {

// POST helper that streams a body produced by `writer` (so we never need a
// full multipart body in RAM) and pipes the response body through `on_chunk`
// for the same reason. Returns HTTP status code, or -1 on transport failure.
//
// The TLS client is constructed locally per call (setInsecure — passive
// eavesdropping is still blocked but active MITM is possible; replace with
// setCACert() of pinned roots before public deployment) so its memory is
// released between calls; a long-lived singleton fragmented the heap enough
// that the third consecutive handshake stalled.
//
// Headers are passed as a single contiguous "Key: Value\r\n..." string.
int post_streaming(const char* host,
                   uint16_t port,
                   const char* path,
                   const char* headers,
                   size_t content_length,
                   std::function<void(WiFiClientSecure&)> writer,
                   std::function<void(const uint8_t*, size_t)> on_chunk);

}  // namespace net
