"""Shared disk-image lookup for the NITI test runners.

The old sandbox had /usr/local/share/besm6/<vol>; the recycled one keeps
the images under /home/z/my-project/disks and the coordinating aesc-crypto
project (whose 2348 image serves as volume 2048).
"""
import os
from pathlib import Path

_BASES = (
    "/usr/local/share/besm6",
    "/home/z/my-project/disks",
    "/home/z/my-project/repos/aesc-crypto-projects/disks",
)


def find_disk(name):
    """Env override (NITI_2348 for 2348, GPSTAR_SYS_<name> otherwise),
    then the known bases; last resort: the historic /usr/local path."""
    env = os.environ.get("NITI_2348") if name == "2348" \
        else os.environ.get("GPSTAR_SYS_%s" % name)
    if env:
        return Path(env)
    for base in _BASES:
        p = Path(base) / name
        if p.exists():
            return p
    return Path("/usr/local/share/besm6/%s" % name)
