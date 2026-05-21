#pragma once

#include <functional>
#include <string>

namespace net {

// One-shot streaming call to Claude. `on_token` fires for each text delta as
// it arrives. `on_sentence` fires whenever the accumulated buffer ends in a
// terminator (. ! ?) — pass the completed sentence to TTS to overlap stages.
// Returns the full response on completion, or empty string on failure.
std::string claude_stream(const char* user_text,
                          const std::string& history_json,
                          std::function<void(const std::string&)> on_token,
                          std::function<void(const std::string&)> on_sentence);

// Default system prompt for Claude Pocket — copied from SPEC §6.4.
const char* default_system_prompt();

}  // namespace net
