#!/usr/bin/env python3
"""Full-SIMH acceptance test: complex Fortran figure through the real GRP 18 path.

gp-fig-deck.b6 computes an octagon plus a bounding rectangle (14 frames,
12 pen-down segments) into COMMON /PDEVXY/ FRAME(64) and an ALL(64,14) table.
DISPAK clears the user area when the job ends, so the runner reads the X/Y/P
arrays from the deck source and re-materializes those very frames as a .b6
image at 040160 in guest memory.  It then loads gp-fig-handler.b6 at 077700,
points the обвнп1+30 vector cell 04023 at 077710, patches the ПОДПРГ/ГАШРП1
constants 01457/01460 and raises the 18Р line with `set gp testirq`.

The CPU takes the real internal interrupt: ВНУПР at 0500, DISPAK clears ГРП
and dispatches through M[04023] to the guest handler.  The handler streams
each 64-word frame to УВВ 0172, i.e. gp_data_write() calls benson_frame()
from inside the interrupt, and then returns with выпр.  A done flag absorbs
the extra line that the stream raises on its own.  No decoder call is made
from the host.  (ЭК 01211 is not used: cmd_033() masks its address with
04377, which makes the 01210/01211 case labels unreachable in this build.)

The runner checks the ACPU marker, the interrupt trace, the three golden
SHA256 values and the SVG path count.  All scratch files stay under /var/tmp.

Environment:
  SIMH_BIN     simulator binary with `set gp testirq` (default BESM6/BIN/besm6)
  NITI_2348    disk image with the GRAFOR port (default ~/.besm6/2348)
  TIMEOUT      seconds to wait for boot and for the job (default 180)
"""
import argparse
import hashlib
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
DECK = Path(os.environ.get("GP_FIG_DECK", str(TESTS / "gp-fig-deck.b6")))
HANDLER = TESTS / "gp-fig-handler.b6"
EMU = Path(os.environ.get("SIMH_BIN", str(SIMH / "BIN/besm6")))
DISK = Path(os.environ.get("NITI_2348", str(Path.home() / ".besm6/2348")))
BOOT = re.compile(r"[ДD]ATA:")
# Digits only: the ACPU printer maps Latin letters to Cyrillic homoglyphs.
ACPU_MARKER = "14 12 3200 500 777 12345"
FRAME_COUNT = 14
VECTOR_CELL = 0o04023
HANDLER_ADDR = 0o077710
DONE_CELL = 0o077700
COUNT_CELL = 0o077701
BASE_CELL = 0o077702
# Free DISPAK words used for the 14 x 64-word frame table.
TABLE_BASE = 0o040160
# FOREX INTEGER tag (064<<42), exactly as in besm6_gp_decoder.c's selftest.
TAG = 0o64 << 42
FIRST_WORD = TAG | 3200
TRACE_AREA = re.compile(r"^\s*0?4023:\s+([0-7 ]+)")

# Golden values are filled from the first verified run and then kept fixed.
EXPECTED = {
    "plot.dat": "ba6c96fb7330c6800d10d738c95e19811a79ccbf939281fc1ef6d0fb29727a77",
    "benson.out": "56948a801159cbe93891820650df7d840004fd1830443e663cf30bdc028e72a0",
    "benson.svg": "c39af7ec38766a5df151936071edf14ab4ae3b01f486af1ab184f703537e7e55",
}
EXPECTED_PATHS = 13


