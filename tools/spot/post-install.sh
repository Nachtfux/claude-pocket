#!/bin/bash
# post-install.sh - set up the Spot after LineageOS booted (USB debugging on).
#
#   - installs the given APKs (single .apk, a folder of split APKs, or an
#     .apks/.apkm/.xapk bundle); pass "-" to skip a slot
#   - finds the Claude package
#   - grants Hey Claude: RECORD_AUDIO, SYSTEM_ALERT_WINDOW (appops),
#     enables its AccessibilityService (appends, never clobbers),
#     battery-optimisation whitelist
#   - disables the lock screen, sets the screen timeout
#   - starts Hey Claude
#
# Usage: tools/spot/post-install.sh [claude.apk|-] [heyclaude.apk|-] [aurora.apk|-]
# Env:   SCREEN_TIMEOUT_MS (default 120000), ASSUME_YES=1 (skip y/N prompts)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=SCRIPTDIR/lib.sh
. "$HERE/lib.sh"

PKG=ai.pocket.heyclaude
A11Y="$PKG/.launch.VoiceModeA11yService"
A11Y_LONG="$PKG/$PKG.launch.VoiceModeA11yService"
SCREEN_TIMEOUT_MS="${SCREEN_TIMEOUT_MS:-120000}"

sh_dev() { adb shell "$@" </dev/null | tr -d '\r'; }
# settings / pm / appops changes: guard (paranoia) then run
sh_set() { guard_write "$*"; sh_dev "$@"; }

TMPD="$(mktemp -d "${TMPDIR:-/tmp}/spot-apk.XXXXXX")"
trap 'rm -rf "$TMPD"' EXIT

install_any() {
  # $1 = label, $2 = path
  local label="$1" src="$2" n
  [ -n "$src" ] && [ "$src" != "-" ] || return 0
  [ -e "$src" ] || die "$label: not found: $src"
  step "Installing $label: $(basename "$src")"
  if [ -d "$src" ]; then
    n="$(find "$src" -maxdepth 1 -name '*.apk' | wc -l | tr -d ' ')"
    [ "$n" -gt 0 ] || die "$label: no .apk files in $src"
    # shellcheck disable=SC2046
    adb install-multiple -r $(find "$src" -maxdepth 1 -name '*.apk' | sort) </dev/null
  else
    case "$(lower "$src")" in
      *.apks|*.apkm|*.xapk|*.zip)
        local d="$TMPD/$label"
        mkdir -p "$d"
        unzip -q -o "$src" '*.apk' -d "$d" || die "$label: could not unpack bundle (.apkm may be encrypted - use the .apk/.apks variant)"
        # shellcheck disable=SC2046
        adb install-multiple -r $(find "$d" -name '*.apk' | sort) </dev/null ;;
      *.apk) adb install -r "$src" </dev/null ;;
      *) die "$label: unknown file type $src" ;;
    esac
  fi
  ok "$label installed"
}

step "Checking the Spot"
need_adb; require_single_device
st="$(adb_state)"
[ "$st" = "device" ] || die "Spot must be booted with USB debugging (adb get-state = device), got '$st'."
SDK="$(sh_dev getprop ro.build.version.sdk)"
LOS="$(sh_dev getprop ro.lineage.version)"
DEV="$(sh_dev getprop ro.product.device)"
ok "device=$DEV sdk=$SDK lineage=${LOS:-?}"
[ "$SDK" = "30" ] || warn "Expected Android 11 (sdk 30) - continuing anyway."

install_any "Claude" "${1:-}"
install_any "HeyClaude" "${2:-}"
install_any "AuroraStore" "${3:-}"

step "Packages"
CLAUDE_PKGS="$(sh_dev pm list packages | sed 's/^package://' | grep -i claude | grep -v "^$PKG\$" || true)"
if [ -z "$CLAUDE_PKGS" ]; then
  warn "No Claude app found. Install it (claude.apk or via Aurora Store) and re-run."
else
  printf '%s\n' "$CLAUDE_PKGS" | sed 's/^/     /'
  if printf '%s\n' "$CLAUDE_PKGS" | grep -x 'com.anthropic.claude' >/dev/null; then
    ok "Claude app: com.anthropic.claude"
  else
    warn "Claude package is not com.anthropic.claude - set it in Hey Claude's settings ('Claude package')."
  fi
