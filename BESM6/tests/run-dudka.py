#!/usr/bin/env python3
"""Guest-path ДУДКА test: FOREX job with output stream + ЭК 50 1216.

Boots DISPAK from md00 (2053), patches the two 18Р dispatch constants
(01457 ПОДПРГ / 01460 ГАШРП1) the way run-gp-irq.py does, then submits
dudka-min.b6: a Fortran job that prints 40 lines (so РОБОТ spills at
least one output zone onto the system drum, тракт 24) and then calls
the MADLEN stub PK which sets М3=71717 and issues ЭК 50 1216.

The expected trace in debug.log:
  TRACE GP-EK050 Aex=01216        - the job's ЭК reaches ВЗУ
  _physobm zone 0413              - ТРУБКА loaded
  _physobm zone 0576              - ДУДКА loaded
  НИТИ: УВВ '155 вкл              - benson.be: увв '155
  РЕГ '34' word N                 - dudka.be init (9 words)
  18Р line raised                 - gp_raise_irq per element
  TRACE GRP18 -> op_int_1 0500    - guest ВНУПР dispatch
  TRACE РЕГ 037 GRP clear         - guest handler clears GRP
  TRACE ВЫПР interrupt return     - handler done
  КЗ / КОНЕЦ ЗАДАЧИ               - job finished

Env: SIMH_BIN, DUDKA_DECK, DUDKA_TIMEOUT (default 900), DUDKA_WORK.
"""
import os
import re
import shutil
import sys
import time
from pathlib import Path

import pexpect

SIMH = Path("/home/z/my-project/repos/simh")
EMU = Path(os.environ.get("SIMH_BIN", str(SIMH / "BIN/besm6")))
DECK = Path(os.environ.get("DUDKA_DECK",
                           str(SIMH / "BESM6/tests/dudka-min.b6")))
WORK = Path(os.environ.get("DUDKA_WORK", "/var/tmp/dudka-run"))
TIMEOUT = int(os.environ.get("DUDKA_TIMEOUT", "900"))
BASE_INI = SIMH / "BESM6/OSZAGR/dispak.ini"
DISKS = Path("/home/z/my-project/disks")


def make_ini(work: Path) -> Path:
    """dispak.ini rewritten for the sandbox: local disks, no telnet tty."""
    run_dir = work / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    out = []
    for line in BASE_INI.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if re.match(r"attach\s+tty\d+\s+Line=", s):
            out.append("; " + line)
            continue
        if "console log=" in s or re.match(r"set\s+cpu\s+log", s):
            out.append("; " + line)
            continue
        if re.match(r"attach\s+-n\s+(vu0|pi0|pl0|prn0|prn1)\b", s):
            out.append("; " + line)
            continue
        m = re.match(r"^(\s*attach\s+)((?:-e|-r|-n)\s+)?((?:md|drum|mg)\d+)\s+(\S+)\s*$", line)
        if m:
            prefix, flags, unit, path = m.groups()
            name = Path(path).name
            local = work / ("img-" + name)
            if unit == "md06":
                out.append("attach -n md06 %s" % (work / "img-2052"))
                continue
            if unit.startswith("drum"):
                out.append("attach -n %s %s" % (unit, work / ("img-" + unit)))
                continue
            if unit == "md00":
                out.append("attach -e md00 %s" % (work / "img-2053"))
                continue
            if unit == "md07":
                out.append("attach -e md07 %s" % (work / "img-2053"))
                continue
            # job decks ask for 'лен 41(2048)' (MADLEN lives on volume 2048)
            # and 'дис 41(2148)' (FOREX3); mirror the gp-star layout:
            # md41 = 2148 (FOREX), md45 = 2048 scratch copy for МАДЛЕН.
            if unit == "md41" and (DISKS / "2148").exists():
                out.append("attach -e md41 %s" % (DISKS / "2148"))
                continue
            if unit == "md45":
                src = DISKS / "2048"
                if not (work / "img-2048").exists():
                    shutil.copy2(src, work / "img-2048")
                out.append("attach -e md45 %s" % (work / "img-2048"))
                continue
            if local.exists():
                out.append("attach %s%s %s" % (flags or "", unit, local))
                continue
            # attach nothing for volumes we do not have: comment out
            out.append("; " + line)
            continue
        if "load ~/Yandex.Disk" in line:
            out.append("load boot_dispak.b6")
            continue
        out.append(line)
    out += [
        "attach -n prn0 %s" % (work / "output.txt"),
        "attach -n prn1 %s" % (work / "output1.txt"),
        "attach gp0 %s" % (run_dir / "gp-stream.dat"),
        "set gp plot=on",
        "set gp debug",
        "set debug %s" % (run_dir / "debug.log"),
        "set console log=%s" % (run_dir / "console.log"),
        "set cpu idle",
    ]
    ini = work / "simh.ini"
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")
    shutil.copy2(SIMH / "BESM6/boot_dispak.b6", run_dir / "boot_dispak.b6")
    # fresh drum images
    for unit in ("drum0", "drum1"):
        p = work / ("img-" + unit)
        if not p.exists():
            p.write_bytes(b"\0" * (8 * 1024 * 1024))
    return ini


