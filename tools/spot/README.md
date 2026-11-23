# Echo Spot (rook) helper scripts

Mac-side helpers for turning an **Amazon Echo Spot 2017** into a Claude
speaker (LineageOS 18.1 + official Claude app + Hey Claude). The full guide,
in German, is [`docs/ECHO_SPOT.md`](../../docs/ECHO_SPOT.md) - read it first.

All scripts run on the macOS stock `/bin/bash` 3.2 (and on Linux), print
coloured status lines, ask before every destructive step and **never write
to a block device**. `lib.sh` additionally refuses any write that names
`preloader`, `lk`, `lk_real`, `tee*` or `mmcblk0boot*`.

| # | Script | Device state | What it does |
|---|--------|--------------|--------------|
| 0 | `preflight.sh` | any | Checks brew, adb/fastboot, python3 + pyserial, libusb, bash >= 4, GNU timeout; lists USB devices (MediaTek `0x0e8d`, Amazon `0x1949`) and adb/fastboot devices; tells you the next step. |
| 1 | `amonet-macos.sh <amonet-dir> [fastbrick\|boot-recovery\|boot-fastboot]` | fastboot | **Untested on Mac.** Runs amonet's `fastbrick.sh` with Homebrew bash, `gtimeout` as `timeout` and the Homebrew `fastboot` instead of the bundled Linux binary. Fallback: Windows PC (`fastbrick.bat`) or an Ubuntu live USB. |
| 2 | `backup.sh [--dry-run] [--with-userdata] [outdir]` | TWRP | Pulls every partition from `by-name` plus `mmcblk0boot0/1` (not `userdata`/`cache` by default), checks size and sha256 (device vs. Mac); aborts if core partitions (boot, recovery, system, lk_real, tee1_real, tee2_real) are missing. `MANIFEST.txt`, `MANIFEST.txt.sha256` and `COMPLETE` are only written when every partition verified (otherwise `MANIFEST.txt.partial`). `--dry-run` works without a device (example map). |
| 3 | `flash-lineage.sh <rom.zip> [sha256]` | TWRP | Verifies the zip, requires a complete verified backup (`COMPLETE` + matching `MANIFEST.txt.sha256`) or an explicit answer, guides the wipe (Dalvik/System/Data/Cache), pushes + verifies on the device, installs (TWRP CLI, by hand or `adb sideload`), reboots. |
| 4 | `inspect-claude-apk.sh [apk]` | LineageOS (or offline) | Pulls the Claude APK (incl. splits) and lists activities, intent filters, shortcuts and voice-related strings via `inspect_apk.py` (androguard venv in `~/.cache/heyclaude-venv`). Ends with a CONCLUSION line. |
| 5 | `post-install.sh [claude.apk] [heyclaude.apk] [aurora.apk]` | LineageOS + USB debugging | Installs APKs (`-` skips a slot; split bundles ok), grants Hey Claude mic + overlay, enables its AccessibilityService (appends), battery whitelist, disables lock screen, sets screen timeout, starts the app. |

Typical order:

```bash
tools/spot/preflight.sh
tools/spot/amonet-macos.sh ~/Downloads/amonet-rook-v2.0.0     # or Windows: fastbrick.bat
tools/spot/backup.sh
tools/spot/flash-lineage.sh ~/Downloads/lineage-18.1-*-rook.zip <sha256-from-release>
tools/spot/post-install.sh ~/Downloads/claude.apk android/hey-claude/app/build/outputs/apk/debug/app-debug.apk
tools/spot/inspect-claude-apk.sh
adb logcat -s HeyClaude
```

Environment knobs: `ASSUME_YES=1` (skip convenience y/N prompts; safety
questions - ROM hash, backup present, wrong device/file name - and the
"type YES" prompts are always asked), `NO_COLOR=1`, `SCREEN_TIMEOUT_MS`, `CLAUDE_PKG`, `HEYCLAUDE_VENV`.

Backups contain device-unique data - keep them private and out of git.