fi
ALL_PKGS="$(sh_dev pm list packages)"
printf '%s\n' "$ALL_PKGS" | grep -x "package:$PKG" >/dev/null || die "$PKG is not installed - pass heyclaude.apk as 2nd argument."
PKG_DUMP="$(sh_dev dumpsys package "$PKG")"
ok "$PKG installed ($(printf '%s\n' "$PKG_DUMP" | awk -F= '/versionName/ {print $2; exit}'))"

step "Permissions + accessibility for $PKG"
hint "RECORD_AUDIO, SYSTEM_ALERT_WINDOW (background start), accessibility service, battery whitelist"
if confirm "Apply these settings?"; then
  sh_set pm grant "$PKG" android.permission.RECORD_AUDIO && ok "RECORD_AUDIO granted"
  sh_set appops set "$PKG" SYSTEM_ALERT_WINDOW allow && ok "SYSTEM_ALERT_WINDOW allowed"

  CUR="$(sh_dev settings get secure enabled_accessibility_services)"
  case ":$CUR:" in
    *":$A11Y:"*|*":$A11Y_LONG:"*) ok "accessibility service already enabled" ;;
    *)
      if [ -z "$CUR" ] || [ "$CUR" = "null" ]; then NEW="$A11Y"; else NEW="$CUR:$A11Y"; fi
      sh_set settings put secure enabled_accessibility_services "$NEW"
      ok "enabled_accessibility_services = $NEW" ;;
  esac
  sh_set settings put secure accessibility_enabled 1
  sh_set dumpsys deviceidle whitelist "+$PKG" >/dev/null && ok "battery optimisation whitelist"

  if [ -n "$CLAUDE_PKGS" ] && printf '%s\n' "$CLAUDE_PKGS" | grep -x 'com.anthropic.claude' >/dev/null; then
    if confirm "Also pre-grant the microphone to the Claude app?"; then
      if sh_set pm grant com.anthropic.claude android.permission.RECORD_AUDIO; then
        ok "Claude RECORD_AUDIO granted"
      else
        warn "could not grant (the app will ask itself)"
      fi
    fi
  fi
fi

step "Kiosk settings"
if confirm "Disable the lock screen (the Spot wakes straight into the app; anyone nearby can use it)?"; then
  out="$(sh_set locksettings set-disabled true || true)"
  printf '%s\n' "$out" | sed 's/^/     /'
fi
if confirm "Set screen timeout to $((SCREEN_TIMEOUT_MS / 1000)) s (SCREEN_TIMEOUT_MS)?"; then
  sh_set settings put system screen_off_timeout "$SCREEN_TIMEOUT_MS" && ok "screen_off_timeout = $SCREEN_TIMEOUT_MS ms"
fi

step "Verify"
info "enabled_accessibility_services = $(sh_dev settings get secure enabled_accessibility_services)"
info "accessibility_enabled          = $(sh_dev settings get secure accessibility_enabled)"
info "SYSTEM_ALERT_WINDOW            = $(sh_dev appops get "$PKG" SYSTEM_ALERT_WINDOW)"
PKG_DUMP="$(sh_dev dumpsys package "$PKG")"
if printf '%s\n' "$PKG_DUMP" | grep 'android.permission.RECORD_AUDIO: granted=true' >/dev/null; then
  ok "RECORD_AUDIO granted=true"
else
  warn "RECORD_AUDIO not granted yet - open Hey Claude and allow the microphone."
fi

step "Starting Hey Claude"
sh_dev am start -n "$PKG/.MainActivity" | sed 's/^/     /'
ok "Done."
hint "Watch the logs:    adb logcat -s HeyClaude"
hint "Mic level test:    adb shell am start -a ai.pocket.heyclaude.MIC_TEST -n $PKG/.MainActivity"
hint "Dump UI nodes:     adb shell am broadcast -a ai.pocket.heyclaude.DUMP_NODES -p $PKG"
hint "Own wake word:     adb push hey_claude.onnx /sdcard/Android/data/$PKG/files/hey_claude.onnx"
