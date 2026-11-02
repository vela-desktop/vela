# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed input/capture instrument checks; no desktop ranking."""

import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import select
import resource
import re
import struct
import subprocess
import time

from . import BenchError
from .archive import atomic_json, decode_json
from .conditions import connector
from .diagnostics import distribution, parse_probe, stop_instrument, user_command

INPUTS = 20
CAPTURES = 12


class Lines:
    def __init__(self, process, *, max_record_bytes=65536):
        self.process, self.pending, self.records = process, b"", []
        self.max_record_bytes = max_record_bytes

    def next(self, deadline):
        while b"\n" not in self.pending:
            if time.monotonic() >= deadline:
                raise BenchError("Instrument output deadline")
            ready, _, _ = select.select([self.process.stdout], [], [], min(.1, max(0, deadline - time.monotonic())))
            if not ready:
                continue
            data = os.read(self.process.stdout.fileno(), 4096)
            if not data:
                raise BenchError("Instrument stream ended before its expected record")
            self.pending += data
            if len(self.pending) > self.max_record_bytes:
                raise BenchError("Oversized instrument record")
        raw, self.pending = self.pending.split(b"\n", 1)
        row = decode_json(raw)
        if not isinstance(row, dict):
            raise BenchError("Invalid instrument record")
        self.records.append(row)
        return row


