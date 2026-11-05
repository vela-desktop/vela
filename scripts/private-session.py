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


class InvalidSessionArguments(Exception):
    pass


def _validated_command(argv):
    """The only accepted operation is our compositor, optionally with -s."""
    if len(argv) not in (1, 3):
        raise InvalidSessionArguments("Expected vela-compositor [-s vela-shell]")
    compositor = Path(argv[0]).resolve(strict=True)
    if (compositor.name != "vela-compositor" or compositor.parent.name != "compositor"
            or not os.access(compositor, os.X_OK)):
        raise InvalidSessionArguments("Expected an executable build/compositor/vela-compositor")
    shell = None
    if len(argv) == 3:
        if argv[1] != "-s":
            raise InvalidSessionArguments("Only -s is supported")
        shell = Path(argv[2]).resolve(strict=True)
        if (shell.name != "vela-shell" or shell.parent.name != "shell"
                or shell.parent.parent != compositor.parent.parent
                or not os.access(shell, os.X_OK)):
            raise InvalidSessionArguments("Expected an executable sibling build/shell/vela-shell")
    return compositor, shell

GRACE_SECONDS = 2.0
KILL_SECONDS = 2.0
_requested_signal = None


def _on_signal(signum, _frame):
    """Record the first requested termination without interrupting cleanup."""
    global _requested_signal
    if _requested_signal is None:
        _requested_signal = signum


def _subreaper():
    """Adopt orphaned compositor children, including double-forked clients."""
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_CHILD_SUBREAPER) failed")


def _copy_entry(source, destination, boundary, parents):
    """Copy in-bound Vela settings as real files, stopping promptly on signals."""
    if _requested_signal is not None:
        return
    try:
        target = source.resolve(strict=True)
        try:
            target.relative_to(boundary)
        except ValueError:
            print(f"Skipping config symlink outside Vela: {source}", file=sys.stderr)
            return
        mode = target.stat().st_mode
        if stat.S_ISDIR(mode):
            if target in parents:
                print(f"Skipping recursive config symlink: {source}", file=sys.stderr)
                return
            destination.mkdir(mode=0o700)
            for child in target.iterdir():
                if _requested_signal is not None:
                    break
                _copy_entry(child, destination / child.name, boundary, parents | {target})
        elif stat.S_ISREG(mode):
            # Copy in bounded chunks: shutil.copyfile can block termination for
            # an arbitrarily large configuration file.
            with target.open("rb") as source_file, destination.open("wb") as destination_file:
                while _requested_signal is None:
                    block = source_file.read(1024 * 1024)
                    if not block or _requested_signal is not None:
                        break
                    destination_file.write(block)
        else:
            print(f"Skipping non-file config entry: {source}", file=sys.stderr)
    except (OSError, RuntimeError) as error:
        # A broken/unreadable symlink is not usable as a setting.
        print(f"Skipping config entry {source}: {error}", file=sys.stderr)


def _copy_config(real_config, private_config):
    """Import only the two recognized Vela configuration roots."""
    for name in ("vela", "Vela"):
        if _requested_signal is not None:
            break
        source = real_config / name
        if source.is_symlink():
            print(f"Skipping symlinked config root: {source}", file=sys.stderr)
        elif source.is_dir():
            _copy_entry(source, private_config / name, source.resolve(), frozenset())

def _descendants():
    """Enumerate descendants and their identities using procfs PPIDs/start times.

    Process PPIDs may change during the scan. The direct-child waitpid check in
    _stop_all is therefore authoritative when deciding whether cleanup is safe.
    """
    parent_of = {}
    with os.scandir("/proc") as entries:
        for entry in entries:
            if not entry.name.isdecimal():
                continue
            try:
                fields = Path(entry.path, "stat").read_text().rsplit(")", 1)[1].split()
                parent_of[int(entry.name)] = (int(fields[1]), int(fields[19]))
            except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError, ValueError):
                continue
    descendants = set()
    frontier = {os.getpid()}
    while frontier:
        found = {pid for pid, (parent, _) in parent_of.items()
                 if parent in frontier and pid not in descendants}
        descendants.update(found)
        frontier = found
    return {pid: parent_of[pid][1] for pid in descendants}


