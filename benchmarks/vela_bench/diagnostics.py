# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Bounded presentation diagnostics, separate from qualified benchmarks."""

import hashlib
import html
import json
from pathlib import Path
import statistics

from . import BenchError
from .archive import atomic_json, decode_json, read_json

SCENARIOS = {"baseline": 0, "client-delay": 40}
FRAMES = 120
PROBE_UNIT = "vela-benchmark-probe.service"


def distribution(values):
    if not values:
        return None
    ordered = sorted(value / 1_000_000 for value in values)
    def percentile(fraction):
        position = (len(ordered) - 1) * fraction
        lower = int(position)
        return ordered[lower] + (ordered[min(lower + 1, len(ordered) - 1)] - ordered[lower]) * (position - lower)
    return {"samples": len(ordered), "min": ordered[0], "p50": percentile(.5), "p95": percentile(.95),
            "max": ordered[-1], "mean": statistics.mean(ordered)}


def parse_probe(raw, *, frames=FRAMES, delay=0, method="wayland-presentation-v1", scope="diagnostic-client-only"):
    """Validate bounded unprivileged output before deriving any statistics."""
    if len(raw.encode("utf-8")) > 2 * 1024 * 1024 or not raw.endswith("\n"):
        raise BenchError("Probe stream is oversized or truncated")
    try:
        rows = [decode_json(line) for line in raw.splitlines()]
    except (ValueError, UnicodeError) as exc:
        raise BenchError(f"Invalid probe JSON: {exc}") from exc
    if not rows or any(not isinstance(row, dict) for row in rows):
        raise BenchError("Invalid probe records")
    def integer(row, key):
        value = row.get(key)
        if type(value) is not int or value < 0:
            raise BenchError(f"Invalid probe integer: {key}")
        return value
    clocks, metadata, commits, feedback, final = [], [], {}, {}, []
    for index, row in enumerate(rows):
        event = row.get("event")
        if event == "clock":
            if index != 0:
                raise BenchError("Probe clock must precede samples")
            integer(row, "presentation_clock_id")
            integer(row, "monotonic_clock_id")
            clocks.append(row)
        elif event == "probe":
            if index != 1 or type(row.get("schema")) is not int or row["schema"] != 1 or row.get("method") != method or row.get("scope") != scope:
                raise BenchError("Unsupported probe method or scope")
            if integer(row, "frames") != frames or integer(row, "delay_requested_ms") != delay:
                raise BenchError("Probe scenario differs from requested workload")
            metadata.append(row)
        elif event == "commit":
            frame = integer(row, "frame")
            if len(metadata) != 1 or frame != len(commits) + 1 or frame > frames:
                raise BenchError("Invalid probe submission identity")
            integer(row, "commit_clock_ns")
            integer(row, "monotonic_ns")
            if integer(row, "delay_elapsed_ns") < delay * 1_000_000:
                raise BenchError("Known client delay was not observed")
            if commits and (row["commit_clock_ns"] < commits[frame - 1]["commit_clock_ns"]
                            or row["monotonic_ns"] < commits[frame - 1]["monotonic_ns"]):
                raise BenchError("Probe submission clock moved backwards")
            commits[frame] = row
        elif event in ("presented", "discarded"):
            frame = integer(row, "frame")
            if frame not in commits or frame in feedback:
                raise BenchError("Unmatched or duplicate presentation feedback")
            if integer(row, "received_monotonic_ns") < commits[frame]["monotonic_ns"]:
                raise BenchError("Feedback receipt predates submission")
            if event == "presented":
                if integer(row, "commit_clock_ns") != commits[frame]["commit_clock_ns"]:
                    raise BenchError("Feedback refers to a different submission clock")
                if integer(row, "presentation_ns") < row["commit_clock_ns"]:
                    raise BenchError("Presentation predates submission")
                for key in ("refresh_ns", "sequence", "flags"):
                    integer(row, key)
            feedback[frame] = row
        elif event == "summary":
            if index != len(rows) - 1 or row.get("complete") is not True:
                raise BenchError("Probe did not complete")
            final.append(row)
        else:
            raise BenchError("Unknown probe event")
    presented = [feedback[frame] for frame in sorted(feedback) if feedback[frame]["event"] == "presented"]
    discarded = len(feedback) - len(presented)
    if (len(clocks) != 1 or len(metadata) != 1 or len(final) != 1 or len(commits) != frames
            or len(feedback) != frames or not presented or integer(final[0], "committed") != frames
            or integer(final[0], "presented") != len(presented) or integer(final[0], "discarded") != discarded):
        raise BenchError("Incomplete or inconsistent probe counts")
    intervals = [b["presentation_ns"] - a["presentation_ns"] for a, b in zip(presented, presented[1:])]
    if any(value < 0 for value in intervals):
        raise BenchError("Presentation clock moved backwards")
    return {"method": method, "scope": scope, "clock": clocks[0],
            "frames": frames, "presented": len(presented), "discarded": discarded,
            "client_delay_ms": delay, "submission_to_presentation_ms": distribution([
                row["presentation_ns"] - row["commit_clock_ns"] for row in presented]),
            "presentation_intervals_ms": distribution(intervals),
            "client_sleep_ms": distribution([row["delay_elapsed_ns"] for row in commits.values()]),
            "feedback_flags": sorted({row["flags"] for row in presented}),
            "raw_sha256": hashlib.sha256(raw.encode("utf-8")).hexdigest()}


