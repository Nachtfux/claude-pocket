#!/bin/bash
# flash-lineage.sh - install LineageOS 18.1 (rook) from TWRP, step by step.
#
#   1. verify the ROM zip on the Mac (sha256, zip integrity)
#   2. check the Spot is a rook in TWRP
#   3. wipe Dalvik/ART, System, Data, Cache (TWRP CLI or by hand)
#   4. push the zip to /sdcard and verify its sha256 ON the device
#   5. install (TWRP CLI, by hand, or adb sideload) and reboot
#
# Every destructive step asks first. Nothing here writes a block device
# directly; preloader / lk / tee are never touched.
#
# Usage: tools/spot/flash-lineage.sh <lineage-18.1-*-rook.zip> [expected_sha256]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=SCRIPTDIR/lib.sh
. "$HERE/lib.sh"

[ $# -ge 1 ] || die "Usage: $0 <lineage-18.1-*-rook.zip> [expected_sha256]"
ROM="$1"; EXPECTED="$(lower "${2:-}")"
[ -f "$ROM" ] || die "File not found: $ROM"
NAME="$(basename "$ROM")"
REMOTE="/sdcard/$NAME"

twrp_cmd() {
  # Run a TWRP command-line action; guard first, never on block devices.
  guard_write "twrp $*"
  adb shell twrp "$@" </dev/null | tr -d '\r'
}

# ------------------------------------------------------------- 1. local ----
step "1/5 Checking $NAME on the Mac"
case "$NAME" in
  *rook*) ok "file name mentions rook" ;;
  *) warn "file name does not contain 'rook' - is this really the Echo Spot build?"
     confirm_safety "Continue anyway?" || exit 1 ;;
esac
case "$NAME" in
  lineage-18.1-*) ;;
  *) warn "expected lineage-18.1-*.zip (Android 11)" ;;
esac
if [ -n "$EXPECTED" ] && ! [[ "$EXPECTED" =~ ^[0-9a-f]{64}$ ]]; then
  die "expected_sha256 must be 64 hex characters"
fi
info "size: $(( $(file_size "$ROM") / 1024 / 1024 )) MiB - hashing..."
LOCAL_SHA="$(sha256_local "$ROM")"
info "sha256: $LOCAL_SHA"
if [ -n "$EXPECTED" ]; then
  [ "$LOCAL_SHA" = "$EXPECTED" ] || die "sha256 MISMATCH (expected $EXPECTED). Download again."
  ok "sha256 matches the expected value"
else
  warn "No expected sha256 given. Compare by eye with the release page"
  hint "(https://github.com/amazon-oss/releases/releases)."
  confirm_safety "Does the hash above match the release page EXACTLY?" || die "Aborted - get the official hash first."
fi
if have unzip; then
  if unzip -tq "$ROM" >/dev/null 2>&1; then ok "zip integrity ok"; else die "zip is corrupt (unzip -t failed)"; fi
  if unzip -l "$ROM" | grep 'META-INF/com/google/android/update-binary' >/dev/null; then ok "flashable zip (has update-binary)"
  else die "no META-INF/com/google/android/update-binary - not a recovery zip"; fi
fi

# ------------------------------------------------------------ 2. device ----
step "2/5 Checking the Spot"
need_adb; require_single_device
st="$(adb_state)"
[ "$st" = "recovery" ] || die "Spot must be in TWRP (adb get-state = recovery), got '$st'. Hold Volume Up while connecting power."
DEV="$(adb shell getprop ro.product.device </dev/null | tr -d '\r')"
[ "$(lower "$DEV")" = "rook" ] || { warn "ro.product.device='$DEV' (expected rook)"; confirm_safety "Continue anyway?" || exit 1; }
ok "TWRP on $DEV"
HAS_TWRP_CLI=0
if adb shell 'command -v twrp' </dev/null 2>/dev/null | grep twrp >/dev/null; then HAS_TWRP_CLI=1; ok "TWRP command line available"; fi

# A backup counts only if backup.sh finished with every partition verified:
# COMPLETE marker + MANIFEST.txt whose .sha256 matches + core partitions
# listed with their .img present.
backup_ok() {
  local d="$1" p want got img
  [ -f "$d/COMPLETE" ] && [ -f "$d/MANIFEST.txt" ] && [ -f "$d/MANIFEST.txt.sha256" ] || return 1
  want="$(awk '{print $1; exit}' "$d/MANIFEST.txt.sha256")"
  got="$(sha256_local "$d/MANIFEST.txt")"
  [ -n "$want" ] && [ "$want" = "$got" ] || return 1
  for p in boot recovery system lk_real tee1_real tee2_real; do
    img="$(awk -v p="$p" '!/^#/ && tolower($1) == p {print $5; exit}' "$d/MANIFEST.txt")"
    [ -n "$img" ] && [ -f "$d/$img" ] || return 1
  done
  return 0
}
HAVE_BACKUP=0
for d in spot-backup-* "$HOME"/EchoSpot-Backup/spot-backup-*; do
  [ -d "$d" ] || continue
  if backup_ok "$d"; then HAVE_BACKUP=1; ok "verified backup found: $d"; break; fi
  warn "ignoring incomplete/unverified backup: $d"
