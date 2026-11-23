# shellcheck shell=bash
# Shared helpers for the Echo Spot (rook) scripts.
#
# Must stay compatible with macOS /bin/bash 3.2:
#   - no associative arrays, no mapfile/readarray, no ${var,,} / ${var^^}
#   - "${arr[@]}" on an empty array trips `set -u` in bash 3.2, so always
#     expand with ${arr[@]+"${arr[@]}"}
#   - use `wc -c` instead of GNU/BSD-specific `stat`
#
# Source it with:  . "$(cd "$(dirname "$0")" && pwd)/lib.sh"

# ---------------------------------------------------------------- colors ----
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
  C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
  C_BLU=$'\033[34m'; C_ORG=$'\033[38;5;173m'; C_BLD=$'\033[1m'; C_RST=$'\033[0m'
else
  C_RED=''; C_GRN=''; C_YEL=''; C_BLU=''; C_ORG=''; C_BLD=''; C_RST=''
fi

info()  { printf '%s\n' "${C_BLU}[..]${C_RST} $*"; }
ok()    { printf '%s\n' "${C_GRN}[ok]${C_RST} $*"; }
warn()  { printf '%s\n' "${C_YEL}[!!]${C_RST} $*" >&2; }
err()   { printf '%s\n' "${C_RED}[xx]${C_RST} $*" >&2; }
die()   { err "$*"; exit 1; }
step()  { printf '\n%s\n' "${C_ORG}${C_BLD}==> $*${C_RST}"; }
hint()  { printf '%s\n' "     $*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# Lower-case without bash-4 ${var,,}
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# ------------------------------------------------------------ confirming ----
# confirm "Question"           -> y/N, returns 0 on yes
# ASSUME_YES=1 skips prompts (never used for the destructive YES prompts).
confirm() {
  local answer
  if [ "${ASSUME_YES:-0}" = "1" ]; then return 0; fi
  printf '%s' "${C_YEL}??${C_RST} $1 [y/N] "
  read -r answer || return 1
  case "$(lower "$answer")" in
    y|yes|j|ja) return 0 ;;
    *) return 1 ;;
  esac
}

# confirm_hard "What will happen" -> user must type YES (upper case)
confirm_hard() {
  local answer
  printf '%s\n' "${C_RED}${C_BLD}!! $1${C_RST}"
  printf '%s' "   Type YES to continue, anything else aborts: "
  read -r answer || return 1
  [ "$answer" = "YES" ]
}

pause() {
  local _x
  printf '%s' "${C_BLU}>>${C_RST} ${1:-Press Enter to continue...} "
  read -r _x || true
}

# --------------------------------------------------------------- hashing ----
# sha256 of a local file (macOS: shasum, Linux: sha256sum)
sha256_local() {
  if have shasum; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif have sha256sum; then
    sha256sum "$1" | awk '{print $1}'
  else
    die "Neither shasum nor sha256sum found."
  fi
}

# sha256 of a file/block device on the device (TWRP/toybox has sha256sum)
sha256_remote() {
  adb shell "sha256sum '$1' 2>/dev/null" </dev/null | tr -d '\r' | awk '{print $1}'
}

file_size() { wc -c < "$1" | tr -d ' '; }

# ------------------------------------------------------ partition guard -----
# These partitions hold the boot chain. Writing to them can hard-brick the
# Spot (only recoverable by opening it and shorting a test point). The
# scripts in this folder never write to ANY block device; this guard is a
# second, explicit safety net.
PROTECTED_PARTS_RE='^(preloader|lk|lk_real|tee|tee1|tee2|tee1_real|tee2_real|tee[0-9a-z_]*_real|mmcblk0boot0|mmcblk0boot1)$'

is_protected_part() {
  local n
  n="$(lower "$(basename "$1")")"
  n="${n%.img}"
  [[ "$n" =~ $PROTECTED_PARTS_RE ]] || [[ "$n" == tee* ]]
}

# Refuse any shell command that would write to a block device or touch a
# protected partition by name. Call before every adb shell / fastboot that
# is not a pure read.
guard_write() {
  local cmd="$*" word
  local blk_re='(of=|>[[:space:]]*)/dev/block'
  if [[ "$cmd" =~ $blk_re ]]; then
    die "SAFETY GUARD: refusing to write to a block device: $cmd"
  fi
  for word in $cmd; do
    word="${word#*/by-name/}"
    if is_protected_part "$word"; then
      case "$cmd" in
        *flash*|*erase*|*of=*|*">"*|*format*|*dd\ *)
          die "SAFETY GUARD: '$word' is a protected boot-chain partition (preloader/lk/tee). Never write to it." ;;
      esac
    fi
  done
  return 0
}

# ---------------------------------------------------------------- adb -------
need_adb() {
  have adb || die "adb not found. Install with: brew install android-platform-tools"
}

adb_state() {
  # prints device | recovery | sideload | unauthorized | offline | none
  local s
  s="$(adb get-state 2>/dev/null | tr -d '\r' || true)"
  if [ -z "$s" ]; then
    if adb devices 2>/dev/null | grep -q 'unauthorized'; then s=unauthorized; else s=none; fi
  fi
  printf '%s' "$s"
}

adb_count() {
  adb devices 2>/dev/null | awk 'NR>1 && NF>=2 {n++} END {print n+0}'
}

require_single_device() {
  local n
  n="$(adb_count)"
  if [ "$n" -gt 1 ]; then
    die "More than one adb device connected. Unplug the others or set ANDROID_SERIAL."
  fi
}
