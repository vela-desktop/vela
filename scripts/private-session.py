#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Own the lifecycle of a disposable, *non-sandboxed* Vela test session.

Linux subreaper supervision is intentional: Vela can double-fork/setsid its
clients, so killing only the compositor or its process group is insufficient.
"""

import ctypes
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

PR_SET_CHILD_SUBREAPER = 36
GRACE_SECONDS = 2.0
KILL_SECONDS = 2.0
_requested_signal = None


def _on_signal(signum, _frame):
    global _requested_signal
    if _requested_signal is None:
        _requested_signal = signum


def _subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_CHILD_SUBREAPER) failed")


def _copy_entry(source, destination, boundary, parents):
    try:
        target = source.resolve(strict=True)
        if not target.is_relative_to(boundary):
            print(f"Skipping config symlink outside Vela: {source}", file=sys.stderr)
            return
        mode = target.stat().st_mode
        if stat.S_ISDIR(mode):
            if target in parents:
                print(f"Skipping recursive config symlink: {source}", file=sys.stderr)
                return
            destination.mkdir(mode=0o700)
            for child in target.iterdir():
                _copy_entry(child, destination / child.name, boundary, parents | {target})
        elif stat.S_ISREG(mode):
            shutil.copyfile(target, destination)
        else:
            print(f"Skipping non-file config entry: {source}", file=sys.stderr)
    except (OSError, RuntimeError) as error:
        # A broken/unreadable symlink is not usable as a setting.
        print(f"Skipping config entry {source}: {error}", file=sys.stderr)


def _copy_config(real_config, private_config):
    for name in ("vela", "Vela"):
        source = real_config / name
        if source.is_symlink():
            print(f"Skipping symlinked config root: {source}", file=sys.stderr)
        elif source.is_dir():
            _copy_entry(source, private_config / name, source.resolve(), frozenset())


def _descendants():
    """Find descendants despite setsid()/double-fork and /proc children gaps.

    /proc/PID/task/PID/children is not available on every Linux kernel; the
    PPID field of /proc/PID/stat is the portable procfs fallback.
    """
    parent_of = {}
    with os.scandir("/proc") as entries:
        for entry in entries:
            if not entry.name.isdecimal():
                continue
            try:
                raw = Path(entry.path, "stat").read_text()
                parent_of[int(entry.name)] = int(raw.rsplit(")", 1)[1].split()[1])
            except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError, ValueError):
                continue
    descendants = set()
    frontier = {os.getpid()}
    while frontier:
        found = {pid for pid, parent in parent_of.items() if parent in frontier and pid not in descendants}
        descendants.update(found)
        frontier = found
    return descendants


def _reap(process):
    while True:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return
        if pid == process.pid:
            process.returncode = os.waitstatus_to_exitcode(status)


def _stop_all(process):
    """Bounded shutdown; don't delete files while a client may recreate them."""
    for signum, duration in ((signal.SIGTERM, GRACE_SECONDS), (signal.SIGKILL, KILL_SECONDS)):
        deadline = time.monotonic() + duration
        while True:
            _reap(process)
            descendants = _descendants()
            if not descendants:
                return True
            for pid in descendants:
                try:
                    os.kill(pid, signum)
                except ProcessLookupError:
                    pass
            if time.monotonic() >= deadline:
                break
            time.sleep(0.05)
    _reap(process)
    return not _descendants()


def main(argv):
    if not argv:
        print("Usage: private-session.py compositor [arguments ...]", file=sys.stderr)
        return 2
    if os.environ.get("VELA_TEST_COPY_CONFIG", "0") not in ("0", "1"):
        print("VELA_TEST_COPY_CONFIG must be 0 or 1", file=sys.stderr)
        return 2
    temp_parent = os.environ.get("TMPDIR", "/tmp")
    if not os.path.isabs(temp_parent):
        print("TMPDIR must be an absolute path", file=sys.stderr)
        return 2
    real_config = os.environ.get("XDG_CONFIG_HOME")
    if not real_config:
        if not os.environ.get("HOME"):
            print("HOME must be set", file=sys.stderr)
            return 2
        real_config = os.path.join(os.environ["HOME"], ".config")

    _subreaper()
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, _on_signal)

    root = Path(tempfile.mkdtemp(prefix="vela-session.", dir=temp_parent))
    safe_to_remove = True
    try:
        env = os.environ.copy()
        for key, leaf in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                          ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                          ("XDG_STATE_HOME", "state")):
            path = root / leaf
            path.mkdir(mode=0o700)
            env[key] = str(path)
        if os.environ.get("VELA_TEST_COPY_CONFIG") == "1":
            _copy_config(Path(real_config), Path(env["XDG_CONFIG_HOME"]))

        if _requested_signal is not None:
            return 128 + _requested_signal
        process = subprocess.Popen(argv, env=env)
        while process.returncode is None and _requested_signal is None:
            _reap(process)
            if process.returncode is not None:
                break
            time.sleep(0.05)
        status = (128 + _requested_signal if _requested_signal is not None
                  else (128 - process.returncode if process.returncode < 0 else process.returncode))
        if not _stop_all(process):
            safe_to_remove = False
            print(f"Session descendants survived SIGKILL; preserving {root}", file=sys.stderr)
            return 1
        return status
    finally:
        if safe_to_remove:
            shutil.rmtree(root)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, ValueError) as error:
        print(f"private-session: {error}", file=sys.stderr)
        sys.exit(1)
