#!/usr/bin/env bash
# Phase F system test: RT-11 V4.00C from the canonical RK05 image.
# The image hash is checked before attach. The work copy and log stay in
# /tmp/.private/azizz/opencode/emvs/, never in the repository.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL[sim]: simulator not built: $sim" >&2
  exit 1
fi

IMAGE=/tmp/.private/azizz/opencode/emvs/rt11/kits/Disks/rtv4_rk.dsk
HASH=ebf0da98feffe3cd82aad95c3f4143757be512199058f996fc15e7e96c173978
WORK=/tmp/.private/azizz/opencode/emvs/phasef
mkdir -p "$WORK"

if [ ! -f "$IMAGE" ]; then
  echo "FAIL[image]: no canonical image: $IMAGE" >&2
  exit 1
fi
actual=$(sha256sum "$IMAGE" | awk '{print $1}')
if [ "$actual" != "$HASH" ]; then
  echo "FAIL[hash]: $IMAGE has $actual, expected $HASH" >&2
  exit 1
fi
echo "PASS[hash]: canonical rtv4_rk.dsk $HASH"

cp "$IMAGE" "$WORK/rtv4_rk_boot.dsk"
cat > "$WORK/rt11_v4_e10025.ini" <<EOF
set CPU BOOTEVENTS=on
set RK enabled
attach RK0 $WORK/rtv4_rk_boot.dsk
boot RK0
EOF

# Device-level facts are deterministic and checked before the guest boot.
dev=$(printf 'set RK enabled\nattach RK0 %s\nshow RK\nquit\n' \
  "$WORK/rtv4_rk_boot.dsk" | "$sim" 2>&1)
if ! printf '%s\n' "$dev" | grep -q 'address=17777400-17777417, vector=220'; then
  echo "FAIL[rk]: expected RK at CSR 01777400, vector 0220" >&2
  printf '%s\n' "$dev" | tail -20 >&2
  exit 1
fi
echo 'PASS[rk]: RK0 attached, CSR 01777400, vector 0220'

LOG=$WORK/rt11_v4_boot.log
(sleep 6; printf 'DIR\n'; sleep 6; printf '\005quit\n'; sleep 2) | \
  timeout 60 "$sim" "$WORK/rt11_v4_e10025.ini" > "$LOG" 2>&1 || true

ok=1
printf '%s\n' "$LOG" | grep -q 'RT-11SJ  *V04\.00C' || { echo "FAIL[console]: no RT-11SJ V04.00C in $LOG" >&2; ok=0; }
printf '%s\n' "$LOG" | grep -q 'Welcome to RT-11 Version 4' || { echo "FAIL[console]: no welcome string in $LOG" >&2; ok=0; }
printf '%s\n' "$LOG" | grep -q '^\.' || { echo "FAIL[console]: no dot prompt in $LOG" >&2; ok=0; }
printf '%s\n' "$LOG" | grep -qE '[A-Z0-9] +\.[A-Z]+ +[0-9]+' || { echo "FAIL[dir]: no directory listing in $LOG" >&2; ok=0; }

if [ "$ok" -eq 0 ]; then
  echo "[GAP] Guest RT-11 V4 boot in E10025: see $LOG" >&2
  exit 1
fi
echo "PASS: RT-11 V4.00C booted from canonical RK05 and DIR listed files ($LOG)"
