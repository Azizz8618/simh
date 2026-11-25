#!/usr/bin/env bash
# Phase F system test: DECUS C HELLO.C on RT-11 V4 in E10025.
# The compiler disk is a copy under /tmp/.private/azizz/opencode/emvs/.
# HELLO.C is injected offline, as in .localdocs/RT11-C-VERIFICATION.md.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL[sim]: simulator not built: $sim" >&2
  exit 1
fi

IMAGE='/tmp/.private/azizz/opencode/emvs/rt11/extracted/RT-11 V2.0 Freeware CD/dsk/misc/decusc.dsk'
HASH=f00977cd292222bcc59326a5677d9320a434976ae2cf5ea1039d385e6ea94e51
BOOT=/tmp/.private/azizz/opencode/emvs/rt11/kits/Disks/rtv4_rk.dsk
BOOTHASH=ebf0da98feffe3cd82aad95c3f4143757be512199058f996fc15e7e96c173978
WORK=/tmp/.private/azizz/opencode/emvs/phasef
mkdir -p "$WORK"

check_hash () {
  src=$1; want=$2
  if [ ! -f "$src" ]; then
    echo "FAIL[image]: no image: $src" >&2
    exit 1
  fi
  got=$(sha256sum "$src" | awk '{print $1}')
  if [ "$got" != "$want" ]; then
    echo "FAIL[hash]: $src has $got, expected $want" >&2
    exit 1
  fi
}
check_hash "$IMAGE" "$HASH"
check_hash "$BOOT" "$BOOTHASH"
echo "PASS[hash]: canonical decusc.dsk and rtv4_rk.dsk"

cp "$BOOT" "$WORK/rtv4_rk_c.dsk"
cp "$IMAGE" "$WORK/decusc_c.dsk"

cat > "$WORK/HELLO.C" <<'EOF'
#include <stdio.h>
main()
{
puts("Hello, RT-11!");
}
EOF

python3 - "$WORK/decusc_c.dsk" "$WORK/HELLO.C" <<'PY'
import sys
src, textfile = sys.argv[1:3]
entry = 0x114
d = bytearray(open(src, 'rb').read())
off = 6 * 512
blk = d[off:off + 512]
def w(o): return int.from_bytes(blk[o:o+2], 'little')
def putw(o, v): blk[o:o+2] = v.to_bytes(2, 'little')
T = ' ' + ''.join(chr(65+i) for i in range(26)) + ''.join(str(i) for i in range(10)) + '. $%'
def enc(s):
    v = 0
    for c in s.upper()[:3].ljust(3): v = v * 40 + T.index(c)
    return v
start = w(8) + sum(w(o+8) for o in range(10, entry, 14))
text = open(textfile, 'rb').read().replace(b'\r\n', b'\n')
assert len(text) <= 512 * w(entry+8)
putw(entry, 0o2000)
putw(entry+2, enc('HEL'))
putw(entry+4, enc('LO'))
putw(entry+6, enc('C'))
putw(entry+8, 1)
d[off:off+512] = blk
d[start*512:start*512+len(text)] = text
open(src, 'wb').write(d)
print(f'PASS[inject]: HELLO.C entry@{entry:03x} block={start}')
PY

cat > "$WORK/rt11_c_e10025.ini" <<EOF
set CHAN TIMING=scaled
set RK enabled
attach RK0 $WORK/rtv4_rk_c.dsk
attach RK1 $WORK/decusc_c.dsk
boot RK0
EOF

dev=$(printf 'attach RK1 %s\nshow rk1\nquit\n' "$WORK/decusc_c.dsk" | "$sim" 2>&1)
if ! printf '%s\n' "$dev" | grep -q 'attached to'; then
  echo 'FAIL[rk1]: DECUS C copy not attachable as RK1' >&2
  exit 1
fi
echo 'PASS[rk1]: DECUS C copy attached as RK1'

LOG=$WORK/rt11_c_run.log
(sleep 6; printf 'ASSIGN RK1: C:\n'; sleep 2; \
 printf 'RUN C:CC C:HELLO.C\n'; sleep 4; \
 printf 'RUN C:AS HELLO.S/D\n'; sleep 3; \
 printf 'LINK/BOT:2000 HELLO.OBJ,C:SUPORT,C:CLIB\n'; sleep 3; \
 printf 'DIR HELLO.*\n'; sleep 2; \
 printf 'RUN HELLO\n'; sleep 4; \
 printf '\005quit\n'; sleep 2) | timeout 90 "$sim" "$WORK/rt11_c_e10025.ini" > "$LOG" 2>&1 || true

if printf '%s\n' "$LOG" | grep -q 'Hello, RT-11!'; then
  echo "PASS: DECUS C chain printed Hello, RT-11! ($LOG)"
else
  echo "[GAP] DECUS C chain needs a booted RT-11 V4 in E10025: see $LOG" >&2
  exit 1
fi