def user_prefix(record, layout):
    uid, gid = record["uid"], record["gid"]
    runtime = str(layout.path(f"run/user/{uid}"))
    # PID 1's user manager supplies the desktop's Wayland environment and
    # its existing mount namespace. The root controller never executes the
    # client itself and never guesses a Wayland socket from /run/user.
    return ["setpriv", f"--reuid={uid}", f"--regid={gid}", "--clear-groups", "/usr/bin/env", "-i",
            "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "HOME=" + str(layout.home), "XDG_RUNTIME_DIR=" + runtime,
            "DBUS_SESSION_BUS_ADDRESS=unix:path=" + runtime + "/bus"]


def user_command(record, layout, operation, command, runtime_seconds=25):
    if operation not in ("probe", "display", "capture", "calibration", "measurement", "producer", "scene", "observation") or runtime_seconds not in (25, 60, 120, 180):
        raise BenchError("Unknown fixed diagnostic operation")
    return user_prefix(record, layout) + ["/usr/bin/systemd-run", "--user",
            "--quiet", "--wait", "--pipe", "--collect", "--service-type=exec", "--unit=vela-benchmark-" + operation + ".service",
            f"--property=RuntimeMaxSec={runtime_seconds}", "--property=TimeoutStopSec=3", "--property=NoNewPrivileges=yes", *command]


def stop_instrument(host, record, layout, operation):
    if operation not in ("probe", "display", "capture", "calibration", "measurement", "producer", "scene", "observation"):
        raise BenchError("Unknown fixed instrument unit")
    host.run(user_prefix(record, layout) + ["/usr/bin/systemctl", "--user", "stop", "vela-benchmark-" + operation + ".service"],
             check=False, timeout=8)


def probe_command(record, scenario, layout):
    if scenario not in SCENARIOS:
        raise BenchError("Unknown presentation diagnostic scenario")
    return user_command(record, layout, "probe", [str(layout.probe), "--frames", str(FRAMES),
                        "--delay-ms", str(SCENARIOS[scenario]), "--timeout", "20"])


def run_probe(host, record, directory, target, scenario, layout):
    if target not in ("vela", "plasma") or scenario not in SCENARIOS:
        raise BenchError("Unknown fixed presentation diagnostic operation")
    prefix = directory / f"{target}-{scenario}"
    outcome = {"target": target, "scenario": scenario, "status": "failed", "frames": FRAMES,
               "client_delay_ms": SCENARIOS[scenario], "probe_sha256": record["files"].get(str(layout.probe))}
    try:
        result = host.run(probe_command(record, scenario, layout), check=False, timeout=32)
        # Raw stdout/stderr survive parse failure as well as a nonzero exit.
        prefix.with_suffix(".jsonl").write_text(result.stdout, encoding="utf-8")
        prefix.with_suffix(".stderr").write_text(result.stderr, encoding="utf-8")
        outcome["returncode"] = result.returncode
        outcome["stderr"] = result.stderr[:16384]
        if result.returncode:
            raise BenchError(f"Probe exited with status {result.returncode}; see its saved stderr")
        outcome["metrics"] = parse_probe(result.stdout, delay=SCENARIOS[scenario])
        outcome["status"] = "complete"
    except (BenchError, OSError) as exc:
        outcome["error"] = str(exc)
    atomic_json(prefix.with_suffix(".json"), outcome)
    return outcome


