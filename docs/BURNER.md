# Burner Build

This is the public, key-less binary of the Claude Pocket firmware — the one
that ships to M5Burner / GitHub Releases for people who do not want to set
up PlatformIO and edit `config.h` themselves.

The Burner build is identical to the dev build with one exception: the
ANTHROPIC and OPENAI API-key macros are forced to empty strings at compile
time, so the published `.bin` file contains no secrets.

## Flashing

The pre-built binary lives at:

```
.pio/build/m5cardputer-adv-burner/firmware.bin
```

(or, post-release, attached to the GitHub release page).

### Option A — M5Burner (GUI, easiest)

1. Install [M5Burner](https://docs.m5stack.com/en/uiflow/m5burner/intro).
2. Connect the Cardputer-Adv via USB-C.
3. In M5Burner, click **Local / Browse** and pick `firmware.bin`.
4. Flash offset: **0x10000**.
5. Click **Burn**. Cardputer reboots into the launcher.

### Option B — esptool (terminal)

```bash
pip3 install esptool
esptool.py --port /dev/cu.usbmodem101 write_flash 0x10000 firmware-burner.bin
```

Replace the port with whatever `ls /dev/cu.usbmodem*` reports on your machine.

## What works without configuration

Out of the box, no extra setup needed:

- **Orbit Fighter** — the pixel shoot-em-up
- **Snake** — Easy / Normal / Heavy
- **Weather** — auto-geolocates via IP (Open-Meteo, no API key needed)
- **Radio** — 10 preset internet streams (Wi-Fi required, set in Settings)
- **Settings** — Wi-Fi scanner, Bluetooth toggle, volume, brightness

## What needs API keys

The voice apps depend on the Anthropic + OpenAI APIs:

- **Claude Pocket** — voice assistant
- **Claude Buddy** — BLE companion info
- **Translator** — voice-to-voice translation

You need:

- `ANTHROPIC_API_KEY` (from [console.anthropic.com](https://console.anthropic.com/))
- `OPENAI_API_KEY`    (from [platform.openai.com](https://platform.openai.com/))

### Method 1 — SD card `/keys.txt` (planned; currently disabled)

> **Status:** scaffolded in `src/keys.cpp` (`import_from_sd()`), but the
> `app::init()` hook is commented out pending verification of the
> Cardputer-Adv SD-card GPIO mapping. Re-enabled in a near-future release.

When enabled, drop a `keys.txt` file onto the microSD card at the root:

```
# /keys.txt
ANTHROPIC_API_KEY=sk-ant-...
OPENAI_API_KEY=sk-proj-...

# Optional — pre-populate Wi-Fi too
WIFI_SSID=MyNetwork
WIFI_PASS=mypassword
```

Insert the card, power-cycle. The device imports into NVS on boot. The
card can stay inserted (idempotent) or be removed afterwards.

### Method 2 — Build from source with your own keys (works today)

If you want voice features _now_, build locally:

```bash
git clone https://github.com/Nachtfux/claude-pocket.git
cd claude-pocket/firmware
cp config.example.h config.h
# edit config.h, paste your keys
pio run -e m5cardputer-adv -t upload
```

`config.h` is git-ignored so your keys never leave your machine.

### Method 3 — Settings-screen key entry (planned)

> **Status:** not implemented yet (Phase B). A future Settings screen will
> let you type keys via the on-device keyboard and save to NVS.

## Producing the Burner binary yourself

If you want to build the public binary from source:

```bash
cd firmware
pio run -e m5cardputer-adv-burner
```

Verify no keys leaked into the binary:

```bash
strings .pio/build/m5cardputer-adv-burner/firmware.bin | grep -E 'sk-(ant|proj)' && \
  echo "FOUND KEYS — DO NOT PUBLISH" || \
  echo "clean — safe to publish"
```

The `BURNER_BUILD=1` build flag is set automatically by the
`m5cardputer-adv-burner` environment in `platformio.ini`. It selects the
empty-string fallbacks in `src/keys.cpp` instead of including `config.h`.
