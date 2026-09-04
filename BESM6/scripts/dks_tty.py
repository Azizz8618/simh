#!/usr/bin/env python3
# dks_tty.py — постоянный telnet-клиент ДКС для run_test_dks.sh
#
# Роль в основном сценарии теста (вместо прежнего nc):
#   - одно постоянное подключение к целевой линии (регистрирует DKS)
#   - t+3: первый ETX (0x03) — «гид» ОС Диспак; далее ETX каждые 15 сек,
#     пока живо соединение (ОС может стартовать несколько минут)
#   - t+6: символ 'A' — проверка dks_poll
#   - весь полученный вывод — в лог с метками времени; run_test_dks.sh
#     по этому логу проверяет реакцию ОС (приглашение «ЭВМ-3, Т-NNN»)
#
# Использование: dks_tty.py <порт> <лог_RX> [длительность_сек]
import socket, time, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 4199
rxlog_path = sys.argv[2] if len(sys.argv) > 2 else '/tmp/besm6_dks_rx.log'
DURATION = int(sys.argv[3]) if len(sys.argv) > 3 else 120
ETX_EVERY = 15         # период отправки ETX
ETX_FIRST = 3          # первый ETX
CHAR_AT = 6            # символ 'A' для dks_poll

def strip_iac(buf):
    """Удалить telnet IAC-последовательности (RFC 854) из потока байтов."""
    out = bytearray()
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]
        if b == 0xFF and i + 1 < n:
            cmd = buf[i + 1]
            if 0xFB <= cmd <= 0xFE:      # WILL/WONT/DO/DONT + option
                i += 3
            elif cmd == 0xFA:            # SB ... IAC SE
                j = buf.find(b'\xff\xf0', i)
                i = (j + 2) if j != -1 else n
            else:
                i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)

rxlog = open(rxlog_path, 'wb', buffering=0)

def ts():
    return time.strftime('%H:%M:%S')

try:
    s = socket.create_connection(('127.0.0.1', port), 5)
    s.settimeout(0.5)
    print(f'[{ts()}] connected to port {port}')
    t0 = time.time()
    sent_etx = sent_char = 0
    while time.time() - t0 < DURATION:
        t = time.time() - t0
        if t >= ETX_FIRST and t // ETX_EVERY + (1 if t >= ETX_FIRST else 0) > sent_etx:
            sent_etx += 1
            s.sendall(b'\x03')
            print(f'[{ts()}] >>> ETX sent (#{sent_etx})')
        if not sent_char and t >= CHAR_AT:
            s.sendall(b'A')
            sent_char = 1
            print(f'[{ts()}] >>> char A sent')
        try:
            d = s.recv(4096)
            if d:
                clean = strip_iac(d)
                if clean:
                    rxlog.write(clean)
                line = clean.rstrip(b'\r\n')
                if line:
                    print(f'[{ts()}] RX {len(clean)}: {line[:80]!r}')
        except socket.timeout:
            pass
except Exception as e:
    print(f'[{ts()}] ERROR: {e}')
    sys.exit(1)
finally:
    rxlog.close()
