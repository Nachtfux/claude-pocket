#!/bin/bash
# amonet-macos.sh - run the amonet-rook unlock from macOS (UNTESTED on Mac).
#
# The amonet-rook release zip is made for Windows (fastbrick.bat -> .ps1) and
# Linux (fastbrick.sh). fastbrick.sh does NOT run on a stock Mac because:
#   1. it uses `declare -A` (bash >= 4; macOS ships bash 3.2)  -> brew install bash
#   2. it calls GNU `timeout 8 ...` and treats exit code 124 as success
#                                                              -> brew install coreutils
#   3. it runs the bundled bin/fastboot, a Linux binary        -> use brew's fastboot
# This wrapper fixes all three without editing amonet's scripts:
#   - bin/fastboot(32) are moved to *.orig and replaced by symlinks to the
#     Homebrew fastboot,
#   - a temp dir with `timeout -> gtimeout` is put first in PATH,
#   - fastbrick.sh is started with Homebrew bash from inside the amonet folder.
#
# fastbrick.sh itself only talks fastboot (`fastboot flash brick fastbrick.img`);
# the payload does the rest on the device and boots into TWRP.
# boot-recovery.sh / boot-fastboot.sh talk to the MediaTek preloader over a
# serial port (/dev/cu.usbmodem*) with python3 + pyserial - also supported here.
# bootrom-step.sh (modules/main.py) reads /proc and does NOT work on macOS:
# use a Linux live USB for that path.
#
# Usage:
#   tools/spot/amonet-macos.sh <extracted-amonet-dir> [fastbrick|boot-recovery|boot-fastboot]
#   (the dir is the one that contains fastbrick.sh, e.g. ~/Downloads/amonet-rook-v2.0.0/amonet)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
. "$HERE/lib.sh"

[ $# -ge 1 ] || die "Usage: $0 <extracted-amonet-dir> [fastbrick|boot-recovery|boot-fastboot]"
DIR="$1"; MODE="${2:-fastbrick}"

# Accept the zip root as well as the inner amonet/ folder.
if [ ! -f "$DIR/fastbrick.sh" ] && [ -f "$DIR/amonet/fastbrick.sh" ]; then DIR="$DIR/amonet"; fi
[ -f "$DIR/fastbrick.sh" ] || die "fastbrick.sh not found in $DIR - point me at the extracted amonet folder."
DIR="$(cd "$DIR" && pwd)"

step "Checking the amonet package in $DIR"
for f in profile.sh boot-recovery.sh boot-fastboot.sh modules/handshake2.py; do
  if [ -e "$DIR/$f" ]; then ok "$f"; else warn "$f missing"; fi
done
# shellcheck disable=SC2012
FB_IMG="$(ls "$DIR"/bin/fastbrick*.img 2>/dev/null | head -n1 || true)"
[ -n "$FB_IMG" ] || die "No bin/fastbrick*.img in the package - incomplete download?"
ok "payload: $(basename "$FB_IMG") ($(file_size "$FB_IMG") bytes, sha256 $(sha256_local "$FB_IMG"))"

case "$MODE" in
  fastbrick) ;;
  boot-recovery|boot-fastboot)
    have python3 || die "python3 missing (brew install python)"
    python3 -c 'import serial' 2>/dev/null || die "pyserial missing: python3 -m pip install --user pyserial"
    step "$MODE: talks to the preloader over USB serial"
    hint "1. Unplug power. 2. Start this. 3. Connect USB + power - the script waits for a new MediaTek port."
    confirm "Run $MODE.sh now?" || exit 0
    cd "$DIR"
    exec /bin/bash "./$MODE.sh" ;;
  bootrom-step|brick) die "$MODE needs Linux (modules/main.py reads /proc). Use an Ubuntu live USB." ;;
  *) die "Unknown mode '$MODE'" ;;
esac

step "Host requirements"
BASH4=""
for b in /opt/homebrew/bin/bash /usr/local/bin/bash "$(command -v bash)"; do
  if [ -x "$b" ] && "$b" -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]' 2>/dev/null; then BASH4="$b"; break; fi
done
[ -n "$BASH4" ] || die "Need bash >= 4: brew install bash"
ok "bash: $BASH4 ($("$BASH4" -c 'echo $BASH_VERSION'))"

HOST_FB="$(command -v fastboot || true)"
[ -n "$HOST_FB" ] || die "fastboot missing: brew install android-platform-tools"
ok "fastboot: $HOST_FB"

SHIM="$(mktemp -d "${TMPDIR:-/tmp}/amonet-shim.XXXXXX")"
trap 'rm -rf "$SHIM"' EXIT
if have gtimeout; then
  ln -s "$(command -v gtimeout)" "$SHIM/timeout"; ok "timeout -> gtimeout"
elif have timeout && timeout 1 true 2>/dev/null; then
  ok "timeout: $(command -v timeout)"
else
  die "GNU timeout missing: brew install coreutils"
fi

step "Swapping the bundled Linux fastboot for the Homebrew one"
for f in fastboot fastboot32; do
  p="$DIR/bin/$f"
  if [ -L "$p" ]; then ok "bin/$f already a symlink -> $(readlink "$p")"; continue; fi
  if [ -e "$p" ]; then
    info "bin/$f: $(file -b "$p" | cut -c1-60)"
    mv "$p" "$p.orig"
  fi
  ln -s "$HOST_FB" "$p"
  ok "bin/$f -> $HOST_FB (original kept as bin/$f.orig)"
done

step "Device"
FB_OUT="$(fastboot devices 2>/dev/null || true)"
if [ -z "$FB_OUT" ]; then
  warn "No fastboot device yet. Power off the Spot, hold ALL THREE top buttons"
  hint "(Volume Down + Volume Up + Mute) and connect power until '=> FASTBOOT mode...' shows."
  hint "fastbrick.sh will wait for it."
else
  ok "fastboot: $FB_OUT"
fi

echo
warn "macOS is NOT the tested path (Windows fastbrick.bat is). If anything looks"
warn "wrong, stop now and use a Windows PC or an Ubuntu live USB instead."
warn "Once the payload is sent: do NOT unplug, do NOT cut power (up to ~5 min)."
confirm_hard "Start amonet fastbrick.sh (it will ask for YES a second time)?" || { info "Aborted."; exit 0; }

cd "$DIR"
PATH="$SHIM:$PATH" "$BASH4" ./fastbrick.sh