def main() -> int:
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    for name in ("2053", "2052"):
        src = DISKS / name
        if src.exists():
            shutil.copy2(src, WORK / ("img-" + name))
    # md06 scratch
    (WORK / "img-2052").write_bytes(b"\0" * (8 * 1024 * 1024))
    ini = make_ini(WORK)
    print("work: %s" % WORK, flush=True)
    print("deck: %s" % DECK, flush=True)

    child = pexpect.spawn(
        str(EMU), [str(ini)], cwd=str(WORK / "run"), encoding="utf-8",
        timeout=TIMEOUT, dimensions=(50, 200))
    full_log = open(WORK / "console-full.log", "w", encoding="utf-8")
    child.logfile_read = full_log

    def send(cmd):
        child.sendline(cmd)

    def wait_prompt(timeout=30):
        child.expect(r"sim>\s*$", timeout=timeout)

    ok = True
    try:
        print("waiting for ДATA: ...", flush=True)
        child.expect(r"[ДD]ATA:", timeout=TIMEOUT)
        print("booted", flush=True)
        time.sleep(5)

        for cmd in ("ДАЙ А0", "ДАЙ А1", "НОМВ"):
            send(cmd)
            time.sleep(1.5)
        time.sleep(1)
        send("\x07")
        wait_prompt()

        # Patch the 18Р dispatch constants (as run-gp-irq.py does):
        # ПОДПРГ must pass bit 18, ГАШРП1 must not clear it.
        for cmd in ("d 01457 0000000036774000",
                    "d 01460 7777777701003000"):
            send(cmd)
            wait_prompt(15)
        send("go")
        time.sleep(3)

        # submit the deck: every simh command needs the ОС running;
        # C-g stops it, then the command goes to simh.
        for cmd in ("at vu0 %s" % DECK, "d 6 1", "d 5 10",
                    "set cpu req"):
            send("\x07")
            wait_prompt(15)
            send(cmd)
            wait_prompt(15)
        send("go")

        deadline = time.time() + 420
        state = "timeout"
        while time.time() < deadline:
            idx = child.expect(
                [r"КЗ\s*\(\s*[0-9ооO]+\s*\)", r"КОНЕЦ ЗАДАЧИ",
                 r"ЗАПРЕЩ", r"sim>\s*$", pexpect.TIMEOUT],
                timeout=30)
            if idx in (0, 1, 2):
                state = child.after if isinstance(child.after, str) else "?"
                break
            if idx == 3:
                send("go")
                continue
        print("job state: %s" % state, flush=True)
        time.sleep(10)

        send("\x07")
        try:
            wait_prompt(30)
            send("quit")
            child.expect(pexpect.EOF, timeout=30)
        except (pexpect.TIMEOUT, pexpect.EOF):
            child.terminate(force=True)
    finally:
        full_log.close()
        try:
            child.terminate(force=True)
        except Exception:
            pass

    # analyze the trace: debug.log has gp/cpu events, console.log has
    # the GP-EK050 extracode trace (printed to sim_log).
    dbg = WORK / "run/debug.log"
    trace_lines = []
    for p in (dbg, WORK / "run/console.log"):
        if p.exists():
            trace_lines += p.read_text(encoding="utf-8",
                                       errors="replace").splitlines()
    con = (WORK / "console-full.log").read_text(encoding="utf-8",
                                                errors="replace")

    def has(*needles):
        return [ln for ln in trace_lines
                if any(n in ln for n in needles)]

    checks = [
        ("EK 01216 reached ВЗУ",
         [ln for ln in trace_lines if re.search(r"GP-EK050.*Aex=01216", ln)]),
        ("ТРУБКА zone 0413 loaded",     has("zone = 0413")),
        ("ДУДКА zone 0576 loaded",      has("zone = 0576")),
        ("НИТИ '155 on",                has("УВВ '155")),
        ("РЕГ '34' init words",         has("РЕГ '34'")),
        ("18Р raised",                  has("18Р line raised")),
        ("ВНУПР dispatch (op_int_1)",   has("op_int_1")),
        ("РЕГ 037 GRP clear",           has("РЕГ 037")),
        ("ВЫПР return",                 has("ВЫПР")),
        ("РЕГ '240' data read",         has("РЕГ '240'")),
        ("job finished (КЗ/КОНЕЦ)",     [] if state in ("timeout",) else ["ok"]),
    ]
    print("\n========== TRACE CHECKS ==========", flush=True)
    for name, lines in checks:
        status = "PASS" if lines else "FAIL"
        if status == "FAIL":
            ok = False
        print("[%s] %s (%d hits)" % (status, name, len(lines)), flush=True)
        for ln in lines[:3]:
            print("      %s" % ln.strip(), flush=True)

    for name in ("plot.dat", "benson.out", "benson.svg", "gp-stream.dat"):
        p = WORK / "run" / name
        if p.exists():
            print("artifact %s: %d bytes" % (name, p.stat().st_size), flush=True)
        else:
            print("artifact %s: missing" % name, flush=True)

    print("\nconsole tail:", flush=True)
    for ln in con.splitlines()[-25:]:
        print("  | %s" % ln, flush=True)

    print("\nPASS: dudka" if ok else "\nFAIL: dudka", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
