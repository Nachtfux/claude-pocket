# How to flash Claude Pocket + Orbit Fighter onto your M5Stack Cardputer

A short copy-paste-ready guide for the Reddit thread.

---

You'll need:

- An M5Stack **Cardputer-Adv** (the version with PDM mic + speaker)
- A USB-C **data** cable (not charge-only — see below if your laptop doesn't see the device)
- The pre-built binary from this release: **[firmware-burner.bin](https://github.com/Nachtfux/claude-pocket/releases)**

That's it. No PlatformIO, no clone, no toolchain.

## Easiest path — M5Burner (GUI, ~2 minutes)

1. Install **[M5Burner](https://docs.m5stack.com/en/uiflow/m5burner/intro)** (free, Mac / Win / Linux).
2. Plug the Cardputer into your laptop via USB-C.
3. Open M5Burner. In the device list, find **Cardputer ADV**.
4. Click **Local** (or **Burn** → **Open File** depending on your version) and select the downloaded `firmware-burner.bin`.
5. Set the flash offset to `0x10000`.
6. Click **Burn**. About 30 seconds later the Cardputer reboots into the launcher.

## Alt path — esptool (if you live in the terminal)

```bash
pip3 install esptool
esptool.py --port /dev/cu.usbmodem101 write_flash 0x10000 firmware-burner.bin
```

(Adjust the port — `ls /dev/cu.usbmodem*` on Mac, `Device Manager` on Windows, `ls /dev/ttyACM*` on Linux.)

## What you get out of the box (no keys needed)

- **Orbit Fighter** — Claude-themed pixel shoot-em-up, 5 biomes, 5 bosses, endless loop
- **Snake** — Easy / Normal / Heavy
- **Weather** — auto-geolocates, 3-day forecast (no API key)
- **Radio** — 10 internet streams
- **Settings** — Wi-Fi, BT, brightness, volume

## What needs your own API keys (for now)

Claude Pocket / Translator / Claude Buddy need:

- `ANTHROPIC_API_KEY` from [console.anthropic.com](https://console.anthropic.com)
- `OPENAI_API_KEY` from [platform.openai.com](https://platform.openai.com)

Right now the only way to add these is by building from source with your own `config.h`:

```bash
git clone https://github.com/Nachtfux/claude-pocket
cd claude-pocket/firmware
cp config.example.h config.h           # then edit config.h with your keys
pio run -e m5cardputer-adv -t upload
```

`config.h` is git-ignored, your keys never leave your machine.

On-device key entry (Settings menu typing + SD-card `/keys.txt`) is in the works — see [`docs/BURNER.md`](BURNER.md) for the roadmap.

## Troubleshooting

**Laptop doesn't see the Cardputer over USB**
→ Try a different USB-C cable. Many "USB-C" cables are charge-only and have no data lines. Use the cable that came with your phone / MacBook charger.

**Cardputer is stuck / won't boot**
→ Hold the **G0** button on the back, briefly press **Reset**, then release G0. The screen goes dark — device is in download mode and esptool / M5Burner can flash it.

**Compiled with M5Burner but the apps don't show**
→ Make sure the flash offset is `0x10000`, not `0x0`. A wrong offset bricks the bootloader; recover via the G0 + Reset sequence above.

---

Source code: [github.com/Nachtfux/claude-pocket](https://github.com/Nachtfux/claude-pocket)

Built on the M5Stack Cardputer ADV that Anthropic handed out at **Code with Claude — London Extended**.
