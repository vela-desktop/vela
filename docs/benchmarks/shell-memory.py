# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run one sample of a headless desktop workload in docs/shell-memory.md."""

import argparse
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "functional"))
import harness  # noqa: E402
import test_shell  # noqa: E402

# One 4096×4096 RGB image-data hint is a 48 MiB message, larger than
# dbus-daemon's default 32 MiB limit.
harness._BUS_CONFIG = harness._BUS_CONFIG.replace(
    "{services}</busconfig>",
    '  <limit name="max_message_size">134217728</limit>\n'
    '  <limit name="max_incoming_bytes">268435456</limit>\n'
    '  <limit name="max_outgoing_bytes">268435456</limit>\n'
    "{services}</busconfig>")


class Desktop(test_shell.RealShell):
    shell_settings = "[clipboard]\nhistory=true\n"
    shell_env = {"QSG_RENDER_TIMING": "1"}
    pins = 0

    def prepare_shell(self):
        desktop = Path(self.vela.directory, "Desktop")
        for i in range(24):
            (desktop / f"Example document {i:02d}.txt").touch()
        if self.pins:
            # Numbered PNG pins, which every shell revision loads.
            directory = Path(self.vela.env["XDG_DATA_HOME"], "vela", "clipboard")
            directory.mkdir(parents=True)
            for i in range(1, self.pins + 1):
                (directory / f"{i}.png").write_bytes(screenshot_png(1600, 900, i))
            (directory / "pinned.json").write_text(json.dumps([{"image": f"{i}.png"} for i in range(1, self.pins + 1)]))


def screenshot_png(width, height, seed):
    """An RGB PNG of 100-pixel blocks with a vertical gradient, quick to encode."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        green = (y * 255 // height + seed * 37) & 255
        for x0 in range(0, width, 100):
            red = (x0 * 255 // width + seed * 11) & 255
            blue = ((x0 // 100 + y // 100 + seed) * 53) & 255
            rows += bytes((red, green, blue)) * (min(width, x0 + 100) - x0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 1)) + chunk(b"IEND", b""))


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


def check_vulkan(desktop):
    if "vela-shell: Qt Quick renders with Vulkan" not in desktop.vela.log_text():
        raise RuntimeError("the shell did not report the Vulkan scene graph")


def sample(desktop):
    """Idle, then five opens and closes of Notification Center."""
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


def clipboard_sample(desktop):
    """25 pinned 1600×900 screenshots: idle, then with clipboard history open."""
    time.sleep(3)
    result = {"idle_KiB": memory_kib(desktop.shell.pid)}
    desktop.shell_command("clipboard")
    desktop.vela.wait_for(lambda s: desktop.mapped(s, "vela-clipboard"), what="clipboard history")
    time.sleep(3)
    result["open_KiB"] = memory_kib(desktop.shell.pid)
    return result


def notification_sample(desktop):
    """Idle, then one 4096×4096 RGB notification, shown in Notification Center."""
    import dbus

    time.sleep(2)
    result = {"idle_KiB": memory_kib(desktop.shell.pid)}
    side = 4096
    row = b"".join(bytes((x * 255 // side, 128, 255 - x * 255 // side)) for x in range(side))
    image = dbus.Struct((side, side, side * 3, False, 8, 3, dbus.ByteArray(row * side)), signature="iiibiiay")
    bus = dbus.bus.BusConnection(desktop.bus.address)
    proxy = bus.get_object("org.freedesktop.Notifications", "/org/freedesktop/Notifications")
    proxy.Notify("Benchmark", dbus.UInt32(0), "", "Large image", "4096×4096 RGB", dbus.Array([], signature="s"),
                 {"image-data": image}, dbus.Int32(-1), dbus_interface="org.freedesktop.Notifications", timeout=60)
    del image
    time.sleep(8)  # the popup expires into history
    desktop.shell_command("notification-center")
    desktop.vela.wait_for(lambda s: desktop.mapped(s, "vela-notification-center"), what="notification center")
    time.sleep(2)
    result["center_KiB"] = memory_kib(desktop.shell.pid)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shell", type=Path, help="vela-shell executable to measure")
    parser.add_argument("--workload", choices=("desktop", "clipboard", "notification"), default="desktop")
    args = parser.parse_args()
    test_shell.SHELL = str(args.shell.resolve(strict=True))
    if args.workload == "clipboard":
        Desktop.pins = 25
    desktop = Desktop()
    try:
        desktop.setUp()
        measure = {"desktop": sample, "clipboard": clipboard_sample, "notification": notification_sample}[args.workload]
        result = measure(desktop)
        check_vulkan(desktop)
        print(json.dumps(result))
    finally:
        desktop.doCleanups()
