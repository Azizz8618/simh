"""pexpect-backed replacement for the tmux calls used by the NITI test
runners (the sandbox has no tmux binary and no root for apt).

Supported invocations (same argv shapes as the runners use):
  tmux(env, "new-session", "-d", "-s", S, "-x", "200", "-y", "50", CMD)
  tmux(env, "capture-pane", "-p", "-S", "-N", "-t", S, check=False)
  tmux(env, "send-keys", "-t", S, "-l", TEXT)
  tmux(env, "send-keys", "-t", S, "Enter")
  tmux(env, "send-keys", "-t", S, "C-g")
  tmux(env, "kill-session", "-t", S, check=False)

One session per process; CMD has the form "cd DIR && VAR=v BIN ARGS...".
"""
import os
import re
import shlex
import threading
from collections import deque

import pexpect

_child = None
_buf = deque(maxlen=4000)
_lock = threading.Lock()
_alive = False


def _reader():
    global _alive
    while _alive:
        try:
            chunk = _child.read_nonblocking(size=4096, timeout=0.2)
        except pexpect.TIMEOUT:
            continue
        except (pexpect.EOF, OSError):
            _alive = False
            return
        with _lock:
            _buf.extend(chunk.replace("\r\n", "\n").replace("\r", "\n").split("\n"))


def tmux(env, *args, check=True):
    global _child, _alive
    op = args[0]
    if op == "new-session":
        # "cd DIR && VAR=v BIN ARGS"
        cmd = args[-1]
        m = re.match(r"\s*cd\s+(\S+)\s*&&\s*(.+)$", cmd)
        cwd = m.group(1) if m else None
        rest = m.group(2) if m else cmd
        tokens = shlex.split(rest)
        child_env = dict(env or os.environ)
        while tokens and re.match(r"[A-Za-z_][A-Za-z0-9_]*=", tokens[0]):
            k, v = tokens[0].split("=", 1)
            child_env[k] = v
            tokens.pop(0)
        binpath, binargs = tokens[0], tokens[1:]
        rows = cols = None
        if "-y" in args:
            rows = int(args[args.index("-y") + 1])
        if "-x" in args:
            cols = int(args[args.index("-x") + 1])
        _child = pexpect.spawn(
            binpath, binargs, cwd=cwd, env=child_env, encoding="utf-8",
            timeout=10, dimensions=(rows or 50, cols or 200))
        with _lock:
            _buf.clear()
        _alive = True
        threading.Thread(target=_reader, daemon=True).start()
        return ""
    if op == "capture-pane":
        n = 2000
        if "-S" in args:
            n = min(int(args[args.index("-S") + 1].lstrip("-")), 4000)
        with _lock:
            lines = list(_buf)
        return "\n".join(lines[-n:])
    if op == "send-keys":
        target = args[args.index("-t") + 1]
        rest = args[args.index("-t") + 2:]
        if "-l" in rest:
            idx = rest.index("-l")
            _child.send(rest[idx + 1])
        elif "Enter" in rest:
            _child.sendline("")
        elif "C-g" in rest:
            _child.send("\x07")
        return ""
    if op == "kill-session":
        if _child is not None:
            try:
                _child.terminate(force=True)
            except Exception:
                pass
            _alive = False
        return ""
    raise RuntimeError("tmux_shim: unsupported op %r" % (op,))
