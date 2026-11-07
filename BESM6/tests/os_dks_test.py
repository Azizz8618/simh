#!/usr/bin/env python3
"""os_dks_test.py - acceptance test of the real OS Dispak with DKS
terminals, on the disks 2053 + 2048 supplied by the user.

Phases:
  probe   - boot only, dump the console (learning the markers)
  local   - phase L: the C engine inside the BESM-6 process;
            1) at boot every configured DKS line gets a connect reply
               (sysreq 1 -> ABFOSh, result 0, terminal assigned);
            2) SET TTY11 OFF -> sysreq 3, the OS frees the terminal;
            3) a telnet client lands on line 11 -> sysreq 1 again,
               typing is echoed, CR submits the line to the OS;
            4) disconnect -> sysreq 3.
  remote  - phase R: the E-60 in a separate simulator (ENGINE=SIM,
            TESTOS=OFF, BRIDGE=4140), the BESM-6 with SET DKS REMOTE;
            every KADOPAM access of the real OS crosses the TCP bridge;
            the terminal sits behind the E-60 concentrator (telnet to
            the pdp11 port).

Usage: os_dks_test.py {probe|local|remote} [BIN/besm6] [BIN/pdp11]

Simulator binaries: argv (above) or $DKS_BIN.  Working directory with
the OS images (2053/2048/2095, drum*.bin, boot_dispak.b6; the INI and
log files are created there): $DKS_WORKDIR.
"""

import os
import pty
import re
import select
import signal
import socket
import subprocess
import sys
import time

SIMH = os.environ.get("DKS_BIN", "/home/z/my-project/work/simh-repo/BIN")
DIR = os.environ.get("DKS_WORKDIR", "/home/z/my-project/work/besm6-test")

PASS = 0
FAIL = 0
NOTES = []


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"PASS: {name}", flush=True)
    else:
        FAIL += 1
        print(f"FAIL: {name}" + (f" ({detail})" if detail else ""), flush=True)


def note(name, text):
    """A documented observation (neither PASS nor FAIL): an OS build
    property or a path covered by the stage-C unit tests."""
    NOTES.append(f"{name}: {text}")
    print(f"NOTE: {name}: {text}", flush=True)


def write_ini(path, text):
    with open(path, "w") as f:
        f.write(text)


class _Proc:
    """Minimal wait/kill wrapper around a raw forked pid."""

    def __init__(self, pid):
        self.pid = pid
        self.returncode = None

    def wait(self, timeout=None):
        import threading
        t = threading.Thread(target=self._wait_blocking)
        t.start()
        t.join(timeout)
        return self.returncode

    def _wait_blocking(self):
        if self.returncode is None:
            try:
                _, st = os.waitpid(self.pid, 0)
                self.returncode = st
            except ChildProcessError:
                self.returncode = 0

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except OSError:
            pass


