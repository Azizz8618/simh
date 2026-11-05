#!/usr/bin/env python3
"""dks_telnet_test.py - telnet client for the DKS stage A tests.

Connects to the DKS port, waits for the connect/term-assignment chain,
sends its stdin (LF -> CR/LF), prints everything received.  The test
runner greps the output for the expected strings.

Usage: dks_telnet_test.py host port [seconds]
"""
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0

s = socket.create_connection((host, port), timeout=5)
s.settimeout(0.2)

out = bytearray()
deadline = time.time() + timeout
sent = False

def pump():
    try:
        chunk = s.recv(4096)
        if not chunk:
            return False
        out.extend(chunk)
        # strip telnet IAC sequences for display
        while True:
            i = out.find(b'\xff')
            if i < 0:
                break
            if i + 1 < len(out) and out[i+1] == 255:    # IAC IAC
                del out[i:i+2]
            else:                                       # IAC X Y
                del out[i:i+3]
    except socket.timeout:
        pass
    return True

while time.time() < deadline:
    if not sent:
        # let the connect -> sysreq -> reply -> greeting chain finish
        t0 = time.time()
        while time.time() - t0 < 2.0 and time.time() < deadline:
            pump()
        data = sys.stdin.buffer.read()
        if data:
            s.sendall(data.replace(b'\n', b'\r\n'))
        sent = True
    pump()

sys.stdout.buffer.write(bytes(out))
sys.stdout.flush()
s.close()