def pilot_results(directory, state):
    order = state.get("plan", {}).get("target_order", list(state.get("plan", {}).get("targets", {})))
    if not isinstance(order, list) or any(target not in ("vela", "plasma") for target in order):
        raise BenchError("Invalid pilot target order")
    samples = []
    for target in order:
        for scenario, delay in SCENARIOS.items():
            prefix = directory / f"{target}-{scenario}"
            if not prefix.with_suffix(".json").exists():
                samples.append({"target": target, "scenario": scenario, "status": "not-run"})
                continue
            row = read_json(prefix.with_suffix(".json"))
            if not isinstance(row, dict) or row.get("target") != target or row.get("scenario") != scenario:
                raise BenchError("Saved probe scenario identity changed")
            if row.get("status") == "complete":
                raw = prefix.with_suffix(".jsonl").read_text(encoding="utf-8")
                metrics = parse_probe(raw, delay=delay)
                if metrics != row.get("metrics"):
                    raise BenchError("Saved probe summary differs from raw evidence")
                row["raw_jsonl"] = raw
            samples.append(row)
    complete = bool(samples) and all(row["status"] == "complete" for row in samples)
    logs = {name: read_json(directory / name) for name in
            ("vela-session-log.json", "vela-session-exit.json", "plasma-session-exit.json",
             "vela-session-stop.json", "plasma-session-stop.json", "display-manager-recovery.json",
             "conditions-recovery.json") if (directory / name).exists()}
    for target in order:
        path = directory / f"{target}-launch.stderr"
        if path.exists():
            with path.open("rb") as stream:
                size = stream.seek(0, 2)
                stream.seek(max(0, size - 131072))
                logs[path.name] = {"schema": 1, "status": "collected", "truncated": size > 131072,
                                   "text": stream.read(131072).decode("utf-8", errors="replace")}
    return {"kind": "session-presentation-diagnostic", "schema": 1, "run_id": state["run_id"],
            "stage": state["stage"], "result": state["result"], "error": state.get("error"),
            "lifecycle_result": state.get("lifecycle_result", "unknown"),
            "conditions_snapshot": state.get("conditions_snapshot"), "conditions_restore": state.get("conditions_restore"),
            "session_logs": logs,
            "cleanup_errors": state.get("cleanup_errors", []), "target_order": order,
            "targets": state.get("plan", {}).get("targets", {}), "launcher_files": state.get("launcher_files", {}),
            "diagnostics_requested": state.get("plan", {}).get("presentation_probe", False),
            "diagnostics_complete": complete, "full_suite_complete": False,
            "performance_comparison_available": False, "samples": samples,
            "limits": ["Client protocol timestamps; no input or optical latency measurement.",
                       "Display layout, refresh, brightness and power conditions are not qualified.",
                       "No warm-up qualification, repeated campaign or desktop ranking.",
                       "Client CPU sleep is an instrument control, not slow-GPU workload B3."]}


def export_report(parent, result):
    # Refuse reuse, even when an earlier export has the same run identity.
    run_id = result["run_id"]
    import re
    if not isinstance(run_id, str) or not re.fullmatch(r"[0-9a-f]{32}", run_id):
        raise BenchError("Invalid diagnostic run identity")
    parent = Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    directory = parent / ("pilot-" + run_id)
    directory.mkdir(mode=0o755)
    atomic_json(directory / "summary.json", result)
    (directory / "summary.json").chmod(0o644)
    escape = lambda value: html.escape(str(value), quote=True)
    document = """<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Vela session presentation diagnostic</title><style>
body{font:16px/1.5 system-ui;max-width:70rem;margin:3rem auto;padding:0 1rem;background:#fafbfc;color:#18232b}
table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:.5rem;border-bottom:1px solid #ccd4dc}
</style><h1>Session presentation diagnostic</h1>
<p>Unqualified client diagnostics. These results do not establish which desktop is faster.</p>"""
    document += f"<p>Kind: {escape(result['kind'])}. Run: {escape(run_id)}. Stage: {escape(result['stage'])}. Result: {escape(result['result'])}.</p>"
    if result.get("error"):
        document += f"<p>Error: {escape(result['error'])}</p>"
    for error in result["cleanup_errors"]:
        document += f"<p>Cleanup error: {escape(error)}</p>"
    for name, evidence in result.get("session_logs", {}).items():
        document += f"<details><summary>{escape(name)}</summary><pre>{escape(json.dumps(evidence, indent=2))}</pre></details>"
    document += "<table><tr><th>Desktop</th><th>Scenario</th><th>Status</th><th>Presented / discarded</th><th>Submission → presentation p50 / p95 (ms)</th></tr>"
    for row in result["samples"]:
        metrics = row.get("metrics", {})
        counts = f"{metrics['presented']} / {metrics['discarded']}" if metrics else "—"
        times = metrics.get("submission_to_presentation_ms")
        times = f"{times['p50']:.3f} / {times['p95']:.3f}" if times else "—"
        document += "<tr>" + "".join(f"<td>{escape(value)}</td>" for value in
            (row["target"], row["scenario"], row["status"], counts, times)) + "</tr>"
        if row.get("error"):
            document += f"<tr><td colspan='5'>{escape(row['error'])}</td></tr>"
        if row.get("stderr"):
            document += f"<tr><td colspan='5'><pre>{escape(row['stderr'])}</pre></td></tr>"
    document += "</table><ul>" + "".join(f"<li>{escape(limit)}</li>" for limit in result["limits"]) + "</ul>"
    document += "<p>The JSON file retains validated raw feedback and diagnostic metadata.</p></html>\n"
    (directory / "report.html").write_text(document, encoding="utf-8")
    (directory / "report.html").chmod(0o644)
    return directory