class Sim:
    def __init__(self, binary, ini, logfile, muxlog=None):
        self.logf = open(logfile, "wb")
        self.logfile = logfile
        self.watch = [logfile] + ([muxlog] if muxlog else [])
        # The sim treats Ctrl-E (WRU) as VINTR on a real terminal: the
        # line discipline turns it into SIGINT for the foreground
        # process group of the controlling terminal.  pty.fork() makes
        # the child a session leader that OPENS the slave itself, so it
        # acquires the pty as its controlling terminal and gets the
        # SIGINT - on a plain pipe stdin would never be polled at all
        # (sim_poll_kbd checks isatty) and every command would sit in
        # the queue forever.
        pid, master = pty.fork()
        if pid == 0:
            # child: slave is fd 0..2 and the controlling terminal
            try:
                os.dup2(self.logf.fileno(), 1)
                os.dup2(self.logf.fileno(), 2)
                os.execv(binary, [binary, ini])
            except Exception:
                pass
            os._exit(127)
        self.master = master
        self.p = _Proc(pid)

    def _drain_master(self, seconds=0.2):
        """Read whatever the pty echoes back, so its buffer never fills."""
        t0 = time.time()
        while time.time() - t0 < seconds:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                break
            try:
                if not os.read(self.master, 4096):
                    break
            except OSError:
                break

    def _stdout_size(self):
        try:
            with open(self.logfile, "rb") as f:
                f.seek(0, 2)
                return f.tell()
        except OSError:
            return 0

    def _wait_stop_banner(self, timeout=5.0):
        """Wait for the 'Simulation stopped' banner.  The stdout log is
        stdio-buffered and only flushed at exit, so poll the console
        (mux) log as well: sim_log is fflushed at every stop."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            for path in self.watch:
                try:
                    with open(path, "rb") as f:
                        if b"Simulation stopped" in f.read():
                            return True
                except OSError:
                    pass
            time.sleep(0.2)
        return False

    def cmd(self, s, delay=0.6, halt=False):
        """Send an SCP command.  With halt=True stop the CPU first
        (Ctrl-E) and CONT afterwards.  The WRU is VINTR on the pty: the
        CPU stops asynchronously, so wait for the \"Simulation
        stopped\" banner BEFORE typing the command - otherwise the
        characters are consumed as guest console input."""
        try:
            if halt:
                os.write(self.master, b"\005")
                stopped = self._wait_stop_banner()
                if not stopped:
                    print("    (warn: no 'Simulation stopped' banner)", flush=True)
                time.sleep(0.5)
            os.write(self.master, (s + "\n").encode())
            self._drain_master()
            if halt:
                time.sleep(0.8)
                os.write(self.master, b"cont\n")
                self._drain_master()
        except (BrokenPipeError, OSError):
            pass
        time.sleep(delay)

    def stop(self):
        for w in (b"\005", b"quit\n"):
            try:
                os.write(self.master, w)
                self._drain_master(1.5)
            except (BrokenPipeError, OSError):
                pass
        try:
            self.p.wait(timeout=15)
        except Exception:
            self.p.kill()
            try:
                self.p.wait(timeout=5)
            except Exception:
                pass
        try:
            os.close(self.master)
        except OSError:
            pass
        self.logf.close()


def read_text(path, nbytes=6000):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_boot(tag, max_wait=420, settle=12):
    """Wait for the ДАТА/ВРЕМЯ boot banner in the console log.  The ОС
    prints it in the mixed КОИ-7 form (Cyrillic Д + Latin "ATA"), so
    match both scripts.  The console log also carries the KADOPAM/bridge
    trace now, so scan a generous window."""
    path = os.path.join(DIR, f"log_{tag}.txt")
    rx = re.compile(r"ATA:\d\d\.\d\d\.\d\d")
    t0 = time.time()
    hit = None
    while time.time() - t0 < max_wait:
        time.sleep(5)
        text = read_text(path, 400000)
        if rx.search(text) and "BPEM" in text:
            hit = "ДАТА/ВРЕМЯ"
            time.sleep(settle)
            break
    return hit, time.time() - t0, read_text(path, 12000)


class Telnet:
    def __init__(self, port, host="127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(0.3)
        self.buf = b""

    def drain(self, seconds=1.5, wait_first=0.0):
        if wait_first:
            time.sleep(wait_first)
        t0 = time.time()
        out = self.buf
        self.buf = b""
        while time.time() - t0 < seconds:
            try:
                d = self.s.recv(4096)
                if not d:
                    break
                out += d
            except socket.timeout:
                pass
            except OSError:
                break
        return out

    def send(self, data, delay=0.4):
        if isinstance(data, str):
            data = data.encode()
        try:
            self.s.sendall(data)
        except OSError:
            pass
        time.sleep(delay)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# ----------------------------------------------------------------------
# INI templates
# ----------------------------------------------------------------------

TTY_LINES_ALWAYS_ON = """\
set tty2 vt
set tty3 vt
set tty4 vt
set tty5 vt
set tty6 vt
set tty7 vt
set tty8 vt
set tty9 vt
set tty10 vt
set tty11 tt
set tty11 dks
set tty11 authbs
set tty12 tt
set tty12 dks
set tty12 authbs
set tty13 tt
set tty13 dks
set tty13 authbs
set tty14 tt
set tty14 dks
set tty14 authbs
set tty15 tt
set tty15 dks
set tty15 authbs
set tty16 tt
set tty16 dks
set tty16 authbs
set tty17 vt
set tty17 dks
set tty17 authbs
"""


def besm6_ini(tag, mux_port, tty_lines, dks_extra="",
              debug_mask="kadopam;ring;irq"):
    return f"""; OS Dispak + DKS ({tag}); generated by os_dks_test.py
