#!/usr/bin/env python3
"""dks_c_bulk_test.py - stage C acceptance: the BULK bridge opcodes.

Connects a RAW socket to the DKS bridge server (SET DKS BRIDGE=port on
the Э-60 side) and exercises the stage C frame extensions:

    BULK_READ   05:  D6 05 chan addr_hi addr_lo cnt 00 00
                     <- D7 85 chan addr_hi addr_lo cnt 00 00 + 2*cnt bytes
    BULK_WRITE  06:  D6 06 chan addr_hi addr_lo cnt 00 00 + 2*cnt bytes
                     <- D7 86 chan addr_hi addr_lo cnt 00 00

Bulk data covers addresses 1..077776: cell 0 (channel select) and the
doorbell 077777 are rejected (cnt = 0 in the reply), as are ranges that
run past 077777.

Usage: dks_c_bulk_test.py [host] [port]
"""

import socket
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4140

OP_READ, OP_WRITE, OP_ATTN, OP_PING, OP_BREAD, OP_BWRITE = 1, 2, 3, 4, 5, 6

passed = 0
failed = 0


def check(name, ok):
    global passed, failed
    if ok:
        passed += 1
        print("PASS: " + name)
    else:
        failed += 1
        print("FAIL: " + name)


def hdr(op, chan, addr, cnt=0, val=0):
    return bytes([0xD6, op, chan, (addr >> 8) & 0xFF, addr & 0xFF,
                  (cnt if op in (OP_BREAD, OP_BWRITE) else (val >> 8) & 0xFF),
                  (val & 0xFF) if op not in (OP_BREAD, OP_BWRITE) else 0, 0])


def recv_n(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed")
        buf += chunk
    return buf


def xact(s, req):
    """Send one request, return the response header (8 bytes)."""
    s.sendall(req)
    while True:
        rsp = recv_n(s, 8)
        if rsp[0] == 0xD7:                      # a response
            return rsp
        # an async ATTN (D6 03 ...) or noise: skip and keep waiting
        if rsp[0] == 0xD6:
            continue
        raise IOError("bad magic %02x" % rsp[0])


def xact_bulk_read(s, chan, addr, cnt):
    rsp = xact(s, hdr(OP_BREAD, chan, addr, cnt=cnt))
    payload = recv_n(s, 2 * rsp[5]) if rsp[5] else b""
    return rsp, payload


def xact_bulk_write(s, chan, addr, words):
    req = hdr(OP_BWRITE, chan, addr, cnt=len(words))
    for w in words:
        req += bytes([(w >> 8) & 0xFF, w & 0xFF])
    return xact(s, req)


def rd(rsp):
    return (rsp[5] << 8) | rsp[6]


def main():
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.settimeout(10)

    # C.1 PING round trip
    rsp = xact(s, hdr(OP_PING, 2, 0))
    check("C.1 PING echoed", rsp[0] == 0xD7 and rsp[1] == (OP_PING | 0x80))

    # C.2 single WRITE, then BULK_READ over the same words
    rsp = xact(s, hdr(OP_WRITE, 2, 0o02410, val=0o12525))
    check("C.2 single WRITE echoed", rsp[1] == (OP_WRITE | 0x80))
    rsp, pay = xact_bulk_read(s, 2, 0o02410, 8)
    words = [(pay[2 * i] << 8) | pay[2 * i + 1] for i in range(rsp[5])] \
        if rsp[5] else []
    check("C.2 BULK_READ count echoed", rsp[1] == (OP_BREAD | 0x80)
          and rsp[5] == 8)
    check("C.2 BULK_READ data", len(words) == 8 and words[0] == 0o12525
          and words[1] == 0)

    # C.3 BULK_WRITE x4, read back word by word
    vals = [0o111, 0o222, 0o333, 0o444]
    rsp = xact_bulk_write(s, 2, 0o02411, vals)
    check("C.3 BULK_WRITE echo count", rsp[1] == (OP_BWRITE | 0x80)
          and rsp[5] == 4)
    ok = True
    for i, w in enumerate(vals):
        rsp = xact(s, hdr(OP_READ, 2, 0o02411 + i))
        ok = ok and rd(rsp) == w
    check("C.3 BULK_WRITE applied", ok)

    # C.4 per-channel isolation: the same addresses on channel 0
    rsp = xact_bulk_write(s, 0, 0o02410, [0o7001, 0o7002])
    check("C.4 bulk to channel 0 echoed", rsp[1] == (OP_BWRITE | 0x80)
          and rsp[5] == 2)
    rsp = xact(s, hdr(OP_READ, 0, 0o02410))
    check("C.4 channel 0 data", rd(rsp) == 0o7001)
    rsp = xact(s, hdr(OP_READ, 2, 0o02410))
    check("C.4 channel 2 untouched", rd(rsp) == 0o12525)

    # C.5 rejections: cnt = 0, cell 0, doorbell overlap, overrun
    check("C.5 BULK_WRITE cnt=0 rejected",
          xact_bulk_write(s, 2, 0o02410, [])[5] == 0)
    check("C.5 BULK_WRITE cell 0 rejected",
          xact_bulk_write(s, 2, 0, [1, 2])[5] == 0)
    check("C.5 BULK_WRITE doorbell overlap rejected",
          xact_bulk_write(s, 2, 0o077775, [1, 2, 3, 4])[5] == 0)
    check("C.5 BULK_WRITE overrun rejected",
          xact_bulk_write(s, 2, 0o077776, [1, 2])[5] == 0)
    check("C.5 BULK_READ cell 0 rejected",
          xact_bulk_read(s, 2, 0, 2)[0][5] == 0)
    check("C.5 BULK_READ overrun rejected",
          xact_bulk_read(s, 2, 0o077770, 16)[0][5] == 0)

    # C.6 the doorbell still works after all the bulk traffic
    rsp = xact(s, hdr(OP_WRITE, 2, 0o077777, val=0o0400))
    check("C.6 doorbell write accepted", rsp[1] == (OP_WRITE | 0x80))

    s.close()
    print("bulk: passed %d, failed %d" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
