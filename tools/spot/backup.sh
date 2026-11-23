#!/bin/bash
# backup.sh - full partition backup of the Echo Spot (rook) from TWRP.
#
# Reads every partition from the by-name directory (located on the device,
# usually /dev/block/platform/mtk-msdc.0/by-name) plus mmcblk0boot0/1, pulls
# it with adb, checks size (/proc/partitions) and sha256 (device vs. Mac).
# MANIFEST.txt (+ .sha256 and a COMPLETE marker) is only written when every
# partition verified; a partial run leaves MANIFEST.txt.partial behind.
# READ-ONLY on the device.
#
# Usage:
#   tools/spot/backup.sh [--dry-run] [--with-userdata] [outdir]
#     --dry-run        print the plan only; without a device it uses a
#                      built-in example map (for testing the script)
#     --with-userdata  also pull userdata (big, contains personal data)
#     outdir           default: ~/EchoSpot-Backup/spot-backup-YYYYmmdd-HHMMSS
#
# The backup contains device-unique data (serials, keys): keep it private.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=SCRIPTDIR/lib.sh
. "$HERE/lib.sh"

BYNAME=/dev/block/platform/mtk-msdc.0/by-name   # default / dry-run label
# Partitions a usable rook backup must contain (lower case).
CORE_PARTS="boot recovery system lk_real tee1_real tee2_real"
DRY=0; WITH_USERDATA=0; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --with-userdata) WITH_USERDATA=1 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    -*) die "Unknown option $1" ;;
    *) OUT="$1" ;;
  esac
  shift
done
[ -n "$OUT" ] || OUT="$HOME/EchoSpot-Backup/spot-backup-$(date +%Y%m%d-%H%M%S)"

# Example map (name target) and /proc/partitions sizes (1K blocks) used by
# --dry-run when no device is attached. Layout follows the rook guide;
# numbers on YOUR device may differ - the real run always reads the device.
example_map() {
  cat <<'EOF'
kb /dev/block/mmcblk0p1
dkb /dev/block/mmcblk0p2
lk /dev/null
lk_real /dev/block/mmcblk0p3
tee1 /dev/null
tee1_real /dev/block/mmcblk0p4
logo /dev/block/mmcblk0p5
tee2 /dev/null
tee2_real /dev/block/mmcblk0p6
expdb /dev/block/mmcblk0p7
MISC /dev/block/mmcblk0p8
boot /dev/block/mmcblk0p9
recovery /dev/block/mmcblk0p10
system /dev/block/mmcblk0p11
cache /dev/block/mmcblk0p12
userdata /dev/block/mmcblk0p13
EOF
}
example_sizes() {
  cat <<'EOF'
mmcblk0 3817472
mmcblk0p1 1024
mmcblk0p2 1024
mmcblk0p3 1024
mmcblk0p4 5120
mmcblk0p5 8192
mmcblk0p6 5120
mmcblk0p7 10240
mmcblk0p8 512
mmcblk0p9 16384
mmcblk0p10 16384
mmcblk0p11 1048576
mmcblk0p12 442368
mmcblk0p13 2259968
mmcblk0boot0 4096
mmcblk0boot1 4096
EOF
}

# --------------------------------------------------------------- source ----
SOURCE=device
if [ "$DRY" = 1 ]; then
  if have adb && [ "$(adb_state)" = "recovery" ]; then SOURCE=device; else SOURCE=example; fi
else
  need_adb
  require_single_device
  st="$(adb_state)"
  [ "$st" = "recovery" ] || die "Device must be in TWRP (adb get-state = recovery), got '$st'. Hold Volume Up while connecting power."
fi

if [ "$SOURCE" = device ]; then
  # The by-name path differs between kernels (mtk-msdc.0/by-name,
  # mtk-msdc.0/11230000.msdc0/by-name, ...): find it instead of hard-coding.
  # shellcheck disable=SC2016  # expanded on the device, not here
  BYNAME="$(adb shell 'for d in /dev/block/platform/*/by-name /dev/block/platform/*/*/by-name /dev/block/by-name; do [ -d "$d" ] && echo "$d"; done; true' </dev/null | tr -d '\r' | sed -n 1p)"
  [ -n "$BYNAME" ] || die "No by-name directory under /dev/block/platform on the device - is this really a rook in TWRP?"
  info "Reading partition map from $BYNAME"
  # name<space>resolved-target, one per line; skip a non-matching glob.
  MAP="$(adb shell "for f in $BYNAME/*; do [ -e \"\$f\" ] || continue; echo \"\${f##*/} \$(readlink -f \"\$f\")\"; done" </dev/null | tr -d '\r')"
  if [ -z "$MAP" ] || printf '%s\n' "$MAP" | grep 'No such file' >/dev/null; then
    die "Could not read $BYNAME - is this really a rook in TWRP?"
  fi
  SIZES="$(adb shell cat /proc/partitions | tr -d '\r' | awk 'NF==4 && $1 ~ /^[0-9]+$/ {print $4, $3}')"
  PRODUCT="$(adb shell getprop ro.product.device | tr -d '\r')"
  case "$PRODUCT" in *rook*) ;; *) warn "ro.product.device='$PRODUCT' (expected rook)";; esac
else
  warn "DRY RUN without a device in TWRP: using the built-in EXAMPLE map."
  MAP="$(example_map)"; SIZES="$(example_sizes)"; PRODUCT="rook (example)"
fi

size_kb() { printf '%s\n' "$SIZES" | awk -v d="$1" '$1 == d {print $2; exit}'; }

