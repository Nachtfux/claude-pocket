// config.example.h
// Copy this file to "config.h" and fill in your real keys.
// config.h is git-ignored on purpose so your keys are never published,
// even though they are compiled ("hardcoded") into the local firmware build.

#pragma once

// --- API keys ---------------------------------------------------------------
#define OPENAI_API_KEY     "sk-...replace-me..."
#define ANTHROPIC_API_KEY  "sk-ant-...replace-me..."

// --- Models -----------------------------------------------------------------
#define STT_MODEL          "gpt-4o-mini-transcribe"   // or "whisper-1"
#define CLAUDE_MODEL       "claude-sonnet-4-6"         // requested: Sonnet
#define TTS_MODEL          "gpt-4o-mini-tts"
#define TTS_VOICE          "coral"                     // try: alloy, coral, sage

// --- Behaviour --------------------------------------------------------------
#define STT_LANGUAGE       "de"        // "" = auto-detect
#define CLAUDE_MAX_TOKENS  120         // ~30 s of spoken audio
#define TTS_FORMAT         "pcm"       // raw 24 kHz mono int16 — no decoder

// --- Optional default Wi-Fi (Settings can override & persist) ---------------
#define DEFAULT_WIFI_SSID  ""
#define DEFAULT_WIFI_PASS  ""
