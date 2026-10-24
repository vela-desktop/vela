# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Durable preflight archives. Performance evidence has a separate future schema."""

from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import tempfile
import time
import uuid

from . import BenchError, SCHEMA
from .discovery import file_hash
from .model import fingerprint, MEASUREMENTS


def atomic_json(path, value):
    """Publish only a complete file, then persist its directory entry."""
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def decode_json(raw):
    def reject_constant(value):
        raise BenchError(f"Non-finite JSON number: {value}")
    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise BenchError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise BenchError("Non-finite JSON number")
        return parsed
    return json.loads(raw, parse_constant=reject_constant, parse_float=finite_float, object_pairs_hook=unique_keys)


def read_json(path):
    try:
        return decode_json(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise BenchError(f"Cannot read {path}: {exc}") from exc


def append_event(path, event):
    record = {"monotonic_ns": time.monotonic_ns(), "wall_time": datetime.now(timezone.utc).isoformat(), **event}
    with Path(path).open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, allow_nan=False, sort_keys=True) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def read_events(path):
    """A truncated final line is evidence of interruption, never completion."""
    try:
        raw = Path(path).read_bytes()
    except OSError as exc:
        raise BenchError(f"Cannot read event journal: {exc}") from exc
    if raw and not raw.endswith(b"\n"):
        raise BenchError("Truncated event journal")
    events = []
    for number, line in enumerate(raw.splitlines(), 1):
        try:
            event = decode_json(line)
            if (not isinstance(event, dict) or type(event.get("monotonic_ns")) is not int
                    or event["monotonic_ns"] < 0 or not isinstance(event.get("event"), str)):
                raise ValueError("invalid event record")
            if events and event["monotonic_ns"] < events[-1]["monotonic_ns"]:
                raise ValueError("non-monotonic event journal")
            events.append(event)
        except (ValueError, UnicodeError) as exc:
            raise BenchError(f"Invalid journal line {number}: {exc}") from exc
    return events


def create_check_archive(output, plan, discovery):
    if plan["phases"] != ["check"] or plan["blocks"]:
        raise BenchError("Only host-side check archives can currently be executed")
    parent = Path(output)
    parent.mkdir(parents=True, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:12]
    directory = parent / run_id
    directory.mkdir(mode=0o700)
    for name in ("blocks", "samples", "calibration", "captures", "logs"):
        (directory / name).mkdir(mode=0o700)
    manifest = {"schema": SCHEMA, "kind": "preflight", "run_id": run_id,
                "created_at": datetime.now(timezone.utc).isoformat(), "plan_fingerprint": plan["fingerprint"],
                "environment": discovery["environment"], "suite": discovery["suite"]}
    atomic_json(directory / "manifest.json", manifest)
    atomic_json(directory / "plan.json", plan)
    append_event(directory / "events.jsonl", {"event": "phase-start", "phase": "check"})
    atomic_json(directory / "check.json", discovery)
    append_event(directory / "events.jsonl", {"event": "phase-complete", "phase": "check", "status": "valid"})
    # Hash evidence only after all streams are closed. Publishing this marker
    # last makes an interrupted check distinguishable from a completed check.
    files = {name: file_hash(directory / name) for name in ("manifest.json", "plan.json", "check.json", "events.jsonl")}
    atomic_json(directory / "blocks/check.json", {"schema": SCHEMA, "phase": "check", "attempt": 1,
                                               "status": "valid", "files": files})
    return directory


def inspect_archive(directory):
    directory = Path(directory)
    manifest = read_json(directory / "manifest.json")
    plan = read_json(directory / "plan.json")
    if (not isinstance(manifest, dict) or type(manifest.get("schema")) is not int
            or manifest["schema"] != SCHEMA or manifest.get("kind") != "preflight"):
        raise BenchError("Unsupported archive schema/kind; performance archives are not implemented yet")
    if (not isinstance(manifest.get("run_id"), str) or not manifest["run_id"]
            or not isinstance(manifest.get("environment"), dict) or not isinstance(manifest.get("suite"), dict)):
        raise BenchError("Invalid archived manifest")
    if (not isinstance(plan, dict) or not isinstance(plan.get("fingerprint"), str)
            or type(plan.get("schema")) is not int or plan["schema"] != SCHEMA
            or plan.get("mode") not in ("normal", "fast") or type(plan.get("repetitions")) is not int
            or plan["repetitions"] != (1 if plan["mode"] == "fast" else 3)
            or not isinstance(plan.get("targets"), dict) or not isinstance(plan.get("display_policy"), dict)
            or not isinstance(plan.get("scenario_revision"), str)):
        raise BenchError("Invalid archived plan")
    contents = {key: value for key, value in plan.items() if key != "fingerprint"}
    if fingerprint(contents) != plan["fingerprint"] or manifest.get("plan_fingerprint") != plan["fingerprint"]:
        raise BenchError("Archived plan fingerprint mismatch")
    if plan.get("phases") != ["check"] or plan.get("blocks") != []:
        raise BenchError("A preflight archive cannot contain measurement blocks")
    if plan.get("environment_fingerprint") != fingerprint(manifest.get("environment")):
        raise BenchError("Archived environment fingerprint mismatch")
    if plan.get("suite_fingerprint") != fingerprint(manifest["suite"]):
        raise BenchError("Archived suite fingerprint mismatch")
    marker_path = directory / "blocks/check.json"
    complete = marker_path.exists()
    if complete:
        marker = read_json(marker_path)
        expected = {"manifest.json", "plan.json", "check.json", "events.jsonl"}
        if (not isinstance(marker, dict) or marker.get("schema") != SCHEMA or marker.get("phase") != "check"
                or marker.get("status") != "valid" or not isinstance(marker.get("files"), dict)
                or set(marker["files"]) != expected):
            raise BenchError("Invalid check completion record")
        for name, digest in marker["files"].items():
            if not isinstance(digest, str) or file_hash(directory / name) != digest:
                raise BenchError(f"Completion evidence mismatch: {name}")
    journal = directory / "events.jsonl"
    events = read_events(journal) if journal.exists() else []
    if complete and (len(events) != 2 or events[0].get("event") != "phase-start"
                     or events[1].get("event") != "phase-complete" or events[1].get("status") != "valid"
                     or any(x.get("phase") != "check" for x in events)):
        raise BenchError("Completion record does not match check journal")
    summary = {"schema": SCHEMA, "run_id": manifest["run_id"], "kind": "preflight",
               "execution_complete": complete, "selection_complete": complete, "full_suite_complete": False,
               "performance_results": False, "mode": plan["mode"], "planned_repetitions": plan["repetitions"],
               "valid_measurement_repetitions": 0, "selected_phases": ["check"],
               "families": {name: "not-selected" for name in MEASUREMENTS},
               "check_status": "valid" if complete else "not-run", "live_execution_available": False}
    return manifest, plan, summary