# ----------------------------------------------------------------- plan -----
# Plan lines: "<name> <blockdev> <bytes>"; skipped lines go to SKIPPED.
PLAN=""; SKIPPED=""; TOTAL=0
add_plan() {
  local name="$1" dev="$2" kb bytes
  kb="$(size_kb "${dev##*/}")"
  [ -n "$kb" ] || { SKIPPED="${SKIPPED}${name} (${dev}: not in /proc/partitions)
"; return 0; }
  bytes=$((kb * 1024))
  PLAN="${PLAN}${name} ${dev} ${bytes}
"
  TOTAL=$((TOTAL + bytes))
}

SEEN_DEVS=" "
while read -r name target; do
  [ -n "$name" ] || continue
  case "$target" in
    /dev/null|"") SKIPPED="${SKIPPED}${name} (-> /dev/null in TWRP, real copy is ${name}_real)
"; continue ;;
  esac
  lname="$(lower "$name")"
  if [ "$lname" = "userdata" ] && [ "$WITH_USERDATA" = 0 ]; then
    SKIPPED="${SKIPPED}${name} (personal data; use --with-userdata)
"; continue
  fi
  if [ "$lname" = "cache" ]; then
    SKIPPED="${SKIPPED}${name} (cache, not needed)
"; continue
  fi
  case "$SEEN_DEVS" in *" $target "*) SKIPPED="${SKIPPED}${name} (alias of already planned ${target})
"; continue ;; esac
  SEEN_DEVS="${SEEN_DEVS}${target} "
  add_plan "$name" "$target"
done <<EOF
$MAP
EOF
add_plan mmcblk0boot0 /dev/block/mmcblk0boot0
add_plan mmcblk0boot1 /dev/block/mmcblk0boot1

# Refuse a plan that lacks the core partitions (e.g. an unreadable by-name
# directory would otherwise leave only boot0/boot1 and look like success).
MISSING=""
for p in $CORE_PARTS; do
  printf '%s' "$PLAN" | awk -v p="$p" 'tolower($1) == p {f=1} END {exit !f}' || MISSING="$MISSING $p"
done

step "Backup plan ($PRODUCT) -> $OUT"
printf '%s' "$PLAN" | awk '{printf "     %-14s %-26s %12d bytes\n", $1, $2, $3}'
if [ -n "$SKIPPED" ]; then
  info "Skipped:"; printf '%s' "$SKIPPED" | sed 's/^/     - /'
fi
info "Total: $((TOTAL / 1024 / 1024)) MiB"
if [ -n "$MISSING" ]; then
  die "Plan is missing core partition(s):$MISSING - partition map looks wrong, refusing to back up."
fi

if [ "$DRY" = 1 ]; then
  ok "Dry run - nothing pulled."
  exit 0
fi

# Free space check (df -k works on macOS and Linux)
mkdir -p "$OUT"
if have git && git -C "$OUT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  warn "$OUT is inside a git repository - never commit this backup (device-unique data)."
  confirm "Use it anyway?" || exit 1
fi
FREE_KB="$(df -k "$OUT" | awk 'NR==2 {print $4}')"
if [ -n "$FREE_KB" ] && [ $((FREE_KB * 1024)) -lt "$TOTAL" ]; then
  die "Not enough free space in $OUT ($((FREE_KB / 1024)) MiB free)."
fi
confirm "Pull $((TOTAL / 1024 / 1024)) MiB into $OUT now? (read-only on the Spot)" || { info "Aborted."; exit 0; }

# Written as .partial; only renamed to MANIFEST.txt when everything verified,
# so flash-lineage.sh never mistakes a broken/interrupted run for a backup.
MANIFEST="$OUT/MANIFEST.txt.partial"
rm -f "$OUT/MANIFEST.txt" "$OUT/MANIFEST.txt.sha256" "$OUT/COMPLETE"
{
  echo "# Echo Spot (rook) partition backup"
  echo "# date:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# device:  $PRODUCT  serial: $(adb get-serialno 2>/dev/null | tr -d '\r')"
  echo "# by-name map:"
  printf '%s\n' "$MAP" | sed 's/^/#   /'
  echo "# name  blockdev  bytes  sha256  file"
} > "$MANIFEST"

FAIL=0
while read -r name dev bytes; do
  [ -n "$name" ] || continue
  file="$OUT/$name.img"
  step "$name ($dev, $((bytes / 1024)) KiB)"
  adb pull "$dev" "$file" </dev/null >/dev/null
  got="$(file_size "$file")"
  if [ "$got" != "$bytes" ]; then
    err "size mismatch: host $got != device $bytes"; FAIL=$((FAIL + 1)); continue
  fi
  h_dev="$(sha256_remote "$dev")"
  h_host="$(sha256_local "$file")"
  if [ -z "$h_dev" ] || [ "$h_dev" != "$h_host" ]; then
    err "sha256 mismatch: device ${h_dev:-?} host $h_host"; FAIL=$((FAIL + 1)); continue
  fi
  ok "size ok, sha256 $h_host"
  echo "$name $dev $bytes $h_host $name.img" >> "$MANIFEST"
done <<EOF
$PLAN
EOF

echo
if [ "$FAIL" -gt 0 ]; then
  die "$FAIL partition(s) failed verification - re-run before flashing anything! ($MANIFEST kept for diagnosis)"
fi
mv "$MANIFEST" "$OUT/MANIFEST.txt"
( cd "$OUT" && printf '%s  MANIFEST.txt\n' "$(sha256_local MANIFEST.txt)" > MANIFEST.txt.sha256 )
date -u +%Y-%m-%dT%H:%M:%SZ > "$OUT/COMPLETE"
ok "Backup complete: $OUT (MANIFEST.txt). Copy it somewhere safe and keep it private."
hint "Next: tools/spot/flash-lineage.sh lineage-18.1-*-rook.zip <sha256>"
