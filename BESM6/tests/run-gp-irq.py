#!/usr/bin/env python3
"""Autonomous DISPAK/GP interrupt-path acceptance test.

The test boots DISPAK under SIMH, installs a tiny guest handler in high
memory, points the обвнп1+30 vector cell (04023) at it, raises ГРП bit 18 with
`set gp testirq` and lets the CPU take the real internal interrupt.  It passes
only when the SIMH debug log shows the whole chain:

  18Р line raised
  TRACE GRP18 -> op_int_1 0500
  [TRACE РЕГ 037 GRP clear]
  TRACE ВЫПР interrupt return

The simulator binary must contain the `set gp testirq` MTAB entry.  Use
SIMH_BIN to point at a freshly built binary when BIN/besm6 is stale.
All scratch files stay under /var/tmp.

Environment:
  SIMH_BIN     simulator binary (default BESM6/BIN/besm6)
  NITI_2348    disk image with the GRAFOR port (default ~/.besm6/2348)
  TIMEOUT      seconds to wait for boot (default 120)
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SIMH = Path(__file__).resolve().parents[1]
BASE_INI = SIMH / "OSZAGR/dispak.ini"
TESTS = Path(__file__).resolve().parent
DECK = TESTS / "gp-irq-handler.b6"
EMU = Path(os.environ.get("SIMH_BIN", str(SIMH / "BIN/besm6")))
DISK = Path(os.environ.get("NITI_2348", str(Path.home() / ".besm6/2348")))
BOOT = re.compile(r"[ДD]ATA:")
VECTOR_CELL = 0o04023
HANDLER_ADDR = 0o077700
TRACE_AREA = re.compile(r"^\s*0?4023:\s+([0-7 ]+)")


def tmux(env, *args, check=True):
    r = subprocess.run(["tmux", *args], text=True, capture_output=True, env=env)
    if check and r.returncode:
        raise RuntimeError("tmux %s: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def make_ini(work):
    (work / "run").mkdir(parents=True)
    (work / "tmux").mkdir()
    sys2048 = Path("/usr/local/share/besm6/2148")
    disk = work / "2048"
    shutil.copy2(DISK, disk)
    compiler = work / "2148"
    shutil.copy2(sys2048, compiler)
    out = []
    for line in BASE_INI.read_text(encoding="utf-8").splitlines():
        if re.match(r"\s*attach\s+tty\d+\s+Line=", line):
            out.append("; " + line)
            continue
        if "console log=" in line:
            out.append("; " + line)
            continue
        if re.match(r"\s*attach\s+-n\s+(vu0|pi0|pl0)\b", line):
            out.append("; " + line)
            continue
        if "attach -e md41 " in line:
            out.append("attach -e md41 %s" % compiler)
            continue
        if re.match(r"\s*;?\s*attach\s+-e\s+md00\s+", line):
            out.append("attach -e md00 /usr/local/share/besm6/2053")
            continue
        if re.match(r"\s*attach\s+-e\s+md45\s+", line):
            out.append("attach -e md45 %s" % disk)
            continue
        if re.match(r"\s*attach\s+-e\s+md07\s+", line):
            out.append("attach -e md07 %s" % (SIMH / "backup/2253"))
            continue
        if "attach -n prn0 " in line:
            out.append("attach -n prn0 %s" % (work / "output.txt"))
            continue
        if "attach -n prn1 " in line:
            out.append("attach -n prn1 %s" % (work / "output1.txt"))
            continue
        # Read/write images are copied so a full DISPAK boot never dirties
        # the repository; read-only tape images can stay where they are.
        am = re.match(r"^(\s*attach\s+.*?\s+)(\S+)\s*$", line)
        if am and not re.search(r"\s-r\s", line):
            token = am.group(2)
            if token.startswith("~/Yandex.Disk/simh/"):
                src = Path.home() / token[2:]
            elif token.startswith(str(SIMH)):
                src = Path(token)
            else:
                src = None
            if src and src.is_file():
                dst = work / ("img-" + src.name)
                if not dst.exists():
                    shutil.copy2(src, dst)
                out.append(am.group(1) + str(dst))
                continue
        out.append(line)
    out += [
        "set gp debug=IRQ",
        "set debug %s" % (work / "debug.log"),
    ]
    ini = work / "simh.ini"
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")
    return ini


def last_match(text, pattern):
    found = None
    for line in text.splitlines():
        if pattern.search(line):
            found = line
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int,
                    default=int(os.environ.get("TIMEOUT", "120")))
    a = ap.parse_args()

    for p in (EMU, BASE_INI, DECK, DISK):
        if not p.exists():
            print("FAIL: gp-irq, missing %s" % p, file=sys.stderr)
            return 2

    work = Path(tempfile.mkdtemp(prefix="gp-irq-", dir="/var/tmp"))
    ini = make_ini(work)
    env = dict(os.environ, TMUX_TMPDIR=str(work / "tmux"))
    session = "gp-irq-%d" % os.getpid()
    print("work   : %s" % work, flush=True)
    print("binary : %s" % EMU, flush=True)

    tmux(env, "new-session", "-d", "-s", session, "-x", "200", "-y", "50",
         "cd %s && BESM6_NITI_USER=1 %s %s" % (work / "run", EMU, ini))

    def pane(history=4000):
        return tmux(env, "capture-pane", "-p", "-S", str(-history),
                    "-t", session, check=False)

    def send(text):
        tmux(env, "send-keys", "-t", session, "-l", text)
        tmux(env, "send-keys", "-t", session, "Enter")

    ok = True
    try:
        deadline = time.time() + a.timeout
        booted = False
        while time.time() < deadline:
            if BOOT.search(pane()):
                booted = True
                break
            time.sleep(0.5)
        if not booted:
            print("FAIL: gp-irq, no ДATA: boot marker", flush=True)
            return 1
        time.sleep(5)
        send("ДАЙ А0")
        time.sleep(1.5)

        for _ in range(5):
            tmux(env, "send-keys", "-t", session, "C-g")
            for _ in range(20):
                if re.search(r"sim>\s*$", pane(history=20)):
                    break
                time.sleep(0.5)
            if re.search(r"sim>\s*$", pane(history=20)):
                break
        if not re.search(r"sim>\s*$", pane(history=20)):
            print("FAIL: gp-irq, no SIMH prompt after boot", flush=True)
            return 1

        # `load` resets PC to 1, so save and restore the live DISPAK PC.
        send("ex PC")
        time.sleep(0.4)
        pc_line = last_match(pane(), re.compile(r"PC:\s+[0-7]+"))
        if not pc_line:
            print("FAIL: gp-irq, cannot read PC", flush=True)
            return 1
        pc = re.search(r"([0-7]+)", pc_line.split(":", 1)[1]).group(1)

        send("load %s" % DECK)
        time.sleep(0.6)
        send("d PC %s" % pc)
        time.sleep(0.3)
        send("ex -m %05o" % HANDLER_ADDR)
        time.sleep(0.3)
        loaded = last_match(pane(), re.compile(r"^\s*77700:"))
        if not loaded or "выпр" not in loaded:
            print("FAIL: gp-irq, guest handler not loaded at 077700", flush=True)
            ok = False

        # Preserve the обвнп1+30 command word, replace only its 15-bit address.
        send("ex %o" % VECTOR_CELL)
        time.sleep(0.4)
        cell = last_match(pane(), TRACE_AREA)
        if not cell:
            print("FAIL: gp-irq, cannot read vector cell 04023", flush=True)
            return 1
        groups = re.findall(r"[0-7]{4}", TRACE_AREA.search(cell).group(1))
        if len(groups) != 4:
            print("FAIL: gp-irq, bad vector cell dump", flush=True)
            return 1
        old_word = int("".join(groups), 8)
        new_word = (old_word & ~0o77777) | HANDLER_ADDR
        print("vector 04023: %016o -> %016o" % (old_word, new_word), flush=True)
        send("d %o %016o" % (VECTOR_CELL, new_word))
        time.sleep(0.3)

        # disp70 constants at 01457/01460 omit bit 18 from ПОДПРГ and
        # preserve it in ГАШРП1.  Patch both so ВНУПР takes the internal
        # path and clears the line, as the GRP18 handler expects.
        send("d 01457 0000000036774000")
        time.sleep(0.3)
        send("d 01460 7777777701003000")
        time.sleep(0.3)

        # Isolate bit 18 so DISPAK does not service an unrelated source first.
        send("d GRP 0")
        time.sleep(0.3)
        send("set gp tv67")
        time.sleep(0.3)
        send("set gp testirq")
        time.sleep(0.3)
        send("go")

        trace = work / "debug.log"
        deadline = time.time() + 30
        complete = False
        while time.time() < deadline:
            text = trace.read_text(encoding="utf-8", errors="replace") \
                if trace.exists() else ""
            if ("18Р line raised" in text and
                    "TRACE GRP18 -> op_int_1 0500" in text and
                    "TRACE ВЫПР interrupt return" in text):
                complete = True
                break
            if re.search(r"sim>\s*$", pane(history=20)):
                break
            time.sleep(0.2)

        tmux(env, "send-keys", "-t", session, "C-g")
        time.sleep(0.5)
        (work / "console.txt").write_text(pane(), encoding="utf-8")
        if not complete:
            print("FAIL: gp-irq, no complete interrupt chain in debug log",
                  flush=True)
            ok = False
    finally:
        tmux(env, "kill-session", "-t", session, check=False)

    trace = work / "debug.log"
    lines = []
    if trace.exists():
        lines = [ln for ln in trace.read_text(encoding="utf-8",
                                              errors="replace").splitlines()
                 if ("TRACE" in ln or "18Р line raised" in ln)]
    print("=== interrupt trace ===", flush=True)
    for ln in lines[:8]:
        print(ln, flush=True)
    if len(lines) > 8:
        print("... %d more lines" % (len(lines) - 8), flush=True)

    joined = "\n".join(lines)
    raised = "18Р line raised" in joined
    entered = "TRACE GRP18 -> op_int_1 0500" in joined
    cleared = "TRACE РЕГ 037 GRP clear" in joined
    returned = "TRACE ВЫПР interrupt return" in joined
    if not (raised and entered and returned):
        ok = False
    print("raised=%s entered=%s cleared=%s returned=%s" %
          (raised, entered, cleared, returned), flush=True)
    guest = any("ВЫПР interrupt return: a=00077700" in ln for ln in lines)
    print("guest handler reached: %s" % guest, flush=True)
    print("PASS: gp-irq" if ok else "FAIL: gp-irq", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
