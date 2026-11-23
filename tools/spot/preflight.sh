#!/bin/bash
# shellcheck disable=SC2016  # single-quoted $vars are meant for the child shell
# preflight.sh - check the Mac for everything the Echo Spot (rook) flow needs
# and tell you what to do next based on what is plugged in.
#
# Read-only: installs nothing, writes nothing to the device.
#
# Usage: tools/spot/preflight.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=SCRIPTDIR/lib.sh
. "$HERE/lib.sh"

MISSING=0
miss() { err "$1"; hint "$2"; MISSING=$((MISSING + 1)); }

step "Host"
info "OS: $(uname -s) $(uname -r) ($(uname -m)), shell: $BASH_VERSION"
if [ "$(uname -s)" != "Darwin" ]; then
  warn "This helper is written for macOS. On Linux most checks still work; system_profiler is skipped."
fi

step "Tools"
if have brew; then ok "Homebrew: $(brew --prefix)"; else
  miss "Homebrew not found" '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
fi

for t in adb fastboot; do
  if have "$t"; then ok "$t: $(command -v "$t") ($("$t" --version 2>/dev/null | head -n1))"
  else miss "$t not found" "brew install android-platform-tools"; fi
done

if have python3; then
  ok "python3: $(python3 --version 2>&1)"
  if python3 -c 'import serial, serial.tools.list_ports' >/dev/null 2>&1; then
    ok "pyserial: $(python3 -c 'import serial; print(serial.__version__)')"
  else
    miss "pyserial missing (needed by amonet's boot-recovery.sh / boot-fastboot.sh)" \
         "python3 -m pip install --user pyserial"
  fi
else
  miss "python3 not found" "brew install python"
fi

LIBUSB_OK=0
if have brew && brew list --versions libusb >/dev/null 2>&1; then LIBUSB_OK=1; fi
if [ "$LIBUSB_OK" = 0 ] && have pkg-config && pkg-config --exists libusb-1.0 2>/dev/null; then LIBUSB_OK=1; fi
if [ "$LIBUSB_OK" = 1 ]; then ok "libusb present"; else
  warn "libusb not found (optional: only for MTKClient / brick rescue)"; hint "brew install libusb"
fi

# amonet's fastbrick.sh uses `declare -A` (bash >= 4) and GNU `timeout`.
BASH4=""
for b in /opt/homebrew/bin/bash /usr/local/bin/bash; do
  if [ -x "$b" ] && "$b" -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]' 2>/dev/null; then BASH4="$b"; break; fi
done
if [ -n "$BASH4" ]; then ok "bash >= 4 for amonet: $BASH4"; else
  miss "no bash >= 4 (amonet fastbrick.sh needs it; macOS ships 3.2)" "brew install bash"
fi
if have timeout || have gtimeout; then ok "timeout: $(command -v timeout || command -v gtimeout)"; else
  miss "GNU timeout missing (amonet fastbrick.sh needs it)" "brew install coreutils"
fi
if have scrcpy; then ok "scrcpy (optional, mirror the screen)"; else
  hint "optional: brew install scrcpy  (mirror the Spot's screen on the Mac)"
fi

step "USB devices"
USB_SEEN=""
if have system_profiler; then
  USB_DUMP="$(system_profiler SPUSBDataType 2>/dev/null || true)"
  # Print product + vendor lines, highlight MediaTek (0x0e8d) and Amazon (0x1949)
  printf '%s\n' "$USB_DUMP" | awk -v r="$C_RED" -v g="$C_GRN" -v z="$C_RST" '
    /^ {8}[^ ].*:$/ { name=$0; sub(/^ +/, "", name); sub(/:$/, "", name) }
    /Product ID:/ { pid=$3 }
    /Vendor ID:/  {
      vid=tolower($3)
      tag=""
      if (vid == "0x0e8d") tag=g "  <-- MediaTek (preloader/bootrom)" z
      if (vid == "0x1949") tag=g "  <-- Amazon (fastboot/adb)" z
      printf "     %-40s vid=%s pid=%s%s\n", name, vid, pid, tag
    }'
  if printf '%s' "$USB_DUMP" | grep -qi '0x0e8d'; then USB_SEEN="mtk"; fi
  if printf '%s' "$USB_DUMP" | grep -qi '0x1949'; then USB_SEEN="${USB_SEEN} amazon"; fi
elif have lsusb; then
  lsusb | sed 's/^/     /'
  if lsusb | grep -i '0e8d:' >/dev/null; then USB_SEEN="mtk"; fi
  if lsusb | grep -i '1949:' >/dev/null; then USB_SEEN="${USB_SEEN} amazon"; fi
