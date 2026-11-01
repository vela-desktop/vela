# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Explicit P0 acceptance checks over archived evidence, never a ranking."""

import json
from pathlib import Path

from . import BenchError
from .archive import decode_json, read_json
from .calibration import parse_input
from .diagnostics import SCENARIOS, parse_probe
from .discovery import file_hash
from .host import FAULT_UNIT


def calibration_evidence(directory, target):
    samples = {}
    for name, count, delay in (("input", 20, 0), ("input-capture", 20, 0), ("input-delay", 5, 40)):
        prefix = directory / f"{target}-{name}"
        saved = read_json(prefix.with_suffix(".json"))
        rows = [decode_json(line) for line in prefix.with_suffix(".jsonl").read_text().splitlines()]
        injections = read_json(prefix.with_suffix(".injections.json"))
        metrics = parse_input(rows, injections, count=count, delay=delay)
        if (saved.get("status") != "passed" or saved.get("uinput_cleanup") != "passed"
                or saved.get("session_accounting", {}).get("status") != "passed"):
            raise BenchError("Calibration did not verify input cleanup and complete UID accounting")
        actual = dict(saved.get("metrics", {}))
        capture = actual.pop("capture", None)
        if actual != metrics:
            raise BenchError("Calibration statistics differ from raw input/injection evidence")
        if name == "input-capture":
            if not isinstance(capture, dict) or capture.get("status") != "passed" or capture.get("scope") != "coarse-output-state-observation":
                raise BenchError("No validated unattended capture route")
            frames = capture.get("frames")
            if not isinstance(frames, list) or len(frames) != 12:
                raise BenchError("Incomplete capture calibration")
            ids = {row["frame"] for row in rows if row.get("event") == "presented"}
            seen = set()
            for index, row in enumerate(frames):
                expected = f"{target}-capture-{index:02d}.png"
                if row.get("image_file") != expected or file_hash(directory / expected) != row.get("image_sha256"):
                    raise BenchError("Capture image identity changed")
                markers = row.get("markers", [])
                if len(markers) != 1 or markers[0].get("frame") not in ids:
                    raise BenchError("Capture lost the presented frame identity")
                seen.add(markers[0]["frame"])
            if len(seen) < 3:
                raise BenchError("Capture did not observe known visual changes")
            metrics["capture"] = capture
        samples[name] = {**saved, "metrics": metrics}
    baseline = samples["input"]["metrics"]["injection_to_presentation_upper_ms"]["p50"]
    delayed = samples["input-delay"]["metrics"]["injection_to_presentation_upper_ms"]["p50"]
    if delayed - baseline < 25:
        raise BenchError("The known 40 ms input/rendering defect was not distinguished")
    return samples


def audit_pilot(directory, target, fault=None, *, instruments=True):
    directory = Path(directory)
    state = read_json(directory / "state.json")
    checks = {"restored": state.get("stage") in ("done", "failed") and not state.get("cleanup_errors"),
              "hardware_restored": state.get("conditions_restore", {}).get("status") == "passed",
              "conditions_requested": isinstance(state.get("conditions_snapshot"), dict)}
    details = {}
    try:
        session = read_json(directory / f"{target}-session.json")
        checks["namespace_isolation"] = session.get("fixture_hidden_from_both_trees") is True
        checks["full_session"] = session.get("graphical_session_target") == "active"
        conditions = read_json(directory / f"{target}-conditions.json")
        checks["hardware_verified"] = conditions.get("status") == "passed"
        if fault:
            supervision = state.get("supervision", {})
            checks["fault_injected"] = state.get("fault_injected") == fault and state.get("unit") == FAULT_UNIT
            checks["fault_failed_run"] = state.get("stage") == "failed" and state.get("result") == "failed"
            checks["pid1_recovered"] = supervision.get("SERVICE_RESULT") == ("signal" if fault == "crash" else "timeout")
            checks["fault_input_cleaned"] = state.get("fault_uinput_cleanup") == "passed"
        else:
            checks["successful_run"] = state.get("stage") == "done" and state.get("result") == "passed"
            if not instruments:
                checks["capture_and_input_disabled"] = (state.get("campaign_block",{}).get("measurement_executed") is True
                    and state["campaign_block"]["block"]["phase"]=="overhead"
                    and state["plan"].get("instrument_calibration") is False
                    and state["plan"].get("presentation_probe") is False)
                return {"schema":1,"method":"uid-resource-lifecycle-v1","target":target,"fault":None,
                        "status":"passed" if all(checks.values()) else "failed","checks":checks,
                        "performance_comparison_available":False}
            for scenario, delay in SCENARIOS.items():
                prefix = directory / f"{target}-{scenario}"
                saved = read_json(prefix.with_suffix(".json"))
                metrics = parse_probe(prefix.with_suffix(".jsonl").read_text(), delay=delay)
                checks["presentation_" + scenario] = saved.get("status") == "complete" and saved.get("metrics") == metrics
            details["calibration"] = calibration_evidence(directory, target)
            checks["input_capture_sensitivity_accounting"] = True
    except (BenchError, OSError, ValueError, KeyError, TypeError) as exc:
        checks["evidence_valid"] = False
        details["error"] = str(exc)
    return {"schema": 1, "method": "p0-instruments-v1", "target": target, "fault": fault,
            "status": "passed" if checks and all(checks.values()) else "failed", "checks": checks, **details,
            "capabilities": {"input": "software-uinput-to-reported-presentation", "capture": "coarse-output-state-only",
                             "fine_grained_desktop_motion": False}, "performance_comparison_available": False}


def qualification_summary(blocks, targets):
    checks, repeatability = {}, {}
    for target in targets:
        selected = [block for block in blocks if block["target"] == target and block.get("status") == "complete"]
        for fault in ("crash", "hang"):
            checks[f"{target}/{fault}"] = any(block.get("fault") == fault and block.get("audit", {}).get("status") == "passed" for block in selected)
        baselines = [block for block in selected if not block.get("fault") and block.get("audit", {}).get("status") == "passed"]
        checks[f"{target}/independent-calibrations"] = len(baselines) >= 2
        if len(baselines) >= 2:
            values = [block["audit"]["calibration"]["input"]["metrics"]["injection_to_presentation_upper_ms"]["p50"] for block in baselines]
            refresh = baselines[0]["profile"]["mode"]["refresh_mhz"]
            # Versioned before any desktop/version comparison; no automatic
            # tolerance widening after seeing a preferred target's numbers.
            tolerance = max(5., 2_000_000 / refresh)
            repeatability[target] = {"independent_blocks": len(values), "p50_range_ms": max(values) - min(values),
                                     "tolerance_ms": tolerance, "qualification_only": True}
            checks[f"{target}/A-A-repeatability"] = max(values) - min(values) <= tolerance
    return {"schema": 1, "method": "p0-instruments-v1", "status": "passed" if checks and all(checks.values()) else "pending",
            "checks": checks, "repeatability": repeatability,
            "scope": "initial-machine-declared-adapters-input-and-coarse-capture", "full_suite_complete": False,
            "fine_grained_desktop_motion_supported": False, "performance_comparison_available": False}