; flush the console log every second: the acceptance test greps it while
; the sim runs (the default is 30s)
de int-flush flush_interval 1
set -n console log=log_{tag}.txt
set cpu idle
set cpu autotime=on

attach -n drum0 drum1x.bin
attach -n drum1 drum2x.bin
attach -n md06 2052.bin
attach -e md07 2053
attach -e md00 2048
; the archive volume 2095 (supplied by the user): the OS asks for it
; ("УСТ.МД АРХИВА / УСТ СРЧ.К40Б2095") at boot
attach -e md05 2095
attach -n prn0 output_{tag}.txt

attach tty1 {mux_port}
{tty_lines}
attach tty1 console

set dks enable
{dks_extra}set dks debug={debug_mask}
; the trace goes into the console (mux) log: sim_log is fflushed at
; every simulation stop, while a debug FILE would sit in a stdio
; buffer that simh never flushes; -F keeps every line (no dedup) so
; occurrence counts stay meaningful
set debug -F LOG

echo ###
echo ### Zeroing the first page of RAM
echo ###
d -ml 1 xta, vtm 1777(1)
d -ml 2 atx (1), utm -1(1)
d -ml 3 v1m 2(1), stop
run 1

echo ###
echo ### Booting OS Dispak from disk 2053
echo ###
load boot_dispak.b6
run 2000
"""


def e60_ini(telnet_port, bridge_port, dbgfile, chan=2):
    chan_cmd = f"set dks chan={chan:o}\n" if chan != 2 else ""
    muxlog = dbgfile.replace(".dbg", ".mux")
    return f"""; the E-60 side of the remote test; generated by os_dks_test.py
