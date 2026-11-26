# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""One sample of docs/shell-graphics-memory.md: the shell's VRAM and GTT
with panels opened and closed, and how soon they open again.

    python3 docs/benchmarks/shell-graphics-memory.py build/shell/vela-shell

Prints one JSON object. The output is headless, 1920x1080 at 100 Hz."""

import argparse
import json
from pathlib import Path
import re
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "functional"))
import test_shell  # noqa: E402

PANELS = (("toggle-start", "vela-start-menu"), ("notification-center", "vela-notification-center"),
          ("quick-settings", "vela-quick-settings"), ("clipboard", "vela-clipboard"),
          ("task-view", "vela-task-view"))
KEPT = 10  # panelGraphicsKept in shell/src/main.cpp
COMMIT = re.compile(r"^\[(\d+):(\d+):(\d+\.\d+)\].*-> wl_surface#(\d+)\.commit\(\)", re.M)


class Desktop(test_shell.RealShell):
    shell_settings = "[clipboard]\nhistory=true\n"

    def runTest(self):
        pass


def memory_mib(pid):
    """drm-resident-vram and -gtt of the process's DRM clients, in MiB."""
    clients, total = set(), {}
    for fd in Path("/proc", str(pid), "fdinfo").iterdir():
        try:
            info = fd.read_text()
        except FileNotFoundError:
            continue
        client = re.search(r"^drm-client-id:\s+(\d+)", info, re.M)
        if not client or client[1] in clients:
            continue
        clients.add(client[1])
        for region, kib in re.findall(r"^drm-resident-(vram|gtt):\s+(\d+) KiB", info, re.M):
            total[region] = total.get(region, 0) + int(kib)
    return {region: round(kib / 1024, 1) for region, kib in total.items()}


def toggle(desktop, command, namespace, opening):
    """Milliseconds from the shell command to the compositor's (un)mapping."""
    start = time.monotonic()
    desktop.shell_command(command)
    while desktop.mapped(desktop.vela.state(), namespace) != opening:
        if time.monotonic() - start > 5:
            raise TimeoutError(f"{namespace} did not {'map' if opening else 'unmap'}")
        time.sleep(0.002)
    return round((time.monotonic() - start) * 1000, 1)


def open_all(desktop):
    times = {}
    for command, namespace in PANELS:
        times[namespace] = toggle(desktop, command, namespace, True)
        time.sleep(0.6)
        toggle(desktop, command, namespace, False)
        time.sleep(0.4)
    return times


def start_frames(desktop):
    """Gaps between Start's commits while it opens at 300% duration."""
    mark = len(desktop.vela.log_text())
    desktop.shell_command("toggle-start")
    time.sleep(0.7)
    text = desktop.vela.log_text()[mark:]
    desktop.shell_command("toggle-start")
    desktop.vela.wait_for(lambda s: not desktop.mapped(s, "vela-start-menu"), what="Start closed")
    surfaces = {}
    for hours, minutes, seconds, surface in COMMIT.findall(text):
        surfaces.setdefault(surface, []).append(int(hours) * 3600 + int(minutes) * 60 + float(seconds))
    commits = max(surfaces.values(), key=len)
    gaps = [(b - a) * 1000 for a, b in zip(commits, commits[1:])][4:]  # after the first frames' burst
    return {"median_ms": round(statistics.median(gaps), 1), "max_ms": round(max(gaps), 1)}


def sample(desktop):
    time.sleep(2.5)
    pid = desktop.shell.pid
    result = {"idle": memory_mib(pid), "first_open_ms": open_all(desktop)}
    result["closed_1s"] = memory_mib(pid)
    result["reopen_within_ms"] = open_all(desktop)
    time.sleep(KEPT + 1)
    result["closed_11s"] = memory_mib(pid)
    result["reopen_after_ms"] = open_all(desktop)
    time.sleep(KEPT + 1)
    desktop.slow_animations()
    result["start_frames_after"] = start_frames(desktop)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("shell", type=Path, help="vela-shell executable to measure")
    args = parser.parse_args()
    test_shell.SHELL = str(args.shell.resolve(strict=True))
    desktop = Desktop()
    try:
        desktop.setUp()
        print(json.dumps(sample(desktop)))
    finally:
        desktop.doCleanups()
