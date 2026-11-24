#!/usr/bin/env python3
"""Isolated functional tests for the RPG model features in simh BESM-6.

No DISPAK boot, no shared BIN overwrite.  Every case starts an isolated
`besm6 <ini>` process in its own scratch directory.  The УК bytes enter the
decoder through the `set pl1 rpg-uk=...` test hook, but each assertion reads
a real model effect: the generated rpg.svg geometry, the `show pl1 rpg`
counter and jam line, the RPG debug trace, or a guest `увв 4170` read of
the 04170 jam register.

Run:
    SIMH_BIN=/path/to/besm6 ./run-rpg-features.py
    SIMH_BIN=/path/to/besm6 ./run-rpg-features.py --update-golden

The golden file in rpg-features-golden/EXPECTED.md records real SHA256,
sizes and output.  All cases are repeated three times by default to check
determinism.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

TESTS = Path(__file__).resolve().parent
EMU = Path(os.environ.get("SIMH_BIN", str(TESTS.parent.parent / "BIN/besm6")))
GOLDEN = TESTS / "rpg-features-golden/EXPECTED.md"
STUB = TESTS / "rpg-features-jam.b6"

CASES = {
    "autoproll_on": "rpg-features-autoproll.ini",
    "autoproll_off": "rpg-features-autoproll-off.ini",
    "step_fine": "rpg-features-step-fine.ini",
    "step_coarse": "rpg-features-step-coarse.ini",
    "jam": "rpg-features-jam.ini",
}

TAIL_STEPS = 1240
TAIL_MM = 155.0
MARKER_RE = re.compile(
    r"^(on|off), (\d+) УК, (\d+) strokes, (no jam|jam)\s*$", re.M)
ACC_RE = re.compile(r"^ACC:\s+([0-9\s]+?)\s*$", re.M)
BANNER_RE = re.compile(r"^БЭСМ-6 simulator .*$", re.M)
CASE_RE = re.compile(r"^case (\w+) (\{.*\})\s*$", re.M)
COMMENT_RE = re.compile(
    r"<!-- step ([\d.]+) mm; roll width (\d+) mm; six-colour (\S+) -->")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_svg(path):
    data = path.read_bytes()
    root = ET.fromstring(data)
    paths = [el.attrib.get("d", "") for el in root.iter()
             if el.tag.endswith("path")]
    comment = COMMENT_RE.search(data.decode("utf-8", errors="replace"))
    return {
        "sha256": sha256(data),
        "size": len(data),
        "width": root.attrib.get("width"),
        "height": root.attrib.get("height"),
        "viewBox": root.attrib.get("viewBox"),
        "comment_step": comment.group(1) if comment else None,
        "comment_roll": comment.group(2) if comment else None,
        "paths": paths,
    }


def run_once(name, ini_path, work_parent):
    work = Path(tempfile.mkdtemp(prefix="rpg-features-%s-" % name,
                                 dir=work_parent))
    # Do not name the scratch script simh.ini: SIMH also auto-runs a file
    # with that name, so an explicit argument would execute the case twice.
    shutil.copy2(ini_path, work / "case.ini")
    if name == "jam":
        shutil.copy2(STUB, work / STUB.name)
    proc = subprocess.run([str(EMU), "case.ini"], cwd=work, text=True,
                          capture_output=True, timeout=90)
    console = proc.stdout + proc.stderr
    markers = ["%s, %s УК, %s strokes, %s" % m
               for m in MARKER_RE.findall(console)]
    acc = ["".join(a.split()) for a in ACC_RE.findall(console)]
    banner = BANNER_RE.search(console)

    rpg_debug = []
    debug_path = work / "debug.log"
    if debug_path.exists():
        for line in debug_path.read_text(
                encoding="utf-8", errors="replace").splitlines():
            if "DBG(" in line and " PL " in line:
                rpg_debug.append(line.split(" PL ", 1)[1])

    svg_path = work / "rpg.svg"
    svg = parse_svg(svg_path) if svg_path.exists() else None
    facts = {
        "returncode": proc.returncode,
        "banner": banner.group(0) if banner else None,
        "markers": markers,
        "acc": acc,
        "rpg_debug": rpg_debug,
        "svg": svg,
    }
    return facts, work


def signature(facts):
    return json.dumps(facts, sort_keys=True, ensure_ascii=False)


def viewbox(svg):
    return [float(v) for v in svg["viewBox"].split()]


def physical_mm(value):
    return float(value[:-2]) if value.endswith("mm") else float(value)


def close(a, b):
    return abs(a - b) < 1e-6


def check(ok, message, failures):
    if ok:
        print("  PASS %s" % message)
    else:
        print("  FAIL %s" % message)
        failures.append(message)


def verify_relations(all_facts, failures):
    """Prove each feature from observed values, not from a successful set."""
    print("functional relations:")
    on = all_facts["autoproll_on"]
    off = all_facts["autoproll_off"]
    fine = all_facts["step_fine"]
    coarse = all_facts["step_coarse"]
    jam = all_facts["jam"]

    for label, facts in (("autoproll_on", on), ("autoproll_off", off),
                         ("step_fine", fine), ("step_coarse", coarse)):
        check(facts["svg"] is not None, "%s: rpg.svg exists" % label,
              failures)
        if facts["svg"] is None:
            continue
        check(facts["markers"] == ["on, 4 УК, 1 strokes, no jam"],
              "%s: model counted 4 УК and 1 stroke, no jam" % label,
              failures)
        check(facts["svg"]["paths"] == ["M 0 2720 L 10 2720"] or
              facts["svg"]["paths"] == ["M 0 5440 L 10 5440"],
              "%s: the fed frame produced one real stroke" % label,
              failures)
        check(sum("RPG stroke " in line for line in facts["rpg_debug"]) == 1,
              "%s: trace shows exactly one stroke" % label, failures)

    if on["svg"] and off["svg"]:
        vb_on, vb_off = viewbox(on["svg"]), viewbox(off["svg"])
        check(close(vb_on[2] - vb_off[2], TAIL_STEPS),
              "autoproll: viewport width grows by 1240 steps", failures)
        check(close(physical_mm(on["svg"]["height"]) -
                    physical_mm(off["svg"]["height"]), TAIL_MM),
              "autoproll: physical height grows by 155 mm", failures)
        check(vb_on[3] == vb_off[3] and
              on["svg"]["paths"] == off["svg"]["paths"],
              "autoproll: paper feed leaves the drawn stroke unchanged",
              failures)

    if fine["svg"] and coarse["svg"]:
        vb_fine, vb_coarse = viewbox(fine["svg"]), viewbox(coarse["svg"])
        check(fine["svg"]["comment_step"] == "0.0625" and
              coarse["svg"]["comment_step"] == "0.125",
              "step: SVG records 0.0625 mm against 0.125 mm", failures)
        check(close(vb_fine[3], 2 * vb_coarse[3]),
              "step: roll viewport height doubles (2720 to 5440)", failures)
        check(close((physical_mm(fine["svg"]["height"]) - 10) * 2,
                    physical_mm(coarse["svg"]["height"]) - 10),
              "step: used paper length halves with the fine step", failures)
        pf = [int(v) for v in re.findall(r"-?\d+", fine["svg"]["paths"][0])]
        pc = [int(v) for v in re.findall(r"-?\d+", coarse["svg"]["paths"][0])]
        check(len(pf) == 4 and len(pc) == 4 and
              pf[1] == 2 * pc[1] and pf[3] == 2 * pc[3],
              "step: the same stroke lands at twice the step coordinate",
              failures)

    check(jam["markers"] == ["on, 11 УК, 1 strokes, jam",
                             "off, 0 УК, 0 strokes, no jam"],
          "jam: show reports jam, reset reports no jam", failures)
    check(jam["acc"] == ["0000000000000001", "0000000000000000"],
          "jam: УВВ 04170 returns 01 while jammed and 0 after reset",
          failures)
    check(any("RPG UK 11: 141 class 3 v 01 pen (x=0 y=2720 pen=1)"
              in line for line in jam["rpg_debug"]),
          "jam: third frame clamps y at the 2720-step roll limit", failures)
    if jam["svg"]:
        check(jam["svg"]["viewBox"] == "0 0 80 2720" and
              jam["svg"]["paths"] == ["M 0 2720 L 0 0"],
              "jam: SVG shows a full-roll stroke at the limit", failures)


def read_golden():
    values = {}
    if not GOLDEN.exists():
        return values
    for match in CASE_RE.finditer(GOLDEN.read_text(encoding="utf-8")):
        try:
            values[match.group(1)] = json.loads(match.group(2))
        except json.JSONDecodeError:
            pass
    return values


def write_golden(all_facts):
    lines = [
        "# RPG features golden output",
        "",
        "Generated by `run-rpg-features.py --update-golden` from real",
        "isolated `besm6` runs.  No DISPAK boot.  УК bytes are injected with",
        "`set pl1 rpg-uk=...`; the assertions below observe rpg.svg,",
        "`show pl1 rpg`, the RPG debug trace, and guest УВВ 04170 reads.",
        "",
        "Binary used for this capture: `%s`." %
        all_facts["autoproll_on"]["banner"],
        "",
        "Each case ran three times with identical facts (SHA256, size,",
        "markers, ACC values, RPG debug lines and SVG attributes).",
        "",
        "## Observed output",
        "",
        "| case | SVG SHA256 | bytes | viewBox | height | markers | ACC |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in sorted(all_facts):
        facts = all_facts[name]
        svg = facts["svg"]
        lines.append("| %s | `%s` | %s | `%s` | %s | %s | %s |" % (
            name, svg["sha256"], svg["size"], svg["viewBox"],
            svg["height"], "; ".join(facts["markers"]),
            ", ".join(facts["acc"]) or "(none)"))
    lines += [
        "",
        "Notes:",
        "",
        "- autoproll: `rpg_finish()` calls `rpg_line(1240, 0)` with the pen",
        "  up.  The feed is observable as a viewport extension of 1240 steps",
        "  and 155 mm of paper.  No tail stroke is drawn by the model.",
        "- step: the fine step doubles the roll limit in steps (2720 to",
        "  5440) and halves millimetres per step.  The SVG comment records",
        "  `step 0.0625 mm` against `step 0.125 mm`.",
        "- jam: three frames of +1023 steps exceed the 2720-step roll, y is",
        "  clamped at 2720, guest УВВ 04170 returns 01, and the device reset",
        "  clears the flag so the next read returns 0.",
        "",
        "## Case facts",
        "",
        "```text",
    ]
    for name in sorted(all_facts):
        lines.append("case %s %s" %
                     (name, signature(all_facts[name])))
    lines += ["```", ""]
    GOLDEN.parent.mkdir(parents=True, exist_ok=True)
    GOLDEN.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=3,
                        help="repetitions per case for determinism (default: 3)")
    parser.add_argument("--update-golden", action="store_true",
                        help="recompute EXPECTED.md after all checks pass")
    parser.add_argument("--keep-scratch", action="store_true",
                        help="keep the per-run besm6 directories for inspection")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if not EMU.is_file():
        print("FAIL: simulator not found: %s" % EMU)
        return 1

    golden = read_golden()
    failures = []
    all_facts = {}
    print("simulator: %s" % EMU)
    scratch = Path(tempfile.mkdtemp(prefix="rpg-features-run-", dir="/var/tmp"))
    print("scratch: %s" % scratch)

    for name, ini_name in CASES.items():
        ini = TESTS / ini_name
        observed = []
        for attempt in range(args.runs):
            facts, work = run_once(name, ini, scratch)
            observed.append(facts)
            if attempt == 0:
                print("case %-14s work=%s" % (name, work))
        first = signature(observed[0])
        if any(signature(item) != first for item in observed[1:]):
            print("FAIL: %s, %d runs are not deterministic" %
                  (name, args.runs))
            failures.append("%s determinism" % name)
        all_facts[name] = observed[0]
        svg = observed[0]["svg"]
        print("  %d/%d identical runs, sha=%s size=%s" %
              (sum(signature(item) == first for item in observed),
               args.runs,
               svg["sha256"][:16] if svg else "none",
               svg["size"] if svg else "none"))
        print("  markers=%s acc=%s" %
              (observed[0]["markers"], observed[0]["acc"]))
        if name in golden and golden[name] != observed[0]:
            print("FAIL: %s, facts differ from EXPECTED.md" % name)
            failures.append("%s golden" % name)

    verify_relations(all_facts, failures)

    if failures:
        print("FAIL: %d problem(s): %s" % (len(failures), ", ".join(failures)))
        print("scratch kept for inspection: %s" % scratch)
        return 1
    if not args.keep_scratch:
        shutil.rmtree(scratch)
    if not golden:
        print("NOTE: no golden facts found, run with --update-golden")
    if args.update_golden:
        write_golden(all_facts)
        print("golden updated: %s" % GOLDEN)
    print("PASS: all RPG feature checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
