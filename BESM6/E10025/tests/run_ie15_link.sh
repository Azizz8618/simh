#!/usr/bin/env bash
# Проверка моста: внешний ie15emu подключается к telnet-консоли E10025.
# Внутри E10025 терминал не моделируется, VT52 и 15-ИЭ-013 не дублируются.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
ie15_dir=${IE15_DIR:-/home/azizz/Yandex.Disk/ie15}
logdir=${LOGDIR:-/tmp/.private/azizz/opencode/emvs}

fail () {
  echo "FAIL[$1]: $2" >&2
  exit 1
}

[ -x "$sim" ] || fail sim "simulator not built: $sim"
[ -d "$ie15_dir" ] || fail ie15 "directory not found: $ie15_dir"
command -v python3 >/dev/null 2>&1 || fail python3 "python3 not found"
mkdir -p "$logdir" || fail logdir "cannot create $logdir"
work=$(mktemp -d "$logdir/ie15-link.XXXXXX") || fail tmp "mktemp failed"
fifo="$work/sim.in"
simlog="$work/sim.log"
ielog="$work/ie15.log"
mkfifo "$fifo" || fail fifo "cannot create $fifo"

# Порт вне диапазона 4204-4210, который используют демо-конфиги БЭСМ-6.
port=$(python3 - <<'PY'
import socket
for p in range(43100, 43400):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
    except OSError:
        continue
    finally:
        s.close()
    print(p)
    break
else:
    raise SystemExit(1)
PY
) || fail port "no free port in 43100-43399"

banner='Connected to the ELECTRONICA-100-25 simulator CON-TELNET device'

cleanup () {
  if [ -n "${sim_pid:-}" ]; then
    printf 'exit\n' >&3 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    kill "$sim_pid" 2>/dev/null || true
    wait "$sim_pid" 2>/dev/null || true
    fi
  rm -f "$fifo"
  }
trap cleanup EXIT

# SIMH держит stdin открытым через FIFO, чтобы telnet-линию можно было
# активировать шагом после подключения терминала.
"$sim" < "$fifo" > "$simlog" 2>&1 &
sim_pid=$!
sleep 0.2
exec 3>"$fifo"
printf 'set console telnet=%d\n' "$port" >&3

for _ in $(seq 1 50); do
  grep -Fq "Listening on port $port" "$simlog" && break
  sleep 0.1
  done
grep -Fq "Listening on port $port" "$simlog" || \
  fail attach "SIMH did not listen on port $port, see $simlog"

# Точная команда из ie15/README.md:684. Флаг --dump подразумевает --headless.
(
  cd "$ie15_dir" || exit 1
  exec timeout 30 python3 -m ie15emu --line "tcp://127.0.0.1:$port" --dump - --status
) > "$ielog" 2>&1 &
ie_pid=$!

# Приём соединения SIMH происходит при первом шаге, после чего эмулятор отдаёт
# детерминированную строку приветствия CON-TELNET в линию.
sleep 0.7
printf 'step\n' >&3

ie_rc=0
wait "$ie_pid" || ie_rc=$?
[ "$ie_rc" -eq 0 ] || fail ie15 "ie15emu exited with $ie_rc, see $ielog"

grep -Fq "Listening on port $port" "$simlog" || \
  fail attach "SIMH telnet line missing in $simlog"
grep -Fq "$banner" "$ielog" || \
  fail recv "ie15emu did not receive E10025 banner, see $ielog"
grep -Fq "$port" "$ielog" || \
  fail channel "ie15emu status does not show port $port, see $ielog"

echo "PASS: ie15emu received E10025 CON-TELNET banner on tcp://127.0.0.1:$port"
echo "PASS: ie15emu --dump - --status is the external terminal frontend for E10025"
echo "logs: $work"