def tmux(env, *args, check=True):
    r = subprocess.run(["tmux", *args], text=True, capture_output=True, env=env)
    if check and r.returncode:
        raise RuntimeError("tmux %s: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


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
        "set gp plot=on",
        "set gp debug=IRQ",
        "set gp debug=REG",
        "set gp debug=FRAME",
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


def parse_words(text):
    """Read `addr: 0000 0000 ...` lines and return {addr: word}."""
    words = {}
    last = None
    for line in text.splitlines():
        m = re.match(r"^\s*([0-7]+):\s+([0-7]{1,4}(?:\s+[0-7]{1,4}){0,7})\s*$",
                     line)
        if m:
            last = int("".join(m.group(2).split()), 8)
            words[int(m.group(1), 8)] = last
            continue
        m = re.match(r"^\s*([0-7]+):\s+thru\s+([0-7]+):\s+same as above\.",
                     line)
        if m and last is not None:
            for a in range(int(m.group(1), 8), int(m.group(2), 8) + 1):
                words[a] = last
    return words


def parse_figure():
    """Read X/Y/P from the deck source: the single definition of the figure."""
    arrays = {"X": {}, "Y": {}, "P": {}}
    pattern = re.compile(r"^\s*([XYP])\((\d+)\)\s*=\s*(\d+)\s*$", re.I)
    for line in DECK.read_text(encoding="utf-8").splitlines():
        m = pattern.match(line)
        if m:
            arrays[m.group(1).upper()][int(m.group(2))] = int(m.group(3))
    for key in ("X", "Y", "P"):
        if sorted(arrays[key]) != list(range(1, FRAME_COUNT + 1)):
            raise ValueError("gp-fig, bad %s array in deck" % key)
    return [arrays[k] for k in ("X", "Y", "P")]


def make_frame_image(work, x, y, p):
    """Write the 14 tagged FOREX frames as a loadable .b6 memory image.

    The Fortran deck computes these very frames into COMMON /PDEVXY/ and
    ALL(64,14).  DISPAK clears the user area when the job ends, so the runner
    re-materializes the same words in guest memory before raising the line.
    """
    lines = ["в %o" % TABLE_BASE]
    for j in range(1, FRAME_COUNT + 1):
        lines.append("с %016o" % (TAG | x[j]))
        lines.append("с %016o" % (TAG | y[j]))
        lines.append("с %016o" % (TAG | p[j]))
        for _ in range(61):
            lines.append("с %016o" % TAG)
    image = work / "gp-fig-frames.b6"
    image.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return image


def verify_frame_base(send, pane):
    """Confirm the materialized first frame really sits at TABLE_BASE."""
    send("ex %o/4" % TABLE_BASE)
    time.sleep(0.5)
    vals = parse_words(pane(history=30))
    words = [vals.get(TABLE_BASE + i) for i in range(4)]
    if (words[0] == FIRST_WORD and words[3] == TAG and
            words[1] is not None and 0 <= (words[1] & 0xffffff) <= 7500):
        return TABLE_BASE
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int,
                    default=int(os.environ.get("TIMEOUT", "180")))
    a = ap.parse_args()

    for p in (EMU, BASE_INI, DECK, HANDLER, DISK):
        if not p.exists():
            print("FAIL: gp-fig, missing %s" % p, file=sys.stderr)
            return 2

    work = Path(tempfile.mkdtemp(prefix="gp-fig-", dir="/var/tmp"))
    ini = make_ini(work)
    env = dict(os.environ, TMUX_TMPDIR=str(work / "tmux"))
    session = "gp-fig-%d" % os.getpid()
    print("work   : %s" % work, flush=True)
    print("binary : %s" % EMU, flush=True)

    tmux(env, "new-session", "-d", "-s", session, "-x", "200", "-y", "50",
         "cd %s && BESM6_NITI_USER=1 %s %s" % (work / "run", EMU, ini))

    def pane(history=2000):
        return tmux(env, "capture-pane", "-p", "-S", str(-history),
                    "-t", session, check=False)

    def send(text):
        tmux(env, "send-keys", "-t", session, "-l", text)
        tmux(env, "send-keys", "-t", session, "Enter")

    ok = True
    try:
        deadline = time.time() + a.timeout
        while time.time() < deadline:
            if BOOT.search(pane()):
                break
            time.sleep(0.5)
        else:
            print("FAIL: gp-fig, no ДATA: boot marker", flush=True)
            return 1
        time.sleep(5)
        # The DISPAK operator dialogue must reach the same state as in
        # run-gp-star.py before the card reader is attached.
        for cmd in ("ДАЙ А0", "ДАЙ А1", "НОМВ"):
            send(cmd)
            time.sleep(1.5)

        # DISPAK stages one job ahead.  The first submission can run a
        # leftover deck while ours waits in the queue, so resubmit until
        # our own ACPU marker is printed.  Reset ACPU files before the
        # final submission to avoid accepting a stale listing.
        def acpu_has_marker():
            for name in ("output.txt", "output1.txt"):
                p = work / name
                if p.exists() and ACPU_MARKER in p.read_text(
                        encoding="utf-8", errors="replace"):
                    return True
            return False

        def wait_sim():
            tmux(env, "send-keys", "-t", session, "C-g", check=False)
            for _ in range(20):
                if re.search(r"sim>\s*$", pane(history=20)):
                    return
                time.sleep(0.5)

        def reset_acpu():
            for cmd in ("detach prn0", "detach prn1",
                        "attach -n prn0 %s" % (work / "output.txt"),
                        "attach -n prn1 %s" % (work / "output1.txt")):
                send(cmd)
                time.sleep(0.3)

        ready = False
        for attempt in range(3):
            wait_sim()
            reset_acpu()
            for cmd in ("at vu0 %s" % DECK, "d 6 1", "d 5 10",
                        "set cpu req"):
                send(cmd)
                time.sleep(0.6)
            send("go")
            deadline = time.time() + a.timeout
            while time.time() < deadline:
                if acpu_has_marker():
                    ready = True
                    break
                if pane().rstrip().endswith("sim>"):
                    send("go")
                time.sleep(0.8)
            if ready:
                break
        # The deck ends in a self-loop, so its data area survives while we
        # take over with the interrupt.  Give the program time to fill it.
        time.sleep(3)
        (work / "console.txt").write_text(pane(), encoding="utf-8")
        if not ready:
            print("FAIL: gp-fig, ACPU marker not seen", flush=True)
            return 1

        tmux(env, "send-keys", "-t", session, "C-g")
        for _ in range(20):
            if re.search(r"sim>\s*$", pane(history=20)):
                break
            time.sleep(0.5)

        # `load` resets PC to 1, so save and restore the live DISPAK PC.
        pc = None
        for _ in range(10):
            send("ex PC")
            time.sleep(0.3)
            line = last_match(pane(), re.compile(r"PC:\s+[0-7]+"))
            if line:
                pc = re.search(r"PC:\s+([0-7]+)", line).group(1)
                break
        if not pc:
            print("FAIL: gp-fig, cannot read PC", flush=True)
            return 1

        x, y, p = parse_figure()
        frame_image = make_frame_image(work, x, y, p)

        send("load %s" % HANDLER)
        time.sleep(0.6)
        send("load %s" % frame_image)
        time.sleep(1.5)
        send("d PC %s" % pc)
        time.sleep(0.3)

        # Preserve the обвнп1+30 command word, change only its 15-bit address.
        cell = None
        for _ in range(10):
            send("ex %o" % VECTOR_CELL)
            time.sleep(0.3)
            cell = last_match(pane(), TRACE_AREA)
            if cell:
                break
        if not cell:
            print("FAIL: gp-fig, cannot read vector cell 04023", flush=True)
            return 1
        groups = re.findall(r"[0-7]{4}", TRACE_AREA.search(cell).group(1))
        if len(groups) != 4:
            print("FAIL: gp-fig, bad vector cell dump", flush=True)
            return 1
        old_word = int("".join(groups), 8)
        new_word = (old_word & ~0o77777) | HANDLER_ADDR
        print("vector 04023: %016o -> %016o" % (old_word, new_word),
              flush=True)
        send("d %o %016o" % (VECTOR_CELL, new_word))
        time.sleep(0.3)

        # disp70 constants keep bit 18 out of ВНУПР's mask and preserve it in
        # ГАШРП1; patch both so the line is really cleared on this path.
        send("d 01457 0000000036774000")
        time.sleep(0.3)
        send("d 01460 7777777701003000")
        time.sleep(0.3)

        base = verify_frame_base(send, pane)
        if base != TABLE_BASE:
            print("FAIL: gp-fig, frame table at %s, expected %05o" %
                  (base, TABLE_BASE), flush=True)
            ok = False
        else:
            print("frame table base: %05o" % base, flush=True)
            send("d %o %o" % (COUNT_CELL, FRAME_COUNT))
            time.sleep(0.3)
            send("d %o %o" % (BASE_CELL, base))
            time.sleep(0.3)
            send("d %o 0" % DONE_CELL)
            time.sleep(0.3)
            send("d GRP 0")
            time.sleep(0.3)
            send("set gp tv67")
            time.sleep(0.3)
            send("set gp testirq")
            time.sleep(0.3)
            send("go")

            trace = work / "debug.log"
            deadline = time.time() + 60
            delivered = False
            while time.time() < deadline:
                text = trace.read_text(encoding="utf-8",
                                       errors="replace") if trace.exists() else ""
                if (text.count("decoder rc=0") >= FRAME_COUNT and
                        "TRACE ВЫПР interrupt return: a=00077747" in text):
                    delivered = True
                    break
                if re.search(r"sim>\s*$", pane(history=20)):
                    break
                time.sleep(0.3)
            tmux(env, "send-keys", "-t", session, "C-g")
            time.sleep(0.5)
            # benson_finish() has no simulator shutdown hook in this build;
            # reset gp calls it, which flushes and renames calcomp.svg.
            send("reset gp")
            time.sleep(1.0)
            if not delivered:
                print("FAIL: gp-fig, frame delivery did not finish in interrupt",
                      flush=True)
                ok = False
    finally:
        tmux(env, "kill-session", "-t", session, check=False)

    trace = work / "debug.log"
    lines = []
    if trace.exists():
        lines = [ln for ln in trace.read_text(encoding="utf-8",
                                              errors="replace").splitlines()
                 if ("TRACE" in ln or "18Р line raised" in ln or
                     "decoder rc=" in ln)]
    print("=== interrupt trace ===", flush=True)
    for ln in lines[:12]:
        print(ln, flush=True)
    frames = sum("decoder rc=0" in ln for ln in lines)
    entered = any("TRACE GRP18 -> op_int_1 0500" in ln for ln in lines)
    guest = any("TRACE ВЫПР interrupt return: a=00077747" in ln
                for ln in lines)
    print("delivered=%d entered=%s guest_return=%s" %
          (frames, entered, guest), flush=True)
    if frames < FRAME_COUNT or not entered or not guest:
        ok = False

    acpu = []
    for name in ("output.txt", "output1.txt"):
        p = work / name
        if p.exists():
            acpu.append(p.read_text(encoding="utf-8", errors="replace"))
    acpu_ok = any(ACPU_MARKER in text for text in acpu)
    print("=== ACPU ===", flush=True)
    for text in acpu:
        for ln in text.splitlines():
            if ACPU_MARKER in ln:
                print(ln.rstrip(), flush=True)
    print("acpu marker: %s" % acpu_ok, flush=True)
    if not acpu_ok:
        ok = False

    for name, want in EXPECTED.items():
        p = work / "run" / name
        if not p.exists():
            print("FAIL: gp-fig, missing %s" % name, flush=True)
            ok = False
            continue
        got = sha256(p)
        print("%-12s %8d B  sha256=%s" % (name, p.stat().st_size, got),
              flush=True)
        if want and got != want:
            print("FAIL: gp-fig, %s SHA mismatch, want %s" % (name, want),
                  flush=True)
            ok = False

    svg = work / "run" / "benson.svg"
    if svg.exists():
        text = svg.read_text(encoding="utf-8")
        paths = text.count("<path")
        sizes = re.search(r'width="(\d+)" height="(\d+)"', text)
        print("svg          paths=%d size=%s bytes=%d" %
              (paths, sizes.groups() if sizes else "?",
               svg.stat().st_size), flush=True)
        if paths != EXPECTED_PATHS:
            print("FAIL: gp-fig, expected %d SVG paths" % EXPECTED_PATHS,
                  flush=True)
            ok = False
    else:
        ok = False

    print("PASS: gp-fig" if ok else "FAIL: gp-fig", flush=True)
    print("work dir kept at %s" % work, flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
