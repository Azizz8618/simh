#!/usr/bin/env python3
"""Drive one VARIANT B deck through BESM-6 SIMH on an isolated tmux socket.

Rules (inherited from the proven ftgost harness, adapted to this workspace):
  * copies only: every image comes from setup_b.py into ./images
  * isolated tmux socket via TMUX_TMPDIR=./runs/tmux (never session 0)
  * console read ONLY via `capture-pane`; input ONLY via `send-keys`
  * boot marker `ДATA:` detected in the pane
  * `ДАЙ А0` / `ДАЙ А1` / `НОМВ` before the deck
  * `at vu0 <deck>` / `go` to feed the deck
  * post-deck `НОМВ` ONLY after a confirmed job-start line
  * stock `attach -n prn0/prn1` semantics, workspace-local capture paths

Exit code 0 means HARNESS PLUMBING passed. The product gate verdict is written
to runs/verdict_<case>.json and printed; it is deliberately NOT the exit code,
so later B-stage subagents can use this driver for both positive and negative
decks.

Usage:
  python3 drive_b.py --case B_X1234 \\
      --deck decks/deck_b_x1234.b6 \\
      --image /path/to/target-disk-image
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_b_stock as setup_b  # noqa: E402

WS = Path(__file__).resolve().parent
SIMH = Path("/home/azizz/Yandex.Disk/simh/BESM6")
EMULATOR = Path("/home/azizz/Yandex.Disk/simh/BESM6/BIN/besm6")
SHA_PREFIX = "d0583057"
RUNS = WS / "runs"
BOOT_MARKER = re.compile(r"[ДD]ATA:")
JOB_START = re.compile(r"B07[0-9]|ОЗ\s*\(|КЗ\s*\(")
COMPLETION = re.compile(r"КОНЕЦ ЗАДАЧИ|СБО")
RESULT21 = re.compile(r"(?m)^\s*21\s+\S")
FATAL = re.compile(r"Fatal|Segmentation|can.t attach|Error opening|backtrace", re.I)


def verify_emulator():
    h = hashlib.sha256(EMULATOR.read_bytes()).hexdigest()
    if not h.startswith(SHA_PREFIX):
        raise RuntimeError(f"emulator SHA mismatch: {h[:16]} != {SHA_PREFIX}...")
    return h[:16]


def tmux(env, *args, check=True):
    r = subprocess.run(["tmux", *args], text=True, capture_output=True, env=env)
    if check and r.returncode:
        raise RuntimeError(f"tmux {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", required=True, help="short run id, e.g. B_X1234")
    ap.add_argument("--deck", required=True, help="deck file (real Fortran source)")
    ap.add_argument("--image", required=True, help="target disk image for md41 (copied locally)")
    ap.add_argument("--session", default=None, help="tmux session name (default bh-<case>)")
    ap.add_argument("--timeout", type=int, default=240, help="overall wait budget, seconds")
    ap.add_argument("--trace", action="store_true", help="arm set debug + set cpu debug from boot")
    ap.add_argument("--skip-setup", action="store_true", help="reuse existing runs/ini_<case>.ini")
    a = ap.parse_args()

    sha = verify_emulator()
    deck = Path(a.deck).resolve()
    if not deck.is_file():
        raise FileNotFoundError(f"deck not found: {deck}")
    ini = (RUNS / f"ini_{a.case}.ini")
    if not a.skip_setup:
        ini = setup_b.make_ini(a.case, a.image, trace=a.trace)
    if not ini.is_file():
        raise FileNotFoundError(f"ini not found: {ini}")

    session = a.session or ("bh-" + a.case)
    env = dict(os.environ, TMUX_TMPDIR=str(RUNS / "tmux"))
    (RUNS / "tmux").mkdir(parents=True, exist_ok=True)

    if subprocess.run(["tmux", "has-session", "-t", session], env=env,
                      capture_output=True).returncode == 0:
        tmux(env, "kill-session", "-t", session, check=False)

    def pane(history=2000):
        return tmux(env, "capture-pane", "-p", "-S", str(-history),
                    "-t", session, check=False)

    def send(text, enter=True, slow=False):
        if slow:
            for ch in text:
                tmux(env, "send-keys", "-t", session, "-l", ch)
                time.sleep(0.12)
        else:
            tmux(env, "send-keys", "-t", session, "-l", text)
        if enter:
            tmux(env, "send-keys", "-t", session, "Enter")

    def lastline():
        lines = [l for l in pane(history=200).splitlines() if l.strip()]
        return lines[-1].strip() if lines else ""

    def to_sim():
        for _ in range(20):
            if lastline().endswith("sim>"):
                return True
            time.sleep(0.5)
        return False

    def wait_for(pred, budget):
        deadline = time.time() + budget
        while time.time() < deadline:
            txt = pane()
            if pred(txt):
                return txt
            time.sleep(0.5)
        return pane()

    booted = sim_ready = deck_sent = job_start = False
    nomv_after = completion = False
    console = ""
    tmux(env, "new-session", "-d", "-s", session, "-x", "200", "-y", "50",
         f"cd {SIMH / 'OSZAGR'} && {EMULATOR} {ini}")
    try:
        txt = wait_for(lambda t: bool(BOOT_MARKER.search(t)), a.timeout)
        booted = bool(BOOT_MARKER.search(txt))
        if not booted:
            raise TimeoutError("no ДATA: boot marker in capture-pane")
        print(f"[{a.case}] BOOT marker ДATA: seen; emulator {sha}", flush=True)

        time.sleep(20.0)
        for cmd in ("ДАЙ А0", "ДАЙ А1"):
            send(cmd, slow=True)
            time.sleep(2.0)
        send("НОМВ", slow=True)
        time.sleep(2.0)

        tmux(env, "send-keys", "-t", session, "C-g")
        sim_ready = to_sim()
        if not sim_ready:
            raise RuntimeError("no sim> prompt after C-g")

        for cmd in (f"at vu0 {deck}", "d 6 1", "d 5 10", "set cpu req"):
            send(cmd)
            time.sleep(0.8)
        deck_sent = True
        send("go")
        time.sleep(1.0)
        print(f"[{a.case}] deck attached to vu0 and go sent", flush=True)

        # Post-deck НОМВ strictly after a confirmed job-start line.
        txt = wait_for(lambda t: bool(JOB_START.search(t)), a.timeout / 2)
        job_start = bool(JOB_START.search(txt))
        if job_start:
            send("НОМВ", slow=True)
            nomv_after = True
            time.sleep(1.0)
            print(f"[{a.case}] post-deck НОМВ sent after job-start", flush=True)
        else:
            print(f"[{a.case}] no job-start line; post-deck НОМВ NOT sent", flush=True)

        deadline = time.time() + a.timeout
        while time.time() < deadline:
            txt = pane()
            if COMPLETION.search(txt):
                completion = True
                break
            if txt.rstrip().endswith("sim>"):
                send("go")
            if FATAL.search(txt):
                break
            time.sleep(1.5)
        time.sleep(3.0)
        console = pane()
    finally:
        (RUNS / f"console_{a.case}.txt").write_text(console, encoding="utf-8")
        acpu_dir = SIMH / "ACPU"
        for name, p in (("acpu", acpu_dir / "output.txt"),
                        ("acpu1", acpu_dir / "output1.txt")):
            if p.exists():
                shutil.copy2(p, RUNS / f"{name}_{a.case}.txt")
        tmux(env, "kill-session", "-t", session, check=False)

    prn_path = RUNS / f"localprn_{a.case}.txt"
    prn1_path = RUNS / f"localprn1_{a.case}.txt"
    prn = prn_path.read_text(encoding="utf-8", errors="replace") if prn_path.is_file() else ""
    prn1_path = RUNS / f"localprn1_{a.case}.txt"
    prn1 = prn1_path.read_text(encoding="utf-8", errors="replace") if prn1_path.is_file() else ""

    result21 = bool(RESULT21.search(prn))
    completed_prn = "КОНЕЦ ЗАДАЧИ" in prn
    absence = "ОТСУТСТВУЕТ" in (prn + console)
    fatal = bool(FATAL.search(console))

    plumbing = (booted and sim_ready and deck_sent and
                (job_start or completed_prn or absence or bool(prn.strip())) and
                not fatal)
    if result21 and completed_prn:
        product = "PASS"
    elif completed_prn and not result21:
        product = "FAIL"
    else:
        product = "N/A"

    verdict = {
        "case": a.case,
        "deck": str(deck),
        "image": str(Path(a.image).resolve()),
        "emulator_sha": sha,
        "boot_marker": booted,
        "dai_a0_a1_nomv_sent": True,
        "sim_prompt_reached": sim_ready,
        "deck_attached_and_go": deck_sent,
        "job_start_line": job_start,
        "post_deck_nomv": nomv_after,
        "completion_marker": completion,
        "result_21_line": result21,
        "absence_marker": absence,
        "fatal_error": fatal,
        "plumbing": "PASS" if plumbing else "FAIL",
        "product_gate": product,
        "captures": {
            "console": str(RUNS / f"console_{a.case}.txt"),
            "prn0": str(prn_path),
            "prn1": str(prn1_path),
            "acpu": str(RUNS / f"acpu_{a.case}.txt"),
        },
    }
    (RUNS / f"verdict_{a.case}.json").write_text(
        json.dumps(verdict, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (RUNS / f"verdict_{a.case}.txt").write_text(
        "\n".join(f"{k}: {v}" for k, v in verdict.items()) + "\n", encoding="utf-8")

    print(f"===== verdict {a.case} =====", flush=True)
    for k in ("plumbing", "product_gate", "boot_marker", "job_start_line",
              "result_21_line", "completion_marker", "absence_marker",
              "fatal_error"):
        print(f"  {k}: {verdict[k]}", flush=True)
    if prn.strip():
        print("----- prn0 (tail) -----", flush=True)
        print(prn[-2000:], flush=True)
    return 0 if plumbing else 3


if __name__ == "__main__":
    sys.exit(main())
