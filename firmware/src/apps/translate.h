#pragma once

namespace apps {
namespace translate {

// Voice translator. Press OK, speak in the source language, the device
// transcribes (Whisper), translates via Claude with a translation-only
// system prompt, and reads the result back through TTS. Press L to swap
// source and target language. Default direction: DE → EN.
void enter();
void tick();

}  // namespace translate
}  // namespace apps
