#!/bin/sh
# run_test_dks_remote.sh - stage C: the real Диспак ОС behind the
# КАДОПАМ TCP bridge.
#
# Usage: BESM6/run_test_dks_remote.sh [BIN/besm6] [BIN/pdp11] [порт-telnet]
#        (BPORT=... overrides the bridge port, default 4140)
#
# The local variant (BESM6/test_dks.ini) boots Диспак with lines 11-17
# in the DKS mode and the ОЗУ of the Э-60 emulated inside the БЭСМ-6
# process.  This runner keeps the Диспак configuration but moves the
# Э-60 into a SEPARATE simulator (BIN/pdp11, SET DKS BRIDGE): every
# 032/0132 of the real ОС crosses the TCP bridge, like with real
# hardware.
#
# REQUIRES the Диспак disk images of the local test (sbor2053.bin,
# krab2063.bin, svs2048.bin, alt2048.bin, sbor2048.bin, 2052.bin,
# drum1x.bin, drum2x.bin) in the current directory - they are not part
# of the repository.  Lines 11-17 of the БЭСМ-6 tty unit must be in the
# DKS mode: uncomment the 'set tty11 dks' lines of test_dks.ini or copy
# the configuration here.
#
# Then: telnet localhost <порт-telnet> is a Диспак terminal behind the
# Э-60 concentrator; the ОС runs in the БЭСМ-6 process.

B6=${1:-./BIN/besm6}
P11=${2:-./BIN/pdp11}
PORT=${3:-4199}
BPORT=${BPORT:-4140}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'kill $P11PID 2>/dev/null; rm -rf "$TMP"' EXIT

if [ ! -f sbor2053.bin ]; then
    echo "Нет образов дисков Диспака (sbor2053.bin и др.) в текущем каталоге."
    echo "Скопируйте их из рабочей конфигурации test_dks.ini и повторите."
    exit 1
    fi

# The Э-60: the СВЯЗЬ7 hardware, the C engine, TESTOS off - the real
# ОС answers through the bridge.
cat > "$TMP/e60.ini" <<EOF
set dks enable
set dks engine=sim
set dks testos=off
set dks bridge=$BPORT
set dks encoding=utf8
set debug stdout
attach dks $PORT
deposit 200 0777
go 200
EOF

# The БЭСМ-6: the Диспак configuration with REMOTE instead of the local
# ОЗУ Э-60.  Start from test_dks.ini, point the ДКС lines at the bridge.
sed -e "s/^set dks enable/set dks enable\\
set dks remote=localhost:$BPORT\\
set dks chans=1/" "$DIR/test_dks.ini" > "$TMP/dispak.ini"

"$P11" "$TMP/e60.ini" > "$TMP/e60.log" 2>&1 &
P11PID=$!
sleep 2
echo "Э-60: telnet localhost $PORT (ДКС lines); bridge: $BPORT"
echo "БЭСМ-6: the Диспак boot, ДКС lines 11-17 -> the bridge"
"$B6" "$TMP/dispak.ini"
