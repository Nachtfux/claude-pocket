# Claude Pocket — Spec & Build Notes

> This started as the implementation brief — *what* to build — and is now also
> a record of *what was built* and the constraints that surfaced during
> implementation. Cross-references and timing numbers reflect the firmware
> on `main`; see [§12 Implementation notes](#12-implementation-notes) for the
> non-obvious decisions that aren't visible in the source.

---

## 1. Vision

A pocket voice assistant on the M5Stack Cardputer ADV. You hold a credit-card
sized device, talk to it, and Claude answers out loud — wrapped in Claude's
visual identity, with live animations on the little screen. Alongside it, a
proper launcher menu with **Snake**, **Claude Buddy**, the new **Claude Pocket**
voice app, and a **Settings** screen for Wi‑Fi, Bluetooth and speaker volume.

The headline experience ("Claude Pocket"):

```
🎙  You speak  →  OpenAI Whisper (STT)  →  Claude Sonnet (thinking)  →  OpenAI TTS  →  🔊 Claude speaks back
```

All three stages are independent cloud calls made directly from the device
(no self-hosted server). The Cardputer is the microphone, the screen, and the
speaker; the cloud does the heavy lifting.

---

## 2. Target hardware (read carefully — pins are from the device silkscreen)

**M5Stack Cardputer ADV**, core module **Stamp S3A = ESP32‑S3FN8** (8 MB flash,
no PSRAM — memory is the main constraint).

| Subsystem | Part | Key pins / notes |
|---|---|---|
| MCU | ESP32‑S3FN8 | dual‑core LX7, WiFi + BLE, **no PSRAM** |
| Display | 1.14" IPS LCD, **ST7789V2**, 240×135 | DATA `G35`, SCK `G36`, RS/DC `G34`, CS `G37`, RST `G33`, BL `G38` |
| Audio codec | **ES8311** (I2S) | ASDOUT `G46`, LRCK `G43`, BCLK/SCK `G41`, DSDIN `G42` |
| Amp + speaker | **NS4150**, 1 W speaker | output path for playback |
| Microphone | onboard MEMS mic | input via ES8311 |
| Keyboard | **TCA8418** matrix controller, 4×14 | I²C SCL `G9`, SDA `G8`, INT `G11` |
| IMU | **BMI270** (6‑axis) | I²C (shared bus) — optional, for shake/tilt |
| Power | 1750 mAh LiPo, **TP4057** charger | USB‑C, OTG D‑ `G19`, D+ `G20` |

Use **M5Unified / M5GFX / M5Cardputer** libraries — they already abstract this
board's display, keyboard and audio. Confirm the exact ADV pin map against
M5Stack's `M5Cardputer`/`M5GFX` board headers before hardcoding anything above;
the table is the on‑device reference but the library constants are authoritative.

---

## 3. Architecture decision: Arduino/C++ (recommended) vs MicroPython

**Recommendation: build the firmware in Arduino/C++ (ESP‑IDF + M5Unified).**

Rationale — the wishlist forces it:
- **Real‑time I²S audio** capture + playback with the ES8311: mature in C++,
  awkward and memory‑tight in MicroPython on a no‑PSRAM S3.
- **Wake word** ("Claude"): only realistic via Espressif **ESP‑SR / WakeNet**,
  which is C/ESP‑IDF.
- **Smooth display animations** (spinning Claude spark, waveforms): need fast
  framebuffer drawing — M5GFX sprites in C++.
- **Multiple sequential TLS calls** with audio payloads: more controllable in C++.

**Tradeoff to be aware of:** the existing "Build with Claude" launcher and the
stock **Claude Buddy** app are MicroPython (part of the `buddy` bundle). Going
C++ means we re‑implement the launcher menu and re‑create / port the menu
entries. For **Claude Buddy** specifically: either (a) port the BLE companion to
C++, or (b) keep a MicroPython build for Buddy and ship Claude Pocket as the C++
firmware, switching via M5Burner. **Decide and document this in the README.**
Default plan: single C++ firmware that re‑creates the menu (Snake, Settings,
Claude Pocket) and includes a Claude Buddy entry (port or clearly stubbed).

If C++ proves heavier than wanted, a fallback path is documented in §10, but
**do not start in MicroPython for the audio core.**

---

## 4. Main menu / launcher

A scrollable, Claude‑styled launcher. Entries:

1. **Claude Pocket** — the voice assistant (the star of the show)
2. **Claude Buddy** — existing BLE companion (port or stub, see §3)
3. **Snake** — the game
4. **Settings** — see §5

Navigation: arrow keys to move, `OK`/`Enter` to select, `esc` to go back.
Each entry is a card with the Claude spark or an icon, label, and a one‑line
description. Selected card uses the Orange accent; background is Ivory.
A persistent **status bar** (top, ~14 px) shows: Wi‑Fi state, Bluetooth state,
speaker‑volume glyph, battery %, and clock.

---

## 5. Settings screen

A simple list with these items; persist all values to flash (NVS / a
`settings.json`) so they survive reboot.

**5.1 Wi‑Fi**
- **Scan** nearby SSIDs (show signal strength + lock icon for secured nets).
- Select an SSID → enter password using the full keyboard → connect.
- Show connection state + obtained IP. Persist the last working network and
  auto‑reconnect on boot. Support storing multiple known networks.
- This replaces any hardcoded event Wi‑Fi credentials.

**5.2 Bluetooth**
- Simple **on/off** toggle. (Relevant if Claude Buddy / BLE features are used;
  turning it off saves power.)

**5.3 Speaker volume**
- 0–100 % slider/stepper, applied to the ES8311/NS4150 output gain. Play a short
  blip on change so the level is audible. Persist.

(Optional, if cheap to add: brightness, wake‑word on/off, "answer length"
toggle that maps to the system prompt.)

---

## 6. Claude Pocket — the voice feature

### 6.1 Pipeline
1. **Capture** mic audio via ES8311 I²S → 16 kHz, mono, 16‑bit PCM (keep it
   small; this is the cheapest, fastest format for upload).
2. **STT** → `POST https://api.openai.com/v1/audio/transcriptions`
   - model `gpt-4o-mini-transcribe` (faster/cheaper) or `whisper-1`.
   - language hint `de` by default, but allow auto.
3. **Brain** → `POST https://api.anthropic.com/v1/messages`
   - **model `claude-sonnet-4-6`** (per requirement). Stream the response.
   - Keep short conversation history in RAM for follow‑ups.
4. **TTS** → `POST https://api.openai.com/v1/audio/speech`
   - model `gpt-4o-mini-tts`, voice `coral`, `response_format: "pcm"` (raw
     24 kHz mono int16 — no decoder, no WAV header to skip).
5. **Playback** through NS4150 speaker at the configured volume. The full
   TTS body is downloaded to a LittleFS file first, *then* read back into
   the speaker; this decouples playback from network jitter (see §12).

### 6.2 Conversation flow — answer the user's question explicitly

Ship **three modes**, default to the most natural one that's reliable:

- **Mode A — Push‑to‑talk (PTT):** hold a key (e.g. `OK`) while speaking, release
  to send. Walkie‑talkie style. Zero false triggers, simplest, lowest risk.
  *Always implement this as the guaranteed fallback.*
- **Mode B — Tap + VAD (shipping default):** tap once to start; the device
  records and **auto‑detects when you stop speaking** (energy‑based VAD on
  the 100 ms chunks coming out of `mic_read_chunk`, endpoint after ~2.5 s
  of silence) and sends automatically. The 2.5 s window is generous enough
  to ride through mid-sentence thinking pauses; a hard 30 s cap prevents
  runaway recordings. Feels natural, no holding, no wake word needed.
- **Mode C — Wake word "Claude" (stretch goal):** fully hands‑free. See §6.3.

So: **the user does not strictly need to press a button** — Mode B gives a
"just talk, it figures out you're done" feel. Mode C removes the initial tap
too. Start with B (+A as fallback), add C once the core works.

### 6.3 Wake word "Claude" — feasibility

Yes, on‑device wake word is possible, with caveats:
- It runs through **Espressif ESP‑SR / WakeNet** on the ESP32‑S3 (C/ESP‑IDF).
- WakeNet ships several **pre‑trained** wake words (e.g. "Hi ESP", "Alexa").
  A custom **"Claude" / "Hey Claude"** word needs a **custom WakeNet model**
  generated via Espressif's process — plan for this as a separate task; until
  the custom model exists, prototype Mode C with a built‑in wake word.
- Wake word only triggers *listening*; the actual transcription still goes to
  Whisper (WakeNet does not transcribe).
- It keeps the mic + a detection loop always on → higher battery draw; gate it
  behind a Settings toggle.

### 6.4 System prompt (shipping)

The current copy lives in `firmware/src/net/anthropic.cpp::default_system_prompt`.
It tells Claude about the spoken-audio output, caps replies at ~30 s of speech
(roughly two or three short sentences), bans markdown / code / URLs / emoji,
and lets it match the user's language. Combined with `CLAUDE_MAX_TOKENS=120`
in `config.h` it's both a soft (prompt) and a hard (API) cap.

### 6.5 Keys / config
- API keys (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`) are compiled in via
  `firmware/config.h`, created from `config.example.h`.
- **`config.h` is git‑ignored** so the published repo never leaks keys, even
  though they are "hardcoded" into the local build. (User accepted hardcoding;
  this just keeps the public repo safe.)

---

## 7. Design system — "Claude design"

Use Anthropic's real brand palette (convert hex → RGB565 for the display):

| Role | Hex | Use |
|---|---|---|
| Ivory (bg) | `#faf9f5` | main background |
| Dark | `#141413` | primary text |
| Orange (primary accent) | `#d97757` | the Claude spark, highlights, active state |
| Blue | `#6a9bcc` | secondary accent (listening waveform) |
| Green | `#788c5d` | tertiary accent (success, "connected") |
| Mid Gray | `#b0aea5` | secondary text, inactive |
| Light Gray | `#e8e6dc` | cards, dividers |

Typography: brand fonts are **Poppins** (headings) / **Lora** (body). On the
embedded display use a clean bitmap/vector font in that spirit (e.g. an M5GFX
font); headings in the heavier weight, body lighter. Keep generous margins —
the Claude look is calm and uncluttered, lots of Ivory whitespace.

**The Claude spark:** draw the Anthropic "spark" mark (a radiating burst of
tapered rays from a center point) in Orange on Ivory as the app's hero icon.
Render it as an M5GFX sprite so it can rotate/scale smoothly.

### 7.1 Screen states + animations (be creative here)

| State | Screen |
|---|---|
| **Idle** | Centered Claude spark gently "breathing" (subtle scale pulse, ~0.4 Hz). Hint text: "Tippe zum Sprechen" / "Sag 'Claude'". Status bar on top. |
| **Listening** | Spark shifts to a **live waveform / VU meter** in Blue that reacts to mic amplitude in real time. Label "Ich höre zu…". A ring fills as VAD counts down the trailing silence. |
| **Thinking** | The Claude spark **spins** (continuous rotation) with rays trailing, plus 3 orbiting dots. Label "Denke nach…". This is the signature moment — make it smooth (sprite rotation, 30+ fps). |
| **Speaking** | Spark pulses **in sync with the output audio** amplitude (Orange). Optionally show the reply text scrolling beneath. Label "…". |
| **Reply/transcript** | Show recognized user text (Gray, small) and Claude's reply (Dark) — auto‑scroll if long. |
| **Error** | Friendly: no Wi‑Fi → "Kein WLAN — in Settings verbinden". API error → short message + retry hint. Never a raw stack trace. |

Transitions should crossfade or slide, not hard‑cut. Aim for a toy‑like,
delightful feel — this is the "wow" of the device.

---

## 8. Latency — target and budget

User goal: **as fast as possible.** Realistic end‑to‑end, measured from "user
stops speaking" to "first audio out", with the no‑server (device‑direct) design:

| Stage | Naive | Optimized |
|---|---|---|
| Upload audio (WiFi, ~3 s clip @16 kHz) | 0.5–1.5 s | 0.3–0.8 s |
| STT (Whisper / 4o‑mini‑transcribe) | 1–2 s | 0.6–1.2 s |
| Claude Sonnet — time to first token | 0.6–1.2 s | 0.4–0.9 s |
| TTS — time to first audio chunk | 1–2 s | 0.4–1.0 s |
| **First sound out (total)** | **~4–7 s** | **~2–4 s** |

Optimizations to implement (this is how we hit the low end):
1. **Stream everything.** Stream Claude's tokens; as soon as the **first
   sentence** completes, fire it to TTS; stream TTS audio and start **playing
   while the rest is still generating**. This overlaps stages 3+4+5 instead of
   summing them. Biggest single win.
2. **Fast STT model** (`gpt-4o-mini-transcribe`) and short, 16 kHz mono PCM.
3. **Keep the WiFi/TLS sessions warm** (reuse connections; pre‑resolve DNS).
4. **WAV/PCM TTS** → no decode step on device.
5. Short `max_tokens` (the system prompt keeps answers brief anyway → less to
   generate and speak).
6. Optional later: a serverless proxy (Cloudflare Worker) doing the 3 calls
   server‑side would cut device‑side round trips and is faster + safer for keys
   — out of scope for v1 (user chose device‑direct) but note it in README as the
   upgrade path.

Set a hard timeout per stage with a graceful "Hm, das hat zu lange gedauert —
nochmal?" fallback.

---

## 9. Milestones — status

| # | Milestone | Status |
|---|---|---|
| M0 | Skeleton — boot, status bar, Claude-styled launcher, Settings shell | ✅ shipped |
| M1 | Settings — Wi-Fi scan/connect/persist, BT toggle, volume | ✅ shipped |
| M2 | Audio I/O — mic record + speaker playback through ES8311 | ✅ shipped |
| M3 | Pipeline — record → Whisper → Claude → TTS → play | ✅ shipped, end-to-end |
| M4 | Claude design + animations — spark across all states, transcript view | ✅ shipped |
| M5 | VAD (auto-endpointing) — default mode | ✅ shipped, 2.5 s silence |
| M6 | Latency pass — flash-buffered TTS, 30 s symmetric caps | ✅ shipped, see §12 |
| M7 | Wake word "Claude" via ESP-SR | ⏳ open, needs custom WakeNet |
| M8 | Polish + publish — README, demo GIF, M5Burner package | 🚧 README + GIF done |

---

## 10. Fallback (only if M2 fails in C++)

Unlikely, but documented: split responsibilities — keep the MicroPython buddy
bundle for menu/Buddy/Snake, and ship Claude Pocket as a dedicated C++ firmware
flashed separately. Worse UX (two firmwares), so avoid unless forced.

---

## 11. Repo layout (target)

```
claude-pocket/
├─ README.md
├─ CLAUDE.md                 # orientation for Claude Code
├─ LICENSE
├─ .gitignore
├─ docs/
│  └─ SPEC.md                # this file
└─ firmware/
   ├─ config.example.h       # copy → config.h, fill keys (config.h is ignored)
   ├─ platformio.ini         # or Arduino sketch structure
   └─ src/                    # menu/, settings/, pocket/, ui/, net/, audio/
```

Keep modules small and named by responsibility (`ui/spark.cpp`,
`audio/i2s.cpp`, `net/openai.cpp`, `net/anthropic.cpp`, `pocket/vad.cpp`, …).

---

## 12. Implementation notes

The non-obvious decisions, in order of when they bit:

### 12.1 mbedTLS on arduino-esp32 2.x truncates Cloudflare bodies

The naive design streamed TTS audio straight off `WiFiClientSecure` into the
speaker. Worked for the first ~172 bytes and then `mbedtls_ssl_read` returned
0 for the rest of the body. After a lot of measurement (heartbeat logs at
the read loop, no progress for minutes, socket not closed), the cause came
back to **TLS-1.3 post-handshake records** that arduino-esp32 2.x's
mbedTLS 2.x doesn't parse cleanly — every gap between body records looks
like EOF to the loop. Trying to upgrade to arduino-esp32 3.x on the
no-PSRAM StampS3A panicked in `psramInit` at boot, and `custom_sdkconfig`
refuses any project path with whitespace ("Claude Pocket/"), so we stayed
on 2.x and worked around it instead.

**The workaround:** read the body into a LittleFS file with a byte-counted
loop that treats `available() == 0` as transient, not as EOF. The
loop keeps reading until either `Content-Length` is met or the socket is
genuinely closed with no buffered data left. The file is then handed to
the speaker as a separate stage, so network jitter can never underrun
playback.

### 12.2 No PSRAM = a 320 KB SRAM tetris game

After Wi-Fi, mbedTLS, M5GFX and FreeRTOS task stacks claim their share,
the heap left is ~90 KB. mbedTLS by itself wants ~50 KB during a fresh
TLS handshake and another big chunk for the send window. Anything we kept
in `.bss` competed directly with that heap budget.

The mic was the worst offender. The original design kept the full
recording in a 128 KB `g_shared_buf` so we could re-stream it during the
Whisper multipart upload. Moving that recording to LittleFS dropped `.bss`
from 65 KB to under 20 KB, gave mbedTLS ~100 KB more heap, and silently
unblocked the 5 s recording cap. The same buffer is reused as the audio
playback slab pool during TTS (mic and speaker never run together on the
ES8311), so the savings come for free.

### 12.3 `M5.Speaker.playRaw` doesn't copy

`playRaw(int16_t* data, size_t len, …)` stores the pointer, not a copy.
Reusing one feeder buffer produced an audible doubling effect because the
next slab overwrote bytes the codec was still streaming. The fix is to
slice the playback pool into `N` rotating slabs (currently 16 × 4 KB
inside the shared mic buffer) so the speaker can drain old slabs while we
prepare new ones. `channel=0` keeps every slab on the same mixer slot
(channel = -1 mixed hundreds of overlapping clips at once, also audible).

### 12.4 Speaker drain has to ignore brief quiet windows

`isPlaying(0)` flickers to 0 between consecutive slabs as M5.Speaker pops
the current entry and hasn't started the next yet. The drain loop in
`stream_end` requires a continuous 600 ms quiet window before tearing down
the codec — without that, the last 500+ ms of every reply got cut off.

### 12.5 Symmetric 30 s budget end-to-end

To keep the UX consistent everything is sized to fit ~30 seconds of audio:

- recording: hard cap at `16000 * 30` samples
- VAD silence: 2.5 s, generous enough to ride through thinking pauses
- Claude reply: `CLAUDE_MAX_TOKENS = 120` (~30 s of speech) + a system
  prompt that tells Claude about the cap
- TTS download: 30 s of 24 kHz mono int16 PCM = 1.44 MB, which fits the
  default 1.5 MB LittleFS partition. No partition expansion needed.

### 12.6 Tiny `loopTask` stack

The Arduino-esp32 main task runs on an 8 KB stack. A naive
`uint8_t buf[2048]` inside a few-frames-deep callback was enough to trip
the stack-canary watchpoint and reset the device. Every working buffer in
the HTTP / TTS / playback path is now `static uint8_t buf[…]` so it lives
in `.bss` instead.
