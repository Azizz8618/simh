#!/usr/bin/env python3
"""Full-SIMH plotter acceptance test: Fortran star card deck to BENSON SVG.

The deck gp-star.b6 issues EK 50 1210/1211 through an inline MADLEN PDEV.
This runner boots the DISPAK OS under SIMH, feeds the deck on vu0, captures
the ACPU listing and verifies the three golden SHA256 values.

Environment:
  SIMH_BIN     simulator binary (default BESM6/BIN/besm6)
  NITI_2348    disk image with the GRAFOR port (default ~/.besm6/2348)
  TIMEOUT      seconds to wait for boot and for the job (default 120)
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
BASE_INI = Path(os.environ.get("GPSTAR_BASE_INI",
                               str(SIMH / "OSZAGR/dispak.ini")))
DECK = Path(os.environ.get("GPSTAR_DECK",
                          str(Path(__file__).resolve().parent / "gp-star.b6")))
EMU = Path(os.environ.get("SIMH_BIN", str(SIMH / "BIN/besm6")))
DISK = Path(os.environ.get("NITI_2348", str(Path.home() / ".besm6/2348")))
BOOT = re.compile(r"[ДD]ATA:")
DONE = re.compile(r"КОНЕЦ ЗАДАЧИ|[КK]З\s*\(")

EXPECTED = {
    "plot.dat": "d3858dcd9fa441b7e5fce367e86b075a2d9aeca0736d8ef6dfdb024f2d9e9f0f",
    "benson.out": "0815857d6a565f2bccf064b8e6ec6814198b1d4875bb75e3b0a69aba2fcf7bc2",
    "benson.svg": "46d97ab50a23647ef2b41d9cd424d6f611c5ec4609004050ce7c0a8c75e181f9",
}
ACPU_MARKER = "GРSТАR: 10 FRАМЕS DОNЕ"


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
        if re.match(r"\s*attach\s+-n\s+vu0\b", line):
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
        out.append(line)
    out += [
        "attach gp0 %s" % (work / "run/gp-stream.dat"),
        "set gp plot=on",
        "set gp debug",
        "set debug %s" % (work / "debug.log"),
    ]
    if os.environ.get("GPSTAR_CPU_DEBUG"):
        out += ["set cpu debug", "set console log=%s" % (work / "cpu.log")]
    ini = work / "simh.ini"
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")
    return ini


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int,
                    default=int(os.environ.get("TIMEOUT", "120")))
    a = ap.parse_args()

    for p in (EMU, BASE_INI, DECK, DISK):
        if not p.exists():
            print("FAIL: missing %s" % p, file=sys.stderr)
            return 2
    work = Path(tempfile.mkdtemp(prefix="gp-star-", dir="/var/tmp"))
    ini = make_ini(work)
    env = dict(os.environ, TMUX_TMPDIR=str(work / "tmux"))
    session = "gp-star-%d" % os.getpid()
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
        booted = False
        while time.time() < deadline:
            if BOOT.search(pane()):
                booted = True
                break
            time.sleep(0.5)
        if not booted:
            print("FAIL: no ДATA: boot marker", flush=True)
            return 1
        time.sleep(5)
        for cmd in ("ДАЙ А0", "ДАЙ А1"):
            send(cmd)
            time.sleep(1.5)
        send("НОМВ")
        time.sleep(1.5)
        tmux(env, "send-keys", "-t", session, "C-g")
        for _ in range(20):
            if re.search(r"sim>\s*$", pane(history=20)):
                break
            time.sleep(0.5)
        # DISPAK stages one job ahead and a stale КЗ(...) from a system
        # task is still in the pane.  Wait for OUR ACPU marker only, and
        # reset the ACPU files before each submission so a stale listing
        # cannot be mistaken for this job.
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

        finished = False
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
                    finished = True
                    break
                if pane().rstrip().endswith("sim>"):
                    send("go")
                time.sleep(0.8)
            if finished:
                break
        (work / "console.txt").write_text(pane(), encoding="utf-8")
        if not finished:
            print("FAIL: own ACPU marker not seen", flush=True)
            ok = False
    finally:
        tmux(env, "kill-session", "-t", session, check=False)

    trace = work / "debug.log"
    if trace.exists():
        lines = [ln for ln in trace.read_text(encoding="utf-8",
                                              errors="replace").splitlines()
                 if ("TRACE" in ln or "18Р" in ln or "РЕГ 03" in ln or
                     "ВЫПР" in ln or "vector 04023" in ln)]
        print("=== interrupt trace ===", flush=True)
        for ln in lines[-40:]:
            print(ln, flush=True)

    acpu = []
    for name in ("output.txt", "output1.txt"):
        p = work / name
        if p.exists():
            acpu.append(p.read_text(encoding="utf-8", errors="replace"))
    if not any(ACPU_MARKER in text for text in acpu):
        print("FAIL: ACPU marker not found", flush=True)
        ok = False

    for name, want in EXPECTED.items():
        p = work / "run" / name
        if not p.exists():
            print("FAIL: missing %s" % name, flush=True)
            ok = False
            continue
        got = sha256(p)
        print("%-12s %s" % (name, got), flush=True)
        if got != want:
            print("FAIL: %s SHA mismatch, want %s" % (name, want), flush=True)
            ok = False

    svg = work / "run/benson.svg"
    if svg.exists():
        text = svg.read_text(encoding="utf-8")
        paths = text.count("<path")
        sizes = re.search(r'width="(\d+)" height="(\d+)"', text)
        print("svg          paths=%d size=%s" %
              (paths, sizes.groups() if sizes else "?"), flush=True)
        if paths != 10 or not sizes or sizes.groups() != ("492", "463"):
            print("FAIL: unexpected SVG geometry", flush=True)
            ok = False

    print("PASS: gp-star" if ok else "FAIL: gp-star", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