fi
# shellcheck disable=SC2012
SERIAL_PORTS="$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | tr '\n' ' ' || true)"
if [ -n "$SERIAL_PORTS" ]; then info "Serial ports: $SERIAL_PORTS"; fi

step "adb / fastboot"
ADB_STATE=none; FB_LIST=""
if have adb; then
  adb devices -l 2>/dev/null | sed 's/^/     /'
  ADB_STATE="$(adb_state)"
fi
if have fastboot; then
  FB_LIST="$(fastboot devices 2>/dev/null || true)"
  if [ -n "$FB_LIST" ]; then printf '%s\n' "$FB_LIST" | sed 's/^/     fastboot: /'; else hint "fastboot: no device"; fi
fi

step "Next step"
if [ -n "$FB_LIST" ]; then
  ok "Spot is in FASTBOOT."
  PRODUCT="$(fastboot getvar product 2>&1 | awk '/product:/ {print $2; exit}' || true)"
  UNLOCK="$(fastboot getvar unlock_status 2>&1 | awk '/unlock_status:/ {print $2; exit}' || true)"
  LKDESC="$(fastboot getvar lk_build_desc 2>&1 | awk -F': ' '/lk_build_desc:/ {print $2; exit}' || true)"
  hint "product=${PRODUCT:-?} unlock_status=${UNLOCK:-?} lk_build_desc=${LKDESC:-?}"
  case "$LKDESC" in
    c1c79aa-20220824_171625*) hint "lk build matches Fire OS 5.5.6.9 (supported)";;
    beb022a-20211202_232416*) hint "lk build matches Fire OS 5.5.5.2 (supported)";;
    d9a2246-20180702_202914*) hint "lk build matches Fire OS 5.5.3.4 (supported)";;
    "") ;;
    *) warn "unknown lk build - check the Fire OS version before unlocking (docs/ECHO_SPOT.md §1)";;
  esac
  if [ "$(lower "${UNLOCK:-}")" = "true" ]; then
    hint "Already unlocked. Boot TWRP: hold Volume Up while connecting power, then run tools/spot/backup.sh"
  else
    hint "Locked: run the amonet unlock (docs/ECHO_SPOT.md, section 'Entsperren'),"
    hint "e.g. tools/spot/amonet-macos.sh ~/Downloads/amonet-rook-v2.0.0"
  fi
else
  case "$ADB_STATE" in
    recovery)
      ok "Spot is in RECOVERY (TWRP)."
      hint "1) tools/spot/backup.sh            (once, before anything else)"
      hint "2) tools/spot/flash-lineage.sh lineage-18.1-*-rook.zip <sha256>" ;;
    sideload)
      ok "Spot is in adb SIDELOAD mode."
      hint "adb sideload lineage-18.1-*-rook.zip   (or run tools/spot/flash-lineage.sh)" ;;
    device)
      PRODUCT="$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r' || true)"
      LOS="$(adb shell getprop ro.lineage.version 2>/dev/null | tr -d '\r' || true)"
      FIRE="$(adb shell getprop ro.build.version.name 2>/dev/null | tr -d '\r' || true)"
      ok "Spot is booted with USB debugging (device=${PRODUCT:-?})."
      if [ -n "$LOS" ]; then
        hint "LineageOS $LOS detected. Next: tools/spot/post-install.sh [claude.apk] [heyclaude.apk] [aurora.apk]"
        hint "Optional: tools/spot/inspect-claude-apk.sh"
      else
        hint "Not LineageOS (${FIRE:-stock?}). Power off, then enter fastboot (all three top buttons) to unlock."
      fi ;;
    unauthorized)
      warn "adb device is UNAUTHORIZED: confirm the RSA prompt on the Spot screen, then re-run." ;;
    *)
      if printf '%s' "$USB_SEEN" | grep -q mtk; then
        ok "MediaTek USB device visible (preloader/bootrom mode)."
        hint "Only needed for amonet's boot-recovery.sh / boot-fastboot.sh or a brick rescue."
      else
        warn "No Spot detected."
        hint "Fastboot (hacked/stock): hold Volume Down while connecting power (unlocked) or all three top buttons (stock)."
        hint "TWRP: hold Volume Up while connecting power."
        hint "Use a DATA micro-USB cable (many cheap ones are charge-only)."
      fi ;;
  esac
fi

echo
if [ "$MISSING" -gt 0 ]; then
  warn "$MISSING item(s) missing - see hints above."
  exit 1
fi
ok "Host looks ready."