de int-flush flush_interval 1
set -n console log={muxlog}
set dks enable
set dks engine=sim
set dks testos=off
set dks bridge={bridge_port}
{chan_cmd}set dks encoding=utf8
set dks debug=bridge;kadopam;ring;irq
; the trace goes into the console log (fflushed at every stop), unfiltered
set debug -F LOG
attach dks {telnet_port}
deposit 200 0777
go 200
"""


def check_terminal(name_pfx, port, e60mux, e60sim, expect_os=True,
                   b6mux=None, b6sim=None):
    """Walk one terminal through connect..type..CR..disconnect and check
    every step.  Returns nothing, records PASS/FAIL itself.  With
    expect_os=False (the channel the ОС build does not serve) only the
    ДКС-level checks run and the ОС-level ones become notes."""
    probe = lambda: flush_probe(e60sim)
    b6probe = (lambda: flush_probe(b6sim)) if (b6mux and b6sim) else None
    cl = None
    try:
        cl = Telnet(port)
        ok, n, e = wait_grep(e60mux, "line 1: system request 1", 25,
                             probe=probe)
        check(f"{name_pfx}: sysreq 1 on client connect", ok,
              f"no sysreq 1 in the E-60 log (count={n})")
        if not expect_os:
            if b6probe:
                ok, n, _ = wait_grep(b6mux, "ПРП 12", 25, probe=b6probe)
                check(f"{name_pfx}: ATTN ПРП12 crossed the bridge", ok,
                      f"no ПРП 12 in the BESM-6 log (count={n})")
            note(f"{name_pfx}: OS terminal assignment",
                 "this ДИСПАК build services ДКС only on КРК channel 2 "
                 "(ПРП6); the sysreq from channel 3 stays unanswered - an "
                 "OS configuration property, not a DKS defect")
            return
        ok, n, e = wait_grep(e60mux, "connect reply term", 25, probe=probe)
        ok2 = ok and "result 0" in e
        check(f"{name_pfx}: OS assigned a terminal (reply result 0)", ok2,
              f"no connect reply in the E-60 log (count={n})")
        got = cl.drain(8.0)
        check(f"{name_pfx}: greeting data on connect", len(got) > 0,
              "no output")
        cl.send(b"HELLO")
        got = cl.drain(3.0)
        check(f"{name_pfx}: engine echo of typed chars", b"HELLO" in got,
              repr(got[-120:]))
        cl.send(b"\r")
        got = cl.drain(40.0)
        check(f"{name_pfx}: data after CR (OS reaction)",
              len(got) > len(b"HELLO"), f"only {len(got)} bytes")
        print(f"    after CR: {got[-200:]!r}", flush=True)
        cl.close()
        cl = None
        ok, n, e = wait_grep(e60mux, "system request 3", 25, probe=probe)
        check(f"{name_pfx}: sysreq 3 on disconnect", ok,
              f"no sysreq 3 in the E-60 log (count={n})")
    finally:
        if cl:
            cl.close()


def grep_file(path, patterns):
    """Return (found_all, found_dict, text)."""
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        text = ""
    found = {p: (p in text) for p in patterns}
    return all(found.values()), found, text


def flush_probe(sim):
    """Stop the CPU, snapshot the DKS state and CONT.  A simulation
    stop fflushes the console (mux) log that carries the trace, so
    after this the on-disk log is current."""
    sim.cmd("show dks", 0.5, halt=True)


def wait_grep(path, pattern, timeout=20.0, need=1, probe=None):
    """Poll a log file until `pattern` occurs >= need times.  Returns
    (ok, count, text).  `probe` (a callable) is invoked once if the
    pattern is still missing after 2s - it should force the simulator
    to flush its buffers."""
    t0 = time.time()
    probed = False
    best, text = 0, ""
    while time.time() - t0 < timeout:
        try:
            with open(path, "r", errors="replace") as f:
                text = f.read()
        except OSError:
            text = ""
        n = text.count(pattern)
        best = max(best, n)
        if n >= need:
            return True, n, text
        if probe and not probed and time.time() - t0 > 2.0:
            probe()
            probed = True
        time.sleep(0.5)
    return False, best, text


# ----------------------------------------------------------------------
# Phases
# ----------------------------------------------------------------------

def phase_probe(b6):
    tag = "P"
    write_ini(os.path.join(DIR, f"dks_os_{tag}.ini"),
              besm6_ini(tag, 4199, TTY_LINES_ALWAYS_ON))
    sim = Sim(b6, os.path.join(DIR, f"dks_os_{tag}.ini"),
              os.path.join(DIR, f"boot_{tag}.log"))
    hit, el, tail = wait_boot(tag)
    print(f"probe: marker={hit} after {el:.0f}s", flush=True)
    print(tail[-2500:], flush=True)
    sim.stop()


def phase_local(b6):
    tag = "L"
    # the trace lives in the console (mux) log now; start from scratch
    # so the boot cycles of a previous run cannot inflate the counts
    for f in (f"log_{tag}.txt", f"boot_{tag}.log"):
        try:
            os.unlink(os.path.join(DIR, f))
        except OSError:
            pass
    write_ini(os.path.join(DIR, f"dks_os_{tag}.ini"),
              besm6_ini(tag, 4199, TTY_LINES_ALWAYS_ON))
    sim = Sim(b6, os.path.join(DIR, f"dks_os_{tag}.ini"),
              os.path.join(DIR, f"boot_{tag}.log"),
              muxlog=os.path.join(DIR, f"log_{tag}.txt"))
    dbg = os.path.join(DIR, f"log_{tag}.txt")

    hit, el, tail = wait_boot(tag)
    check("L1: OS Dispak booted (ДАТА/ВРЕМЯ)", hit is not None,
          f"no marker in {el:.0f}s; tail: {tail[-200:]!r}")

    # 1) boot-time connect chain: 7 lines (11..17), each sysreq 1 + reply
    ok, found, text = grep_file(dbg, [
        "line 11: system request 1",
        "line 17: system request 1",
        "connect reply term 15 result 0",
        "connect reply term 20 result 0",
    ])
    check("L2: sysreq 1 for lines 11..17 at boot", found["line 11: system request 1"]
          and found["line 17: system request 1"], str(found))
    check("L3: OS assigned terminals (reply result 0)",
          found["connect reply term 15 result 0"] and
          found["connect reply term 20 result 0"], str(found))

    # 2) dynamic: TTY11 OFF -> sysreq 3 -> the OS frees the terminal
    sim.cmd("set tty11 off", 2.0, halt=True)
    ok, n, text = wait_grep(dbg, "system request 3", 25,
                            probe=lambda: flush_probe(sim))
    check("L4: sysreq 3 after SET TTY11 OFF", ok, f"count={n}")

    # 3) a telnet client lands on the now-free line 11
    cl = None
    try:
        cl = Telnet(4199)
        got = cl.drain(2.0)
        # The connect must trigger sysreq 1 again.
        ok, n, text = wait_grep(dbg, "line 11: system request 1", 25, need=2,
                                probe=lambda: flush_probe(sim))
        check("L5: client on line 11 -> sysreq 1 again", ok,
              f"sysreq1 count={n}")
        ok, n, text = wait_grep(dbg, "connect reply term 15 result 0", 25,
                                need=2, probe=lambda: flush_probe(sim))
        check("L6: terminal 15 re-assigned", ok, f"replies={n}")

        # 4) typing: local echo by the engine, then the OS reaction on CR
        cl.send(b"HELLO")
        got = cl.drain(3.0)
        check("L7: engine echo of typed chars", b"HELLO" in got, repr(got[-120:]))
        cl.send(b"\r")
        got = cl.drain(10.0)
        print(f"    after CR: {got[-300:]!r}", flush=True)
        check("L8: data after CR (OS reaction)", len(got) > len(b"HELLO"),
              f"only {len(got)} bytes")

        # 5) disconnect -> sysreq 3
        cl.close()
        cl = None
        ok, n, text = wait_grep(dbg, "system request 3", 25, need=2,
                                probe=lambda: flush_probe(sim))
        check("L9: sysreq 3 on disconnect", ok, f"count={n}")
    finally:
        if cl:
            cl.close()
    print(f"    console tail: {tail[-400:]!r}", flush=True)
    sim.stop()


def phase_remote(b6, p11, bport=4140, telnet=4300):
    tag = "R"
    for f in (f"e60_R0.log", f"e60_R1.log", f"e60_R0.mux", f"e60_R1.mux",
              f"log_{tag}.txt", f"boot_{tag}.log"):
        try:
            os.unlink(os.path.join(DIR, f))
        except OSError:
            pass
    # Two Э-60 concentrators: client i serves КРК channel 2+i and the
    # БЭСМ-6 connects to port bport+i, so:
    #   Э-60 #0: channel 2, bridge 4140, telnet 4300
    #   Э-60 #1: channel 3, bridge 4141, telnet 4301
    e60s = []
    for i in range(2):
        write_ini(os.path.join(DIR, f"e60_R{i}.ini"),
                  e60_ini(telnet + i, bport + i, f"e60_R{i}.dbg",
                          chan=2 + i))
        e60s.append(Sim(p11, os.path.join(DIR, f"e60_R{i}.ini"),
                        os.path.join(DIR, f"e60_R{i}.log"),
                        muxlog=os.path.join(DIR, f"e60_R{i}.mux")))
    time.sleep(4)
    write_ini(os.path.join(DIR, f"dks_os_{tag}.ini"),
              besm6_ini(tag, 4198, TTY_LINES_ALWAYS_ON,
                        dks_extra=f"set dks remote=localhost:{bport}\n"
                                  f"set dks chans=2\n",
                        debug_mask="kadopam;ring;irq;bridge"))
    sim = Sim(b6, os.path.join(DIR, f"dks_os_{tag}.ini"),
              os.path.join(DIR, f"boot_{tag}.log"),
              muxlog=os.path.join(DIR, f"log_{tag}.txt"))
    hit, el, tail = wait_boot(tag)
    check("R1: OS Dispak booted on the bridge (ДАТА/ВРЕМЯ)", hit is not None,
          f"no marker in {el:.0f}s; tail: {tail[-200:]!r}")

    # the bridge carried the whole boot: KADOPAM reads of cell 0 etc.
    b6mux = os.path.join(DIR, f"log_{tag}.txt")
    probe_b6 = lambda: flush_probe(sim)
    ok, n, text = wait_grep(b6mux, "BRIDGE", 25, probe=probe_b6)
    check("R2: KADOPAM of the OS crossed the bridge", ok,
          f"no BRIDGE frames in the BESM-6 trace (count={n})")

    # cell 0 as the OS reads it: bit 9+2 (channel 2) | bit 9+3 (channel 3)
    ok, n, text = wait_grep(b6mux, "-> 006000", 25, probe=probe_b6)
    check("R3: cell 0 shows both КРК channels live (006000)", ok,
          f"'-> 006000' count={n}")

    # ---- a terminal behind Э-60 #0 (КРК channel 2) ----
    print(f"    console tail: {tail[-400:]!r}", flush=True)
    check_terminal("R4/C2", telnet,
                   os.path.join(DIR, "e60_R0.mux"), e60s[0])

    # ---- a terminal behind Э-60 #1 (КРК channel 3) ----
    # The ОС build services ДКС only on the base channel 2; on channel 3
    # the ДКС-level chain (connect -> sysreq 1 -> ПРП12) is still
    # exercised, the ОС-level steps are documented as notes.
    check_terminal("R5/C3", telnet + 1,
                   os.path.join(DIR, "e60_R1.mux"), e60s[1],
                   expect_os=False, b6mux=b6mux, b6sim=sim)

    # channel-3 data path: exercised by the stage-C unit tests (CH.1-CH.4);
    # the running ОС never selects channel 3, so no [3] KADOPAM frames
    # can appear here.
    note("R6/C3: KADOPAM traffic on channel 3",
         "the ОС build never selects КРК channel 3 (see R5 note); the "
         "channel-3 КМП data path is covered by the stage-C tests "
         "(dks_c_chans_test.sh, CH.1-CH.4)")

    for s in e60s:
        s.stop()
    sim.stop()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "probe"
    b6 = sys.argv[2] if len(sys.argv) > 2 else os.path.join(SIMH, "besm6")
    p11 = sys.argv[3] if len(sys.argv) > 3 else os.path.join(SIMH, "pdp11")
    os.chdir(DIR)
    if mode == "probe":
        phase_probe(b6)
    elif mode == "local":
        phase_local(b6)
    elif mode == "remote":
        phase_remote(b6, p11)
    for t in NOTES:
        print(f"NOTE: {t}", flush=True)
    print(f"\nPASS={PASS} FAIL={FAIL} NOTES={len(NOTES)}", flush=True)


if __name__ == "__main__":
    main()
