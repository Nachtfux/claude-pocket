#include "anthropic.h"

#include <ArduinoJson.h>

#include "../../config.h"
#include "http.h"

namespace net {

namespace {
constexpr const char* HOST = "api.anthropic.com";
constexpr uint16_t PORT = 443;

bool ends_sentence(char c) { return c == '.' || c == '!' || c == '?' || c == '\n'; }
}  // namespace

const char* default_system_prompt() {
    return
        "You are Claude Pocket, running on a tiny handheld device with a small "
        "screen and a speaker. Replies are spoken aloud and shown on a 240x135 "
        "display, and capped at ~30 seconds of speech (roughly two or three "
        "short sentences). Always answer in that budget. If a question really "
        "needs more, give the key point first, then ask whether the user "
        "wants detail (\"Want me to expand on that?\") instead of dumping "
        "everything at once. Because answers are spoken: no markdown, no "
        "bullet lists, no code blocks, no URLs read aloud, no emoji. Write "
        "the way a person would say it. Match the user's language (German "
        "or English). If you didn't catch something, say so and ask them "
        "to repeat.";
}

std::string claude_stream(const char* user_text,
                          const std::string& history_json,
                          std::function<void(const std::string&)> on_token,
                          std::function<void(const std::string&)> on_sentence,
                          const char* system_prompt) {
    JsonDocument req;
    req["model"] = CLAUDE_MODEL;
    req["max_tokens"] = CLAUDE_MAX_TOKENS;
    req["stream"] = true;
    req["system"] = system_prompt ? system_prompt : default_system_prompt();

    JsonArray messages = req["messages"].to<JsonArray>();
    if (!history_json.empty()) {
        JsonDocument prior;
        if (deserializeJson(prior, history_json) == DeserializationError::Ok) {
            for (JsonObject m : prior.as<JsonArray>()) messages.add(m);
        }
    }
    JsonObject user_msg = messages.add<JsonObject>();
    user_msg["role"] = "user";
    user_msg["content"] = user_text;

    std::string body;
    serializeJson(req, body);

    char headers[512];
    snprintf(headers, sizeof(headers),
             "x-api-key: %s\r\n"
             "anthropic-version: 2023-06-01\r\n"
             "Content-Type: application/json\r\n"
             "Accept: text/event-stream\r\n",
             ANTHROPIC_API_KEY);

    std::string full_reply;
    std::string sentence_buf;
    std::string line_buf;

    auto process_event = [&](const std::string& line) {
        if (line.rfind("data: ", 0) != 0) return;
        std::string payload = line.substr(6);
        if (payload == "[DONE]") return;
        JsonDocument ev;
        if (deserializeJson(ev, payload) != DeserializationError::Ok) return;
        const char* type = ev["type"];
        if (!type) return;
        if (strcmp(type, "content_block_delta") == 0) {
            const char* delta = ev["delta"]["text"];
            if (!delta) return;
            std::string s = delta;
            full_reply += s;
            sentence_buf += s;
            on_token(s);
            // Flush sentence chunks to TTS as soon as one ends.
            size_t last_terminator = std::string::npos;
            for (size_t i = 0; i < sentence_buf.size(); ++i) {
                if (ends_sentence(sentence_buf[i])) last_terminator = i;
            }
            if (last_terminator != std::string::npos && last_terminator >= 8) {
                std::string done = sentence_buf.substr(0, last_terminator + 1);
                sentence_buf.erase(0, last_terminator + 1);
                on_sentence(done);
            }
        }
    };

    int status = post_streaming(HOST, PORT, "/v1/messages",
                                headers, body.size(),
                                [&](WiFiClientSecure& c) {
                                    c.write((const uint8_t*)body.data(), body.size());
                                },
                                [&](const uint8_t* buf, size_t n) {
                                    for (size_t i = 0; i < n; ++i) {
                                        char ch = (char)buf[i];
                                        if (ch == '\n') {
                                            while (!line_buf.empty() && line_buf.back() == '\r')
                                                line_buf.pop_back();
                                            process_event(line_buf);
                                            line_buf.clear();
                                        } else {
                                            line_buf += ch;
                                        }
                                    }
                                });
    if (!sentence_buf.empty()) on_sentence(sentence_buf);
    return status == 200 ? full_reply : std::string();
}

}  // namespace net
