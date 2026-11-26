#!/usr/bin/env bash
# Проверка: гостевая программа E10025 выдаёт байты на TTO, внешний ie15emu их видит.
# Внутренний терминал не моделируется; используется только telnet-консоль SIMH.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
ie15_dir=${IE15_DIR:-/home/azizz/Yandex.Disk/ie15}
logdir=${LOGDIR:-/tmp/.private/azizz/opencode/emvs}
guest_text=${GUEST_TEXT:-P6GUEST7}

fail () {
  echo "FAIL[$1]: $2" >&2
  exit 1
}

[ -x "$sim" ] || fail sim "simulator not built: $sim"
[ -d "$ie15_dir" ] || fail ie15 "directory not found: $ie15_dir"
command -v python3 >/dev/null 2>&1 || fail python3 "python3 not found"
mkdir -p "$logdir" || fail logdir "cannot create $logdir"
work=$(mktemp -d "$logdir/ie15-guest.XXXXXX") || fail tmp "mktemp failed"
fifo="$work/sim.in"
ie_in="$work/ie15.in"
simlog="$work/sim.log"
ielog="$work/ie15.log"
mkfifo "$fifo" "$ie_in" || fail fifo "cannot create fifo"

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
  if [ -n "${ie_pid:-}" ]; then
    exec 4>&- 2>/dev/null || true
    kill -9 "$ie_pid" 2>/dev/null || true
    fi
  if [ -n "${sim_pid:-}" ]; then
    printf 'exit\n' >&3 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    kill -9 "$sim_pid" 2>/dev/null || true
    fi
  rm -f "$fifo" "$ie_in"
  }
trap cleanup EXIT

# Первый step принимает ожидающее подключение telnet и оставляет PC=2.
# Программа кладётся начиная с адреса 2, чтобы не трогать PC регистром deposit.
# Кодировка PDP-11/E10025 (восьмеричные слова, ведущий 0 = восьмеричное):
#   TSTB @#177564 = 0105737; BPL -3 = 0100375; MOVB #c,@#177566 = 0112737.
python3 - "$guest_text" > "$work/prog.dep" <<'PY'
import sys
text = sys.argv[1].encode("ascii")
base = 2
for ch in text:
    print("deposit %d 0105737" % base)
    print("deposit %d 0177564" % (base + 2))
    print("deposit %d 0100375" % (base + 4))
    print("deposit %d 0112737" % (base + 6))
    print("deposit %d %07o" % (base + 8, ch))
    print("deposit %d 0177566" % (base + 10))
    base += 12
# Ждать завершения последней записи, иначе HALT может наступить до
# обслуживания TTO и последний символ не уйдёт в telnet.
print("deposit %d 0105737" % base)
print("deposit %d 0177564" % (base + 2))
print("deposit %d 0100375" % (base + 4))
print("deposit %d 0000000" % (base + 6))
PY

"$sim" < "$fifo" > "$simlog" 2>&1 &
sim_pid=$!
sleep 0.3
exec 3>"$fifo"
printf 'set console telnet=%d\nset CPU BOOTEVENTS=on\n' "$port" >&3

for _ in $(seq 1 50); do
  grep -Fq "Listening on port $port" "$simlog" && break
  sleep 0.1
  done
grep -Fq "Listening on port $port" "$simlog" || \
  fail attach "SIMH did not listen on port $port, see $simlog"

# Документированная команда ie15/README.md:684. Флаг --dump подразумевает --headless.
# stdin держится открытым, чтобы терминал не закрылся до выдачи гостевых байт.
(
  cd "$ie15_dir" || exit 1
  exec timeout 45 python3 -m ie15emu --line "tcp://127.0.0.1:$port" --dump - --status
) < "$ie_in" > "$ielog" 2>&1 &
ie_pid=$!
exec 4>"$ie_in"

sleep 0.7
printf 'step\n' >&3
sleep 0.5

while IFS= read -r cmd; do
  printf '%s\n' "$cmd" >&3
  done < "$work/prog.dep"

# Ограниченный шаг вместо бесконечного go: гостевая программа сама становится на HALT.
printf 'step 100000\n' >&3
# Последняя буква может ожидать в очереди TTO после HALT: продолжаем
# обслуживание событий, чтобы сбросить её в telnet.
printf 'cont\n' >&3
# stdin терминала держим открытым до конца выдачи, иначе он выйдет после баннера.
sleep 8

exec 4>&-
sleep 1
# --dump --status не завершается по EOF, поэтому останавливаем процесс явно.
if kill -0 "$ie_pid" 2>/dev/null; then
  kill -9 "$ie_pid" 2>/dev/null || true
  ie_rc=137
  else
  ie_rc=0
  wait "$ie_pid" || ie_rc=$?
  fi
[ "$ie_rc" -eq 0 ] || [ "$ie_rc" -eq 137 ] || \
  fail ie15 "ie15emu exited with $ie_rc, see $ielog"

grep -Fq "$banner" "$ielog" || \
  fail recv "ie15emu did not receive the CON-TELNET banner, see $ielog"

# Убираем строку приветствия SIMH: она выдаётся самим эмулятором, не гостем.
grep -Fv "$banner" "$ielog" > "$work/ie15.nobanner" || true
payload=$(cat "$work/ie15.nobanner")

printf 'guest_text=%s\n' "$guest_text"
printf 'ie15_without_banner=%q\n' "$payload"
printf 'logs: %s\n' "$work"

if ! grep -Fq "$guest_text" "$work/ie15.nobanner"; then
  echo "FAIL[guest-echo]: ie15emu did not receive guest TTO string '$guest_text'" >&2
  echo "FAIL: ie15 frontend received only the SIMH CON-TELNET banner, not guest output" >&2
  exit 1
  fi

echo "PASS: ie15emu received guest TTO bytes '$guest_text' from E10025"
echo "PASS: ie15 frontend displays guest-generated output, not only the CON-TELNET banner"
