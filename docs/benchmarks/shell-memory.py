# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run one sample of the headless desktop workload in docs/shell-memory.md."""

import argparse
import json
import math
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "functional"))
import test_shell  # noqa: E402


class Desktop(test_shell.RealShell):
    shell_settings = "[clipboard]\nhistory=true\n"
    shell_env = {"QSG_RENDER_TIMING": "1"}

    def prepare_shell(self):
        desktop = Path(self.vela.directory, "Desktop")
        for i in range(24):
            (desktop / f"Example document {i:02d}.txt").touch()


def memory_kib(pid):
    proc = Path("/proc", str(pid))
    fields = re.findall(r"^(Pss|Private_Dirty):\s+(\d+) kB", (proc / "smaps_rollup").read_text(), re.M)
    result = {key: int(value) for key, value in fields}
    clients = set()
    for fd in (proc / "fdinfo").iterdir():
        try:
            info = fd.read_text()
        except FileNotFoundError:
            continue
        client = re.search(r"^drm-client-id:\s+(\d+)", info, re.M)
        if not client:
            continue
        device = re.search(r"^drm-pdev:\s+(\S+)", info, re.M)
        key = (device[1] if device else None, client[1])
        if key in clients:
            continue
        clients.add(key)
        for region, kib in re.findall(r"^(drm-resident-\S+):\s+(\d+) KiB", info, re.M):
            result[region] = result.get(region, 0) + int(kib)
    return result


def sample(desktop):
    time.sleep(2)
    result = {"idle_KiB": memory_kib(desktop.shell.pid), "open_ms": []}
    desktop.vela.screenshot(str(Path(desktop.vela.directory, "desktop.png")))
    for _ in range(5):
        start = time.monotonic()
        desktop.shell_command("notification-center")
        while not desktop.mapped(desktop.vela.state(), "vela-notification-center"):
            if time.monotonic() - start > 3:
                raise TimeoutError("notification center did not map")
            time.sleep(0.002)
        result["open_ms"].append(round((time.monotonic() - start) * 1000, 2))
        time.sleep(0.3)
        desktop.shell_command("notification-center")
        desktop.vela.wait_for(lambda s: not desktop.mapped(s, "vela-notification-center"), what="center closed")
    result["after_KiB"] = memory_kib(desktop.shell.pid)
    frames = sorted(map(int, re.findall(r"syncAndRender: frame rendered in (\d+)ms", desktop.vela.log_text())))
    if not frames:
        raise RuntimeError("Qt did not report frame times")
    result["rendered_frames"] = len(frames)
    result["render_ms"] = {f"p{p}": frames[math.ceil(len(frames) * p / 100) - 1] for p in (50, 95, 99)}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shell", type=Path, help="vela-shell executable to measure")
    args = parser.parse_args()
    test_shell.SHELL = str(args.shell.resolve(strict=True))
    desktop = Desktop()
    try:
        desktop.setUp()
        print(json.dumps(sample(desktop)))
    finally:
        desktop.doCleanups()
