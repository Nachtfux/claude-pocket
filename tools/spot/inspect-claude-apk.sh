#!/bin/bash
# inspect-claude-apk.sh - is there an official way straight into voice mode?
#
# Without an argument the Claude APK (incl. split APKs) is pulled from the
# connected device. Then tools/spot/inspect_apk.py (androguard, in a venv
# under ~/.cache/heyclaude-venv) prints activities, intent filters, app
# shortcuts and voice-related strings, ending in a CONCLUSION line.
#
# Usage: tools/spot/inspect-claude-apk.sh [claude.apk | dir-with-splits]
# Env:   CLAUDE_PKG (default: autodetect, falls back to com.anthropic.claude)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=SCRIPTDIR/lib.sh
. "$HERE/lib.sh"

VENV="${HEYCLAUDE_VENV:-$HOME/.cache/heyclaude-venv}"
ANDROGUARD_SPEC="${ANDROGUARD_SPEC:-androguard>=4.1,<5}"

have python3 || die "python3 missing (brew install python)"

# ------------------------------------------------------------- venv --------
if [ ! -x "$VENV/bin/python" ] || ! "$VENV/bin/python" -c 'import androguard' 2>/dev/null; then
  step "Setting up androguard in $VENV"
  confirm "Create a Python venv there and pip install '$ANDROGUARD_SPEC'?" || die "Aborted."
  python3 -m venv "$VENV"
  "$VENV/bin/python" -m pip install -q --upgrade pip
  "$VENV/bin/python" -m pip install -q "$ANDROGUARD_SPEC"
fi
ok "androguard $("$VENV/bin/python" -c 'import androguard; print(getattr(androguard, "__version__", "?"))')"

# --------------------------------------------------------------- APKs ------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/claude-apk.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
APKS=()

if [ $# -ge 1 ]; then
  if [ -d "$1" ]; then
    for f in "$1"/*.apk; do [ -f "$f" ] && APKS+=("$f"); done
  elif [ -f "$1" ]; then
    case "$(lower "$1")" in
      *.apks|*.xapk|*.apkm)
        unzip -q -o "$1" '*.apk' -d "$WORK" || die "could not unpack bundle $1"
        for f in "$WORK"/*.apk "$WORK"/*/*.apk; do [ -f "$f" ] && APKS+=("$f"); done ;;
      *) APKS+=("$1") ;;
    esac
  else
    die "not found: $1"
  fi
else
  need_adb; require_single_device
  [ "$(adb_state)" = device ] || die "No booted device with USB debugging - or pass an APK path."
  PKG="${CLAUDE_PKG:-}"
  if [ -z "$PKG" ]; then
    PKG="$(adb shell pm list packages </dev/null | tr -d '\r' | sed 's/^package://' \
           | grep -i claude | grep -v '^ai\.pocket\.heyclaude$' | head -n1 || true)"
  fi
  [ -n "$PKG" ] || PKG=com.anthropic.claude
  step "Pulling $PKG from the device"
  PATHS="$(adb shell pm path "$PKG" </dev/null | tr -d '\r' | sed 's/^package://')"
  [ -n "$PATHS" ] || die "$PKG is not installed on the device."
  while read -r p; do
    [ -n "$p" ] || continue
    out="$WORK/$(basename "$p")"
    adb pull "$p" "$out" </dev/null >/dev/null
    ok "$(basename "$p") ($(( $(file_size "$out") / 1024 )) KiB)"
    APKS+=("$out")
  done <<EOF
$PATHS
EOF
fi
[ "${#APKS[@]}" -gt 0 ] || die "no APK files found"

step "Inspecting ${#APKS[@]} APK file(s)"
# -I: isolated mode, so nothing next to the APKs can be imported
"$VENV/bin/python" -I "$HERE/inspect_apk.py" "${APKS[@]}"