def _reap(process):
    """Reap exited direct children; report whether any direct child remains.

    With PR_SET_CHILD_SUBREAPER, every living descendant has a living direct
    child ancestor (or becomes our own child when its parent exits). Thus a
    ChildProcessError after reaping means no session descendant can survive,
    even if a procfs scan missed a concurrent reparenting.
    """
    while True:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return False
        if pid == 0:
            return True
        if pid == process.pid:
            process.returncode = os.waitstatus_to_exitcode(status)


def _require_pidfds():
    """Reject unsupported platforms before launching any session process."""
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise OSError("Private session supervision needs Python 3.9+ and Linux 5.3+ (pidfd)")
    fd = os.pidfd_open(os.getpid())
    try:
        signal.pidfd_send_signal(fd, 0)
    finally:
        os.close(fd)


def _signal_descendant(pid, started, signum):
    """Signal the exact scanned process, not another process reusing its PID."""
    try:
        fd = os.pidfd_open(pid)
    except ProcessLookupError:
        return
    try:
        try:
            current = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            if int(current[19]) != started:
                return
        except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
            return
        try:
            signal.pidfd_send_signal(fd, signum)
        except ProcessLookupError:
            pass
    finally:
        os.close(fd)


def _stop_all(process):
    """Stop all descendants, and prove none remains before removing their home.

    A single empty procfs snapshot is insufficient: only after waitpid confirms
    that the subreaper has no direct children can a scan be considered quiescent.
    """
    for signum, duration in ((signal.SIGTERM, GRACE_SECONDS), (signal.SIGKILL, KILL_SECONDS)):
        deadline = time.monotonic() + duration
        while True:
            has_children = _reap(process)
            descendants = _descendants()
            if not has_children and not descendants:
                return True
            for pid, started in descendants.items():
                _signal_descendant(pid, started, signum)
            if time.monotonic() >= deadline:
                break
            time.sleep(0.05)
    return not _reap(process) and not _descendants()

def main(argv):
    """Validate, launch, supervise and clean up one disposable Vela session."""
    compositor, shell = _validated_command(argv)
    if os.environ.get("VELA_TEST_COPY_CONFIG", "0") not in ("0", "1"):
        raise InvalidSessionArguments("VELA_TEST_COPY_CONFIG must be 0 or 1")
    real_config = os.environ.get("XDG_CONFIG_HOME")
    if not real_config:
        if not os.environ.get("HOME"):
            raise InvalidSessionArguments("HOME must be set")
        real_config = os.path.join(os.environ["HOME"], ".config")

    _require_pidfds()
    _subreaper()
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, _on_signal)

    # tempfile chooses valid TMPDIR automatically, creating a private (0700)
    # directory atomically even when its parent is publicly writable.
    root = Path(tempfile.mkdtemp(prefix="vela-session."))
    safe_to_remove = True
    process = None
    stopped = False
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
        env["VELA_PRIVATE_COMPOSITOR"] = str(compositor)
        if shell is not None:
            env["VELA_PRIVATE_SHELL"] = str(shell)
        else:
            env.pop("VELA_PRIVATE_SHELL", None)
        process = subprocess.Popen(
            ["/bin/sh", str(Path(__file__).with_name("private-session-exec.sh"))],
            env=env)
        while process.returncode is None and _requested_signal is None:
            _reap(process)
            if process.returncode is not None:
                break
            time.sleep(0.05)
        status = (128 + _requested_signal if _requested_signal is not None
                  else (128 - process.returncode if process.returncode < 0 else process.returncode))
        try:
            stopped = _stop_all(process)
        except OSError:
            safe_to_remove = False
            raise
        if not stopped:
            safe_to_remove = False
            print("Session descendants survived SIGKILL", file=sys.stderr)
            return 1
        return 128 + _requested_signal if _requested_signal is not None else status
    finally:
        # Also supervise partial startup if an unexpected error interrupted the
        # normal path after Popen. Never delete a home with live descendants.
        if process is not None and not stopped and safe_to_remove:
            try:
                if not _stop_all(process):
                    safe_to_remove = False
            except OSError as error:
                safe_to_remove = False
                print(f"Failed to stop session descendants: {error}", file=sys.stderr)
        if safe_to_remove:
            shutil.rmtree(root)
        else:
            print(f"Preserving private session directory: {root}", file=sys.stderr)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except InvalidSessionArguments as error:
        print(f"private-session: {error}", file=sys.stderr)
        sys.exit(2)
    except (OSError, ValueError) as error:
        print(f"private-session: {error}", file=sys.stderr)
        sys.exit(1)