done
if [ "$HAVE_BACKUP" = 0 ]; then
  warn "No complete, verified spot-backup-* folder in $(pwd) or ~/EchoSpot-Backup."
  confirm_safety "Did you already run tools/spot/backup.sh successfully and keep the result elsewhere?" || die "Run tools/spot/backup.sh first."
fi

# -------------------------------------------------------------- 3. wipe ----
step "3/5 Wipe: Dalvik/ART Cache, System, Data, Cache (nothing else!)"
hint "Internal storage, boot, recovery, lk/tee/preloader are NOT wiped."
WIPED=0
if [ "$HAS_TWRP_CLI" = 1 ] && confirm "Let me run the wipe via the TWRP command line?"; then
  if confirm_hard "This erases System, Data, Cache and Dalvik on the Spot (Fire OS is gone afterwards)."; then
    for p in dalvik system data cache; do
      info "twrp wipe $p"
      out="$(twrp_cmd wipe "$p" || true)"
      printf '%s\n' "$out" | sed 's/^/     /'
      if printf '%s' "$out" | grep -qi 'fail\|error\|unable'; then
        warn "wipe $p reported a problem."
        if [ "$p" = system ]; then
          hint "Usually 'Keep Read Only': on the Spot tap Mount, untick 'Mount system partition read-only', retry."
        fi
        pause "Fix it on the Spot (or wipe $p there via Wipe > Advanced Wipe), then press Enter..."
      fi
    done
    WIPED=1
  fi
fi
if [ "$WIPED" = 0 ]; then
  hint "On the Spot: Wipe > Advanced Wipe > tick ONLY Dalvik / ART Cache, System, Data, Cache > swipe."
  hint "If System fails: Mount > untick 'Mount system partition read-only' and repeat."
  pause "Press Enter when the wipe succeeded..."
fi

# ------------------------------------------------------- 4. push+verify ----
step "4/5 Push + verify on the device"
METHOD=""
if confirm "Copy the zip to $REMOTE and install from there? (No = adb sideload instead)"; then
  METHOD=push
else
  METHOD=sideload
fi

if [ "$METHOD" = push ]; then
  adb push "$ROM" "$REMOTE" </dev/null
  REMOTE_SHA="$(sha256_remote "$REMOTE")"
  info "device sha256: ${REMOTE_SHA:-<none>}"
  [ "$REMOTE_SHA" = "$LOCAL_SHA" ] || die "sha256 on the device does NOT match - do not install. Re-push."
  ok "device copy verified"
fi

# ----------------------------------------------------------- 5. install ----
step "5/5 Install"
if [ "$METHOD" = push ]; then
  if [ "$HAS_TWRP_CLI" = 1 ] && confirm "Install via the TWRP command line (twrp install $REMOTE)?"; then
    confirm_hard "Install $NAME now. Do NOT unplug until it finishes." || die "Aborted before install."
    out="$(twrp_cmd install "$REMOTE" || true)"
    printf '%s\n' "$out" | sed 's/^/     /'
    if printf '%s' "$out" | grep -qi 'fail\|error'; then
      die "Install reported an error - check the TWRP screen. Do NOT reboot yet."
    fi
  else
    hint "On the Spot: Install > select $NAME (in /sdcard) > swipe to confirm."
    pause "Press Enter when TWRP says 'Successful'..."
  fi
else
  hint "On the Spot: Advanced > ADB Sideload > swipe to start."
  pause "Press Enter once the Spot shows 'Starting ADB sideload feature'..."
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ "$(adb_state)" = sideload ] && break
    sleep 1
  done
  [ "$(adb_state)" = sideload ] || die "Spot is not in sideload mode."
  confirm_hard "Sideload $NAME now. Do NOT unplug until it finishes." || die "Aborted before install."
  adb sideload "$ROM" </dev/null || warn "adb sideload exited non-zero ('Total xfer: 1.00x' is still a success) - check the TWRP screen."
  pause "Press Enter when TWRP shows the install finished..."
fi
ok "LineageOS installed."

if confirm "Reboot into LineageOS now?"; then
  hint "If TWRP offers to install the 'TWRP app': decline."
  adb reboot </dev/null || true
  ok "Rebooting. First boot takes a few minutes (Lineage animation -> launcher, no setup wizard)."
else
  hint "Reboot manually: TWRP > Reboot > System (decline the TWRP app)."
fi
hint "Then: enable USB debugging (Settings > About > tap Build number 7x > Developer options)"
hint "and run tools/spot/post-install.sh"
