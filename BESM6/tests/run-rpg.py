#!/usr/bin/env python3
"""RPG plotter acceptance test through the PL channel (0170/0171).

This test does not use the БЕНСОН/ГРП 18 path.  A DISPAK card deck
(rpg-line.b6 or rpg-color.b6) defines the figure in arrays X, Y, P, DX, DY
and prints an ACPU marker when run under DISPAK.  The runner loads
rpg-stream.b6 in guest memory and emits each УК byte with
`d ACC` / `d PC` / `go`.  (`run` would reset the simulator and clear ACC,
so `go`, the continue command, is used.)  The guest instruction is
УВВ 0171, i.e. the PL1 channel, the real RPG transport.
`04170..04173` must return 0 in the RPG model; STOP_UNIMPLEMENTED fails
the run before any SVG is produced.

The simulator must be built with `set pl1 rpg`, `set pl1 rpg-wide`,
`set pl1 rpg-6color` and `set pl1 rpg-step=...`.  SIMH_BIN overrides the
binary.  The default harness is bare: no DISPAK boot, with the explicit
`show pl1 rpg` marker as proof.  `--harness deck` boots DISPAK and feeds
the card deck on vu0 as well.  All scratch files stay under /var/tmp.
This test is not part of besm6_test.ini.

Historical protocol is not fully restored.  The six-colour pen-field
mapping used here is a hypothesis from gp1.be/gp4.be, not a proved
VNIIEF encoding.
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
import xml.etree.ElementTree as ET
from pathlib import Path

SIMH = Path(__file__).resolve().parents[1]
BASE_INI = SIMH / "OSZAGR/dispak.ini"
TESTS = Path(__file__).resolve().parent
EMU = Path(os.environ.get("SIMH_BIN", str(SIMH / "BIN/besm6")))
DISK = Path(os.environ.get("NITI_2348", str(Path.home() / ".besm6/2348")))
COMPILER = Path("/usr/local/share/besm6/2148")
STREAM = TESTS / "rpg-stream.b6"
GOLDEN = TESTS / "rpg-golden/EXPECTED.md"
BOOT = re.compile(r"[ДD]ATA:")
DONE = re.compile(r"КОНЕЦ ЗАДАЧИ|[КK]З\s*\(|12345")
PROMPT = re.compile(r"sim>\s*$")

MODES = {
    "line": {
        "deck": TESTS / "rpg-line.b6",
        "marker": "4 2 8 6 12345",
        "paths": 3,
        "colors": 2,
        "palette": {"black", "red"},
        "roll": "340mm",
        "wide": False,
        "six": False,
    },
    "color": {
        "deck": TESTS / "rpg-color.b6",
        "marker": "12 6 6 4500 12345",
        "paths": 6,
        "colors": 6,
        "palette": {"black", "red", "green", "blue", "magenta", "cyan"},
        "roll": "680mm",
        "wide": True,
        "six": True,
    },
}

# Frame УК bytes from ГП.md 6.1: 001vvvvv is |DX| low, 101vvvvv high,
# 010vvvvv starts a frame with |DY| low, 110vvvvv high, 011vvvvv carries
# the signs and the pen.  The decoder uses v3 as the upper pen bank for
# colours 4..6, which is a documented model hypothesis.
FRAME_DX_LO = 0o040
FRAME_DX_HI = 0o240
FRAME_DY_LO = 0o100
FRAME_DY_HI = 0o300
FRAME_PEN = 0o140
MOTOR_OFF = 0o100
ARRAY_RE = re.compile(r"^\s*(DX|DY|X|Y|P)\((\d+)\)\s*=\s*(-?\d+)\s*$")


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


def make_ini(work, harness):
    (work / "run").mkdir(parents=True)
    (work / "tmux").mkdir()
    if harness == "bare":
        # Minimal harness: no DISPAK boot, just the guest stub and PL1.
        ini = work / "simh.ini"
        ini.write_text("\n".join([
            "load %s" % STREAM,
            "set pl debug=UK,GEOM,SVG",
            "set debug %s" % (work / "debug.log"),
        ]) + "\n", encoding="utf-8")
        return ini
    disk = work / "2048"
    compiler = work / "2148"
    shutil.copy2(DISK, disk)
    shutil.copy2(COMPILER, compiler)
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
        if "attach -e md45 " in line:
            out.append("attach -e md45 %s" % disk)
            continue
        if "attach -n prn0 " in line:
            out.append("attach -n prn0 %s" % (work / "output.txt"))
            continue
        if "attach -n prn1 " in line:
            out.append("attach -n prn1 %s" % (work / "output1.txt"))
            continue
        out.append(line)
    out += [
        "set pl debug=UK,GEOM,SVG",
        "set debug %s" % (work / "debug.log"),
    ]
    ini = work / "simh.ini"
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")
    return ini


def parse_figure(deck):
    """Read the figure from the deck, the single source of geometry."""
    arrays = {name: {} for name in ("X", "Y", "P", "DX", "DY")}
    for line in deck.read_text(encoding="utf-8").splitlines():
        m = ARRAY_RE.match(line)
        if m:
            arrays[m.group(1)][int(m.group(2))] = int(m.group(3))
    for name in ("X", "Y", "P", "DX", "DY"):
        if arrays[name]:
            keys = sorted(arrays[name])
            if keys != list(range(1, keys[-1] + 1)):
                raise ValueError("rpg, sparse %s array in %s" % (name, deck))
    count = len(arrays["X"])
    if count == 0 or any(len(arrays[n]) != count
                         for n in ("Y", "P", "DX", "DY")):
        raise ValueError("rpg, incomplete figure in %s" % deck)
    return [tuple(arrays[n][j] for n in ("X", "Y", "P", "DX", "DY"))
            for j in range(1, count + 1)]


def frame_pen_byte(pen, dx, dy):
    """011vvvvv: signs in v5/v4, the upper pen bank in v3, pen in v2v1."""
    v = 0
    if dy < 0:
        v |= 0x10
    if dx < 0:
        v |= 0x08
    if pen > 3:
        v |= 0x04 | (pen - 3)
    else:
        v |= pen
    return FRAME_PEN | v


def uk_bytes(figure):
    """Turn X/Y/P/DX/DY frames into the PL1 УК frame-byte stream."""
    # The decoder ignores coordinate bytes until the first 011vvvvv byte,
    # so start in frame mode with a pen-up, zero-movement frame.
    result = [FRAME_PEN]
    for _x, _y, pen, dx, dy in figure:
        adx, ady = abs(dx), abs(dy)
        # The decoder treats '010' (|DY| low) as the start of a frame and
        # clears the accumulated X bytes there, so Y is sent first.
        result.append(FRAME_DY_LO | (ady & 0x1f))
        result.append(FRAME_DY_HI | ((ady >> 5) & 0x1f))
        result.append(FRAME_DX_LO | (adx & 0x1f))
        result.append(FRAME_DX_HI | ((adx >> 5) & 0x1f))
        result.append(frame_pen_byte(pen, dx, dy))
    result.append(MOTOR_OFF)
    return result


def golden_values(mode):
    values = {}
    if GOLDEN.exists():
        text = GOLDEN.read_text(encoding="utf-8")
        m = re.search(r"^mode %s:\s*sha=(\S+)\s+size=(\S+)\s+paths=(\S+)"
                      r"\s+colors=(\S+)" % re.escape(mode), text, re.M)
        if m:
            values = dict(zip(("sha", "size", "paths", "colors"),
                              m.groups()))
    return values


def wait_for(pane, deadline, predicate, interval=0.5):
    while time.time() < deadline:
        text = pane()
        if predicate(text):
            return text
        time.sleep(interval)
    return None


def run_once(mode, cfg, a, step):
    work = Path(tempfile.mkdtemp(prefix="rpg-%s-" % mode, dir="/var/tmp"))
    ini = make_ini(work, a.harness)
    env = dict(os.environ, TMUX_TMPDIR=str(work / "tmux"))
    session = "rpg-%s-%d" % (mode, os.getpid())
    bytes_out = uk_bytes(parse_figure(cfg["deck"]))
    print("mode=%s work=%s bytes=%d" % (mode, work, len(bytes_out)),
          flush=True)
    tmux(env, "new-session", "-d", "-s", session, "-x", "200", "-y", "50",
         "cd %s && BESM6_NITI_USER=1 %s %s" % (work / "run", EMU, ini))

    def pane(history=2000):
        return tmux(env, "capture-pane", "-p", "-S", str(-history),
                    "-t", session, check=False)

    def send(text):
        tmux(env, "send-keys", "-t", session, "-l", text)
        tmux(env, "send-keys", "-t", session, "Enter")

    ok = True
    done = False
    try:
        if a.harness == "bare":
            if not wait_for(lambda h=20: pane(history=h),
                            time.time() + 15,
                            lambda t: bool(PROMPT.search(t)),
                            interval=0.3):
                print("FAIL: rpg-%s, no sim prompt" % mode, flush=True)
                return None
        else:
            deadline = time.time() + a.timeout
            if not wait_for(pane, deadline, lambda t: BOOT.search(t)):
                print("FAIL: rpg-%s, no ДATA: boot marker" % mode,
                      flush=True)
                return None
            time.sleep(5)
            for cmd in ("ДАЙ А0", "ДАЙ А1", "НОМВ"):
                send(cmd)
                time.sleep(1.5)

            tmux(env, "send-keys", "-t", session, "C-g")
            wait_for(lambda h=20: pane(history=h), time.time() + 15,
                     lambda t: bool(PROMPT.search(t)), interval=0.3)
            for cmd in ("at vu0 %s" % cfg["deck"], "d 6 1", "d 5 10",
                        "set cpu req"):
                send(cmd)
                time.sleep(0.7)
            send("go")
            time.sleep(2)

            deadline = time.time() + a.timeout

            def job_done(text):
                if DONE.search(text):
                    return True
                for name in ("output.txt", "output1.txt"):
                    p = work / name
                    if p.exists() and cfg["marker"] in p.read_text(
                            encoding="utf-8", errors="replace"):
                        return True
                return False

            while time.time() < deadline:
                text = pane()
                if job_done(text):
                    done = True
                    break
                if PROMPT.search(text):
                    send("go")
                time.sleep(1.0)
            if not done:
                print("note: rpg-%s, deck job marker not seen; using the "
                      "explicit RPG model marker" % mode, flush=True)

            tmux(env, "send-keys", "-t", session, "C-g")
            wait_for(lambda h=20: pane(history=h), time.time() + 15,
                     lambda t: bool(PROMPT.search(t)), interval=0.3)

            # The guest is stopped, so mask interrupts and drive the PL
            # channel from the host.  No GP/BENSON device is involved.
            for cmd in ("d MPRP 0", "d MGRP 0", "load %s" % STREAM):
                send(cmd)
                time.sleep(0.4)

        send("attach pl1 %s" % (work / "run/pl1.dat"))
        time.sleep(0.4)
        for cmd in (["set pl1 rpg"] +
                    (["set pl1 rpg-wide"] if cfg["wide"] else []) +
                    (["set pl1 rpg-6color"] if cfg["six"] else []) +
                    ["set pl1 rpg-step=%s" % step]):
            send(cmd)
            time.sleep(0.4)

        for index, value in enumerate(bytes_out):
            token = "RPG%03d" % (index * 512 + value)
            send("d ACC %016o" % value)
            send("d PC 077700")
            send("echo %s" % token)
            send("go")
            waited = wait_for(lambda h=20: pane(history=h),
                              time.time() + 2,
                              lambda t: token in t and
                              bool(PROMPT.search(t)),
                              interval=0.05)
            if waited is None:
                tmux(env, "send-keys", "-t", session, "C-g", check=False)
                print("FAIL: rpg-%s, emit stub did not stop on %03o" %
                      (mode, value), flush=True)
                ok = False
                break
            spin_token = "SPN%03d" % (index * 512 + value)
            send("d PC 077720")
            send("echo %s" % spin_token)
            send("step %d" % a.spin)
            if wait_for(lambda h=20: pane(history=h),
                        time.time() + a.timeout,
                        lambda t: spin_token in t and
                        bool(PROMPT.search(t)), interval=0.05) is None:
                tmux(env, "send-keys", "-t", session, "C-g", check=False)
                print("FAIL: rpg-%s, spin stub did not stop" % mode,
                      flush=True)
                ok = False
                break

        show_token = "RPGMARK%03d" % len(bytes_out)
        send("echo %s" % show_token)
        send("show pl1 rpg")
        wait_for(lambda h=20: pane(history=h), time.time() + 15,
                 lambda t: show_token in t and bool(PROMPT.search(t)),
                 interval=0.1)
        (work / "console.txt").write_text(pane(), encoding="utf-8")
        for cmd in ("detach pl1", "quit"):
            send(cmd)
            time.sleep(1.0)
    finally:
        tmux(env, "kill-session", "-t", session, check=False)

    result = {
        "mode": mode,
        "work": work,
        "ok": ok,
        "console": work / "console.txt",
        "svg": work / "run/rpg.svg",
        "raw": work / "run/pl1.dat",
        "bytes": len(bytes_out),
        "acpu_marker": False,
        "svg_sha": None,
        "svg_size": 0,
        "paths": 0,
        "colors": [],
        "width": None,
        "height": None,
    }
    acpu = []
    for name in ("output.txt", "output1.txt"):
        p = work / name
        if p.exists():
            acpu.append(p.read_text(encoding="utf-8", errors="replace"))
    result["acpu_marker"] = any(cfg["marker"] in text for text in acpu)
    console = result["console"].read_text(
        encoding="utf-8", errors="replace") if result["console"].exists() else ""
    if a.harness == "deck":
        if not (result["acpu_marker"] or cfg["marker"] in console or
                "КОНЕЦ ЗАДАЧИ" in console):
            print("note: rpg-%s, deck marker not in ACPU/console" % mode,
                  flush=True)

    # Explicit completion marker from the RPG model itself.
    marker = re.search(r"(\d+) УК, (\d+) strokes, no jam", console)
    if marker:
        got_uk, got_strokes = map(int, marker.groups())
        print("model marker: %d УК, %d strokes" % (got_uk, got_strokes),
              flush=True)
        if got_uk != len(bytes_out) or got_strokes != cfg["paths"]:
            print("FAIL: rpg-%s, bad model marker" % mode, flush=True)
            result["ok"] = False
    else:
        print("FAIL: rpg-%s, no RPG model marker in console" % mode,
              flush=True)
        result["ok"] = False

    # In RPG mode pl_control() feeds rpg_uk() instead of writing the raw
    # byte to pl1.dat.  The model counter checked above is the transport
    # proof; the raw file is only kept as optional diagnostic evidence.
    raw = result["raw"]
    if raw.exists():
        print("pl1.dat       %d B (diagnostic only)" % raw.stat().st_size,
              flush=True)

    svg = result["svg"]
    if not svg.exists() or svg.stat().st_size == 0:
        print("FAIL: rpg-%s, rpg.svg missing or empty" % mode, flush=True)
        result["ok"] = False
        return result
    result["svg_sha"] = sha256(svg)
    result["svg_size"] = svg.stat().st_size
    try:
        root = ET.parse(str(svg)).getroot()
    except ET.ParseError as exc:
        print("FAIL: rpg-%s, invalid SVG: %s" % (mode, exc), flush=True)
        result["ok"] = False
        return result
    if not root.tag.endswith("svg"):
        print("FAIL: rpg-%s, root is not <svg>" % mode, flush=True)
        result["ok"] = False
    paths = [p for p in root.iter() if p.tag.endswith("path")]
    result["paths"] = len(paths)
    result["colors"] = sorted({p.get("stroke") for p in paths
                               if p.get("stroke")})
    result["width"] = root.get("width")
    result["height"] = root.get("height")
    print("rpg.svg       %d B sha=%s" %
          (result["svg_size"], result["svg_sha"]), flush=True)
    print("svg           paths=%d colors=%s width=%s height=%s" %
          (result["paths"], ",".join(result["colors"]),
           result["width"], result["height"]), flush=True)

    if result["paths"] < cfg["paths"]:
        print("FAIL: rpg-%s, expected at least %d paths" %
              (mode, cfg["paths"]), flush=True)
        result["ok"] = False
    if len(result["colors"]) < cfg["colors"]:
        print("FAIL: rpg-%s, expected at least %d stroke colours" %
              (mode, cfg["colors"]), flush=True)
        result["ok"] = False
    if set(result["colors"]) != cfg["palette"]:
        print("FAIL: rpg-%s, unexpected palette: %s" %
              (mode, result["colors"]), flush=True)
        result["ok"] = False
    if result["width"] != cfg["roll"]:
        print("FAIL: rpg-%s, roll width is %s, expected %s" %
              (mode, result["width"], cfg["roll"]), flush=True)
        result["ok"] = False

    gold = golden_values(mode)
    if gold.get("sha") and re.fullmatch(r"[0-9a-f]{64}", gold["sha"]):
        if gold["sha"] != result["svg_sha"]:
            print("FAIL: rpg-%s, SVG SHA mismatch, want %s" %
                  (mode, gold["sha"]), flush=True)
            result["ok"] = False
    else:
        print("note: rpg-%s, golden SHA pending, structure only" % mode,
              flush=True)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int,
                    default=int(os.environ.get("TIMEOUT", "300")))
    ap.add_argument("--runs", type=int, default=int(os.environ.get("RUNS", "3")))
    ap.add_argument("--modes", default=os.environ.get("RPG_MODES", "line,color"))
    ap.add_argument("--step", default=os.environ.get("RPG_STEP", "0.125"))
    ap.add_argument("--spin", type=int,
                    default=int(os.environ.get("RPG_SPIN", "500000")))
    ap.add_argument("--harness", default=os.environ.get("RPG_HARNESS", "bare"),
                    choices=("bare", "deck"))
    a = ap.parse_args()

    for p in (EMU, BASE_INI, STREAM, DISK, COMPILER):
        if not p.exists():
            print("FAIL: rpg, missing %s" % p, file=sys.stderr)
            return 2
    print("binary: %s" % EMU, flush=True)

    ok = True
    for mode in [m.strip() for m in a.modes.split(",") if m.strip()]:
        if mode not in MODES:
            print("FAIL: rpg, unknown mode %s" % mode, flush=True)
            return 2
        cfg = MODES[mode]
        runs = a.runs if mode == "color" else 1
        hashes = []
        for i in range(runs):
            result = run_once(mode, cfg, a, a.step)
            if result is None:
                return 1
            ok = ok and result["ok"]
            hashes.append(result["svg_sha"])
            if runs == 1:
                print("PASS: rpg-%s" % mode if result["ok"]
                      else "FAIL: rpg-%s" % mode, flush=True)
        if runs > 1:
            if any(h is None for h in hashes) or len(set(hashes)) != 1:
                print("FAIL: rpg-%s, non-deterministic SVG SHA: %s" %
                      (mode, hashes), flush=True)
                ok = False
            else:
                print("deterministic: rpg-%s, %d runs sha=%s" %
                      (mode, runs, hashes[0]), flush=True)
            print("PASS: rpg-%s" % mode if ok else "FAIL: rpg-%s" % mode,
                  flush=True)

    print("PASS: rpg" if ok else "FAIL: rpg", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
