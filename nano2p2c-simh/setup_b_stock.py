#!/usr/bin/env python3
"""Build an isolated SIMH ini for VARIANT B (real Fortran source decks).

Every attached image is a workspace COPY: no live disk, no repo writes, no C
patch. Stock `attach -n prn0/prn1` semantics are preserved; only the capture
paths are redirected into this workspace. The deck reader `vu0` is commented
out here and attached at runtime by drive_b.py via `at vu0 <deck>`.

Usage: python3 setup_b.py CASE MD41_IMAGE [--trace]
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
from pathlib import Path

WS = Path(__file__).resolve().parent
SIMH = Path("/home/azizz/Yandex.Disk/simh/BESM6")
BASE = SIMH / "OSZAGR" / "dispak.ini"
IMG = WS / "images"
RUNS = WS / "runs"
RUNS.mkdir(parents=True, exist_ok=True)
(RUNS / "tmux").mkdir(parents=True, exist_ok=True)
IMG.mkdir(parents=True, exist_ok=True)

# 2348 fallback locations, first existing wins.
D45_SOURCES = (
    "/usr/local/share/besm6/2348",
    Path.home() / ".besm6" / "2348",
    SIMH / "MD" / "EC5061" / "2348",
)

ATTACH_RE = re.compile(r"^(\s*)(;?\s*)(attach\s+\S+\s+\S+\s+)(\S+)(.*)$")


def _workspace_copy(src, name=None):
    """Copy src into IMG (once) and return the workspace path."""
    p = Path(os.path.expanduser(str(src))).resolve()
    if not p.is_file():
        raise FileNotFoundError(f"image not found: {p}")
    dst = IMG / (name or p.name)
    if p != dst and not dst.exists():
        shutil.copy2(p, dst)
    return dst


def make_ini(case, md41_image, trace=False):
    """Return the generated ini Path. md41_image is always copied locally."""
    md41_src = Path(os.path.expanduser(str(md41_image))).resolve()
    if not md41_src.is_file():
        raise FileNotFoundError(f"md41 image not found: {md41_src}")
    tag = hashlib.sha1(str(md41_src).encode()).hexdigest()[:8]
    md41_local = (md41_src if md41_src.parent == IMG
                  else _workspace_copy(md41_src, f"md41_{case}_{tag}_{md41_src.name}"))

    d45_src = next((Path(s) for s in D45_SOURCES if Path(s).is_file()), None)
    if d45_src is None:
        raise FileNotFoundError("no 2348 image found for md45")
    d45_local = _workspace_copy(d45_src, "md45_2348")

    prn0 = RUNS / f"localprn_{case}.txt"
    prn1 = RUNS / f"localprn1_{case}.txt"
    out = []
    for line in BASE.read_text(encoding="utf-8").splitlines():
        m = ATTACH_RE.match(line)
        if m:
            ind, com, head, path, tail = m.groups()
            if com.strip():
                out.append(line)
                continue
            if re.search(r"\bprn0\b", head):
                # Stock -n semantics, workspace-local capture path.
                out.append(f"{ind}{head}{prn0}{tail}")
                continue
            if re.search(r"\bprn1\b", head):
                out.append(f"{ind}{head}{prn1}{tail}")
                continue
            if re.search(r"attach\s+-n\s+vu0\b", head):
                out.append(f"{ind}; {head}{path}{tail}")  # attached at runtime
                continue
            if re.search(r"\bmd41\b", head):
                new = md41_local
            elif re.search(r"\bmd45\b", head):
                new = d45_local
            elif re.search(r"\bmd07\b", head):
                new = _workspace_copy(Path.home() / ".besm6" / "2053", "md07_2053")
            else:
                try:
                    new = _workspace_copy(path)
                except FileNotFoundError:
                    # Missing optional stock media (e.g. mg57 -> ML/204): keep
                    # the device unmounted rather than attaching a dead path.
                    out.append(f"{ind}; {head}{path}{tail}  ; source missing")
                    continue
            out.append(f"{ind}{head}{new}{tail}")
            continue

        if re.match(r"^\s*load\s+", line):
            p = line.split(None, 1)[1].strip()
            out.append("load " + str(_workspace_copy(p)))
            continue

        if "console log=" in line:
            # set cpu debug echoes here and the log grows to ~86 MB; the driver
            # captures the pane through tmux instead.
            out.append("; " + line)
            continue

        out.append(line)

    if trace:
        out.append(f"set debug {RUNS / ('debug_' + case + '.txt')}")
        out.append("set cpu debug")

    ini = RUNS / f"ini_{case}.ini"
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")
    return ini


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("case")
    ap.add_argument("md41_image")
    ap.add_argument("--trace", action="store_true")
    a = ap.parse_args()
    ini = make_ini(a.case, a.md41_image, trace=a.trace)
    print(ini)
    for line in ini.read_text(encoding="utf-8").splitlines():
        if re.search(r"attach -e md(41|45)|attach -n prn0|attach -n prn1|attach -n vu0|^load ", line):
            print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