def parse_input(rows, injections, *, count=INPUTS, delay=0):
    ready = [row for row in rows if row.get("event") == "input-ready"]
    inputs = [row for row in rows if row.get("event") == "input"]
    if len(ready) != 1 or len(inputs) != count or len(injections) != count:
        raise BenchError("Lost or unexpected input actions")
    if type(ready[0].get("monotonic_ns")) is not int or ready[0]["monotonic_ns"] < 0:
        raise BenchError("Invalid input readiness clock")
    raw = "".join(json.dumps(row) + "\n" for row in rows if row.get("event") not in ("input-ready", "input"))
    metrics = parse_probe(raw, frames=count + 1, delay=delay, method="wayland-input-presentation-v1",
                          scope="software-input-to-reported-presentation")
    if metrics["clock"]["presentation_clock_id"] != metrics["clock"]["monotonic_clock_id"] or metrics["clock"]["monotonic_clock_id"] != 1:
        raise BenchError("Input and presentation do not establish a shared CLOCK_MONOTONIC domain")
    committed = [row for row in rows if row.get("event") == "commit"]
    if any(type(row.get("input_id")) is not int for row in committed):
        raise BenchError("Invalid input submission identity")
    commits = {row["input_id"]: row for row in committed}
    presented = {row["frame"]: row for row in rows if row.get("event") == "presented"}
    if set(commits) != set(range(count + 1)):
        raise BenchError("Input actions do not map one-to-one to visual submissions")
    upper, lower, receipt, uncertainty = [], [], [], []
    for action, received, injection in zip(range(1, count + 1), inputs, injections):
        if (type(received.get("action_id")) is not int or type(injection.get("action_id")) is not int
                or received.get("action_id") != action or injection.get("action_id") != action
                or injection.get("event") != "injected"):
            raise BenchError("Input action identity mismatch")
        keys = (injection.get("before_monotonic_ns"), injection.get("after_monotonic_ns"),
                received.get("received_monotonic_ns"), received.get("delivered_ms"))
        if any(type(value) is not int or value < 0 for value in keys):
            raise BenchError("Invalid input clock values")
        before, after, delivered, delivered_ms = keys
        commit = commits[action]
        feedback = presented.get(commit["frame"])
        if (not feedback or before > after or before < ready[0]["monotonic_ns"] or delivered < before
                or delivered > commit["monotonic_ns"] or commit["input_received_monotonic_ns"] != delivered
                or commit["input_delivered_ms"] != delivered_ms or feedback["presentation_ns"] < delivered):
            raise BenchError("Input/presentation clock ordering or feedback failed")
        # wl_keyboard has a wrapping millisecond clock. Establish its relation
        # to the injection interval instead of treating it as nanoseconds.
        distance = ((delivered_ms - ((before // 1_000_000) & 0xffffffff) + (1 << 31)) % (1 << 32)) - (1 << 31)
        if distance < -2 or distance > (after - before) / 1_000_000 + 2:
            raise BenchError("Delivered key clock is incompatible with injection time")
        upper.append(feedback["presentation_ns"] - before)
        lower.append(max(0, feedback["presentation_ns"] - after))
        receipt.append(delivered - before)
        uncertainty.append(after - before)
    return {"status": "passed", "method": "linux-uinput-f24-to-wayland-presentation-v1", "actions": count,
            "delay_ms": delay, "injection_to_presentation_upper_ms": distribution(upper),
            "injection_to_presentation_lower_ms": distribution(lower), "injection_to_client_receipt_ms": distribution(receipt),
            "injection_interval_ms": distribution(uncertainty), "presentation": metrics,
            "excludes": ["physical switch", "USB polling", "panel scanout position", "pixel response"]}


def capture_worker(target, primary, layout, count=CAPTURES):
    """Called only in a reserved-UID user unit, never by a root workload."""
    primary = connector(primary)
    if count not in (1, CAPTURES): raise BenchError('Unknown fixed capture sample count')
    helper = layout.helper.parent / "vela-bench-desktop"
    if target == "plasma":
        # Stream through the fixed native API, retaining its request/receipt
        # bounds. No portal or other capture backend is attempted.
        # Forward records immediately. Desktop actions synchronize against
        # their first pre-action captures, not completion of the whole batch.
        with subprocess.Popen([str(helper), "--capture", primary, str(count)],
                              stdout=subprocess.PIPE, text=True) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
            if process.wait(timeout=5):
                raise BenchError("Plasma capture failed")
        return
    if target != "vela":
        raise BenchError("Unknown fixed capture adapter")
    path = layout.home / ".benchmark-capture.png"
    try:
        for index in range(count):
            begin = time.monotonic_ns()
            env = {key: value for key, value in os.environ.items() if not key.startswith("VELA_SCREEN_DUMP_")}
            completed = subprocess.run([str(layout.helper.parent / "vela-screen-dump"), str(path)], env=env,
                                       capture_output=True, text=True, timeout=8)
            end = time.monotonic_ns()
            if completed.returncode:
                raise BenchError(completed.stderr.strip() or "Vela capture failed")
            decoded = subprocess.run([str(helper), "--analyze", str(path)], capture_output=True, text=True, timeout=8)
            if decoded.returncode:
                raise BenchError(decoded.stderr.strip() or "Capture marker analysis failed")
            row = decode_json(decoded.stdout)
            row.update(event="capture", backend="ext-image-copy-capture-v1", index=index,
                       begin_monotonic_ns=str(begin), end_monotonic_ns=str(end))
            print(json.dumps(row), flush=True)
            if index + 1 < count:
                time.sleep(.1)
    finally:
        path.unlink(missing_ok=True)


def collect_capture(host, record, directory, target, profile, layout):
    command = user_command(record, layout, "capture", ["/usr/bin/python3", "-I", str(layout.helper),
                           "_capture", target, profile["primary"]], runtime_seconds=60)
    result = host.run(command, check=False, timeout=65)
    (directory / f"{target}-capture.stderr").write_text(result.stderr)
    if result.returncode:
        raise BenchError("Capture adapter failed: " + result.stderr[-1000:])
    if len(result.stdout) > 192 * 1024 * 1024:
        raise BenchError("Oversized capture evidence")
    rows = [decode_json(line) for line in result.stdout.splitlines()]
    if len(rows) != CAPTURES:
        raise BenchError("Capture sample count mismatch")
    backend = "ext-image-copy-capture-v1" if target == "vela" else "kwin-screenshot2-v1"
    previous_end = -1
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or type(row.get("index")) is not int or row.get("index") != index or row.get("backend") != backend or row.get("event") != "capture":
            raise BenchError("Capture backend or sample identity changed")
        if (row.get("width"), row.get("height")) != (profile["mode"]["width"], profile["mode"]["height"]):
            raise BenchError("Capture geometry is not the requested physical output geometry")
        try:
            png = base64.b64decode(row.pop("png_base64"), validate=True)
        except (ValueError, KeyError) as exc:
            raise BenchError("Invalid capture PNG encoding") from exc
        if len(png) > 12 * 1024 * 1024 or len(png) < 24 or png[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack("!II", png[16:24]) != (row["width"], row["height"]):
            raise BenchError("Capture PNG header/size does not match recorded geometry")
        image = directory / f"{target}-capture-{index:02d}.png"
        image.write_bytes(png)
        row.update(image_file=image.name, image_sha256=hashlib.sha256(png).hexdigest())
        for key in ("begin_monotonic_ns", "end_monotonic_ns"):
            if not isinstance(row.get(key), str) or not row[key].isdigit():
                raise BenchError("Invalid capture request/receipt clock")
            row[key] = int(row[key])
        if row["begin_monotonic_ns"] > row["end_monotonic_ns"] or row["begin_monotonic_ns"] < previous_end:
            raise BenchError("Capture request/receipt intervals are reversed or overlapping")
        previous_end = row["end_monotonic_ns"]
        markers = row.get("markers")
        if not isinstance(markers, list) or len(markers) != 1 or not isinstance(markers[0], dict) or markers[0].get("marker") != "vela-frame-id-v2":
            raise BenchError("Expected exactly one calibration marker in each output capture")
        marker = markers[0]
        if (any(type(marker.get(key)) is not int for key in ("frame", "x", "y"))
                or not 0 <= marker["frame"] <= 0xffffffff or not 0 <= marker["x"] <= row["width"] - 608
                or not 0 <= marker["y"] <= row["height"] - 32):
            raise BenchError("Invalid calibration marker identity or geometry")
    return rows


def stop_process(process):
    if process is None:
        return
    if process.stdin:
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    for stream in (process.stdout, process.stderr):
        if stream is not None:
            stream.close()


def run_input(host, record, directory, target, layout, health, *, delay=0, capture_profile=None,
              measurement_name=None):
    if measurement_name not in (None, "measure-input", "measure-explicit-control-input", "measure-implicit-control-input", "measure-explicit-input", "measure-implicit-input"):
        raise BenchError("Unknown fixed input measurement")
    count = 105 if measurement_name else INPUTS if delay == 0 else 5
    if measurement_name and (delay or capture_profile):
        raise BenchError("Neutral input measurements require fixed no-delay, capture-off conditions")
    prefix = directory / (target + "-" + measurement_name if measurement_name else
                          target + ("-input-delay" if delay else "-input-capture" if capture_profile else "-input"))
    injector = probe = None
    pool, capture = ThreadPoolExecutor(max_workers=1), None
    outcome = {"status": "failed", "target": target, "method": "linux-uinput-f24-to-wayland-presentation-v1"}
    input_rows, injections, capture_rows = [], [], []
    sysname = None
    usage = resource.getrusage(resource.RUSAGE_SELF)
    start = time.monotonic_ns()
    cpu_path = layout.path(f"sys/fs/cgroup/user.slice/user-{record['uid']}.slice/cpu.stat")
    def cpu_usage():
        try:
            counters = dict(line.split() for line in cpu_path.read_text().splitlines())
            return int(counters["usage_usec"])
        except (OSError, ValueError, KeyError):
            return None
    before_cpu = cpu_usage()
    try:
        with prefix.with_suffix(".stderr").open("wb") as errors:
            injector = host.spawn([str(layout.helper.parent / "vela-bench-input")], errors)
            injection_lines = Lines(injector)
            ready = injection_lines.next(time.monotonic() + 5)
            if (ready.get("event") != "uinput-ready" or ready.get("method") != "linux-uinput-f24-v1"
                    or ready.get("clock") != "CLOCK_MONOTONIC"
                    or not re.fullmatch(r"/sys/devices/virtual/input/input[0-9]+", ready.get("sysfs", ""))):
                raise BenchError("uinput device did not become ready")
            sysname = ready["sysfs"]
            time.sleep(1)  # udev/libinput device discovery, outside measurement.
            timeout = 40 if measurement_name else 20
            args = [str(layout.probe), "--inputs", str(count), "--delay-ms", str(delay), "--timeout", str(timeout)]
            if capture_profile:
                args.append("--hold")
            probe = host.spawn(user_command(record, layout, "calibration", args, runtime_seconds=60 if measurement_name else 25), errors)
            lines, deadline = Lines(probe), time.monotonic() + timeout + 2
            while lines.next(deadline).get("event") != "input-ready":
                pass
            time.sleep(.5)  # Bounded window-animation settling, outside sampled actions.
            if capture_profile:
                capture = pool.submit(collect_capture, host, record, directory, target, capture_profile, layout)
            for action in range(1, count + 1):
                health()
                time.sleep(.08 + ((action * 7) % 20) / 1000)
                injector.stdin.write(b"pulse\n")
                injection = injection_lines.next(deadline)
                if injection.get("action_id") != action:
                    raise BenchError("uinput action identity mismatch")
                injections.append(injection)
                # Drain the probe while injecting, and wait for this action's
                # visual feedback. Large samples must not fill the pipe and
                # stall the client or overlap unresolved actions.
                while True:
                    row = lines.next(deadline)
                    if row.get("event") in ("presented", "discarded") and row.get("frame") == action + 1:
                        if row["event"] == "discarded":
                            raise BenchError("Input action's visual submission was discarded")
                        break
                    if row.get("event") == "summary":
                        raise BenchError("Input client ended before the requested action")
            if row.get("event") != "summary":
                while lines.next(deadline).get("event") != "summary":
                    pass
            input_rows = lines.records
            metrics = parse_input(input_rows, injections, count=count, delay=delay)
            if capture:
                capture_rows = capture.result(timeout=65)
                ids = {row["frame"] for row in input_rows if row.get("event") == "presented"}
                seen = [row["markers"][0].get("frame") for row in capture_rows]
                if any(type(frame) is not int or frame not in ids for frame in seen) or len(set(seen)) < 3:
                    raise BenchError("Capture did not observe distinct matching presented frame IDs")
                gaps = [b["begin_monotonic_ns"] - a["begin_monotonic_ns"] for a, b in zip(capture_rows, capture_rows[1:])]
                metrics["capture"] = {"status": "passed", "backend": capture_rows[0]["backend"], "frames": capture_rows,
                    "distinct_visual_ids": len(set(seen)), "request_intervals_ms": distribution(gaps),
                    "request_receipt_ms": distribution([row["end_monotonic_ns"] - row["begin_monotonic_ns"] for row in capture_rows]),
                    "scope": "coarse-output-state-observation", "fine_grained_motion_supported": False}
            if probe.stdin:
                probe.stdin.close()
            if probe.wait(timeout=3):
                raise BenchError("Input probe returned a failure status")
            outcome.update(status="passed", metrics=metrics)
    except (BenchError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        outcome["error"] = str(exc)
    finally:
        for process in (probe, injector):
            try:
                stop_process(process)
            except (OSError, subprocess.TimeoutExpired) as exc:
                outcome.update(status="failed", error="Instrument process cleanup failed: " + str(exc))
        pool.shutdown(wait=True, cancel_futures=True)
        try:
            stop_instrument(host, record, layout, "calibration")
        except BenchError as exc:
            outcome.update(status="failed", error="Instrument stop failed: " + str(exc))
        outcome["uinput_cleanup"] = "passed" if sysname and not Path(sysname).exists() else "failed"
        if outcome["uinput_cleanup"] != "passed":
            outcome.update(status="failed", error=outcome.get("error", "uinput device cleanup was not verified"))
        after = resource.getrusage(resource.RUSAGE_SELF)
        after_cpu = cpu_usage()
        outcome["collector"] = {"scope": "root-coordinator-process-only", "cpu_seconds":
            after.ru_utime + after.ru_stime - usage.ru_utime - usage.ru_stime,
            "elapsed_seconds": (time.monotonic_ns() - start) / 1_000_000_000, "max_rss_kib_at_end": after.ru_maxrss}
        outcome["session_accounting"] = {"scope": "entire-reserved-UID-cgroup-including-probe-and-capture",
            "status": "passed" if before_cpu is not None and after_cpu is not None and after_cpu >= before_cpu else "unsupported",
            "cpu_usage_begin_usec": before_cpu, "cpu_usage_end_usec": after_cpu}
        if not input_rows and probe is not None and 'lines' in locals():
            input_rows = lines.records
        prefix.with_suffix(".jsonl").write_text("".join(json.dumps(row) + "\n" for row in input_rows))
        atomic_json(prefix.with_suffix(".injections.json"), injections)
        if capture_rows:
            atomic_json(directory / f"{target}-capture.json", capture_rows)
        atomic_json(prefix.with_suffix(".json"), outcome)
    return outcome


def prepare_fault_input(host, layout, directory):
    with (directory / "fault-input.stderr").open("wb") as errors:
        process = host.spawn([str(layout.helper.parent / "vela-bench-input")], errors)
        try:
            row = Lines(process).next(time.monotonic() + 5)
            if (row.get("event") != "uinput-ready" or row.get("method") != "linux-uinput-f24-v1"
                    or not re.fullmatch(r"/sys/devices/virtual/input/input[0-9]+", row.get("sysfs", ""))):
                raise BenchError("Fault fixture could not establish its live uinput device")
            return process, row["sysfs"]
        except BaseException:
            stop_process(process)
            raise
