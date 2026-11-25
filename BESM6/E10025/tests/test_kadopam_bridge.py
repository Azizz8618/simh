#!/usr/bin/env python3
"""Integrated КАДОПАМ regression: a BESM-6-style master drives the E10025 bridge.

Frame protocol is the one used by BESM6/besm6_dks.c REMOTE mode:
  request  D6 op chan addr_hi addr_lo val_hi val_lo 00
  response D7 op|0200 chan addr_hi addr_lo val_hi val_lo 00
Bulk frames (05/06) carry cnt data words after the 8-byte header.
"""
import os
import re
import socket
import subprocess
import sys
import time

SIM = os.environ.get("E10025", os.path.join(os.path.dirname(__file__), "..", "build", "e10025"))
PORT = int(os.environ.get("E10025_PORT", "4204"))
MAGIC_REQ, MAGIC_RSP = 0xD6, 0xD7
READ, WRITE, ATTN, PING, BREAD, BWRITE = 1, 2, 3, 4, 5, 6
CHAN = 2
CELL_STATUS = 0
CELL_TEST = 0o100
CELL_DOORBELL = 0o77777
# Vector chosen for DKS ATTN: free in the E10025 model (TTI 0060, TTO 0064,
# DMA 0070, CLK/auto 0300+, PIRQ 0240, traps below 0250 are all taken).
DKSATN_VEC = 0o150
HANDLER = 512                      # decimal address holding the handler HALT
EXPECTED_PC = 514                  # decimal PC after the handler HALT


def frame(op, addr=0, val=0, payload=b""):
    return bytes([MAGIC_REQ, op, CHAN, (addr >> 8) & 0xFF, addr & 0xFF,
                  (val >> 8) & 0xFF, val & 0xFF, 0]) + payload


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise RuntimeError("bridge closed")
        data += part
    return data


def exchange(sock, req, reply_len=8):
    sock.sendall(req)
    rsp = recv_exact(sock, reply_len)
    if rsp[0] != MAGIC_RSP or rsp[1] != (req[1] | 0x80):
        raise RuntimeError("bad response: " + rsp.hex())
    return rsp


def main():
    if not os.path.isfile(SIM):
        print("simulator not built: " + SIM, file=sys.stderr)
        return 1
    proc = subprocess.Popen([SIM], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    finished = False
    try:
        proc.stdin.write("set DKSE BRIDGE=%d\n" % PORT)
        # RTI drops the reset PSW IPL 7 to 0 so BR4 can be taken. The bounded
        # spin gives the bridge time to raise ATTN and, without the fix, ends
        # at its own HALT with a PC we can still examine.
        proc.stdin.write("deposit 0 000002\n")       # RTI
        proc.stdin.write("deposit 2 000000\n")
        proc.stdin.write("deposit SP 896\n")
        proc.stdin.write("deposit 896 000004\n")     # return PC = 000004
        proc.stdin.write("deposit 898 000000\n")     # return PSW, IPL 0
        proc.stdin.write("deposit 4 012700\n")       # MOV #0200000,R0
        proc.stdin.write("deposit 6 0200000\n")
        proc.stdin.write("deposit 8 005200\n")       # DEC R0
        proc.stdin.write("deposit 10 001376\n")      # BNE 000010
        proc.stdin.write("deposit 12 000000\n")      # HALT, spin exhausted
        # ATTN vector and handler: VEC -> 01000, handler contains HALT.
        proc.stdin.write("deposit %d 01000\n" % DKSATN_VEC)
        proc.stdin.write("deposit %d 000000\n" % (DKSATN_VEC + 2))
        proc.stdin.write("deposit %d 000000\n" % HANDLER)
        proc.stdin.write("go\n")
        proc.stdin.flush()
        deadline = time.time() + 5
        while time.time() < deadline:
            line = proc.stdout.readline()
            if "DKSE listening" in line:
                break
        else:
            raise RuntimeError("bridge did not start")
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        sock.settimeout(5)
        try:
            rsp = exchange(sock, frame(READ, CELL_STATUS))
            status = (rsp[5] << 8) | rsp[6]
            print("status cell: %o" % status)
            assert status == 0o400 << CHAN, "bad channel status"

            word = 0o12345
            exchange(sock, frame(WRITE, CELL_TEST, word))
            rsp = exchange(sock, frame(READ, CELL_TEST))
            got = (rsp[5] << 8) | rsp[6]
            print("word 0100: %o" % got)
            assert got == word, "write/read mismatch"

            words = [1, 2, 3]
            payload = b"".join(w.to_bytes(2, "big") for w in words)
            rsp = exchange(sock, frame(BWRITE, 0o101, len(words) << 8, payload))
            assert rsp[5] == len(words), "bulk write rejected"
            rsp = exchange(sock, frame(BREAD, 0o101, len(words) << 8), 8 + 2*len(words))
            got = [int.from_bytes(rsp[8+2*i:10+2*i], "big") for i in range(len(words))]
            print("bulk words:", got)
            assert got == words, "bulk read mismatch"

            rsp = exchange(sock, frame(WRITE, CELL_DOORBELL, 0o400))
            attn = recv_exact(sock, 8)
            print("ATTN frame:", attn.hex())
            assert attn[0] == MAGIC_REQ and attn[1] == ATTN, "no ATTN on doorbell"

            # The ATTN frame alone is not enough: the guest CPU must take the
            # vector and reach the handler HALT. Queued console commands are
            # processed once the CPU stops.
            proc.stdin.write("examine PC\n")
            proc.stdin.write("exit\n")
            proc.stdin.flush()
            finished = True
        finally:
            sock.close()
    finally:
        if not finished:
            try:
                proc.stdin.write("exit\n")
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass
        try:
            proc.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        tail = proc.stdout.read()

    pc_lines = [ln.strip() for ln in tail.splitlines() if "PC:" in ln]
    for ln in pc_lines:
        print("PC after ATTN:", ln)
    assert any(re.search(r"PC:\s*0*%d\b" % EXPECTED_PC, ln) for ln in pc_lines), \
        "guest CPU did not take ATTN vector %o (PC lines: %s)" % (DKSATN_VEC, pc_lines)
    print("PASS: BESM6 КАДОПАМ bridge -> E10025 integrated path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
