# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed phase selection and deterministic, serial campaign planning."""

import hashlib
import json
from pathlib import Path
import tomllib

from . import BenchError, SCHEMA


PHASES = {
    "check": {"description": "Discover the host and resolve the campaign", "requirements": [], "takeover": False},
    "calibrate": {"description": "Validate live measurement paths", "requirements": ["all-selected-paths"], "takeover": True},
    "responsiveness": {"description": "Input and native shell response", "requirements": ["input", "presentation", "shell-observation"], "takeover": True},
    "motion": {"description": "Client animation and real window movement", "requirements": ["presentation", "capture", "window-movement"], "takeover": True},
    "slow-apps": {"description": "Paired controls, delayed producer and contention", "requirements": ["input", "presentation", "capture", "load"], "takeover": True},
    "scaling": {"description": "Pixel geometry across scales and window states", "requirements": ["display", "capture", "pixel-pattern"], "takeover": True},
    "overhead": {"description": "Full-session resource inventory and accounting", "requirements": ["process-inventory", "resource-counters"], "takeover": True},
    "report": {"description": "Validate an archive and generate offline HTML", "requirements": [], "takeover": False},
}
MEASUREMENTS = tuple(name for name in PHASES if name not in ("check", "calibrate", "report"))
TARGETS = ("vela", "plasma", "gnome")
# Planning allowances, not frozen scenario definitions or measured runtimes.
# Each tuple is (measurement, fresh-session/calibration/cleanup, deadline) seconds.
BUDGETS = {
    "calibrate": (0, 240, 600),
    "responsiveness": (20, 60, 900),
    "motion": (20, 60, 900),
    "slow-apps": (150, 60, 900),
    "scaling": (180, 120, 720),
    "overhead": (240, 90, 660),
}
DEFAULT_DISPLAY = {"output": "primary", "resolution": "native", "refresh": "highest-common-fixed", "scale": 1.0,
                   "brightness_percent": 100, "power_profile": "performance"}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def fingerprint(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def load_campaign(path=None):
    data = {} if path is None else tomllib.loads(Path(path).read_text(encoding="utf-8"))
    unknown = data.keys() - {"schema", "targets", "phases", "seed", "display"}
    if unknown:
        raise BenchError(f"Unknown campaign keys: {', '.join(sorted(unknown))}")
    if type(data.get("schema", SCHEMA)) is not int or data.get("schema", SCHEMA) != SCHEMA:
        raise BenchError("Unsupported campaign schema")
    for key, choices in (("targets", TARGETS), ("phases", tuple(PHASES))):
        if key in data:
            validate_names(key, data[key], choices)
    seed = data.get("seed", 0)
    if type(seed) is not int or not 0 <= seed <= 2**32 - 1:
        raise BenchError("seed must be an integer between 0 and 4294967295")
    display = data.get("display", {})
    if not isinstance(display, dict) or display.keys() - DEFAULT_DISPLAY.keys():
        raise BenchError("Unknown display configuration")
    # Hardware mode resolution remains a runtime adapter operation. V1 starts
    # with this policy; arbitrary settings must not silently weaken controls.
    for key, value in display.items():
        if type(value) is not type(DEFAULT_DISPLAY[key]) or value != DEFAULT_DISPLAY[key]:
            raise BenchError(f"display.{key} currently requires {DEFAULT_DISPLAY[key]!r}")
    return {"schema": SCHEMA, "targets": data.get("targets"), "phases": data.get("phases"),
            "seed": seed, "display": dict(DEFAULT_DISPLAY, **display)}


def validate_names(label, names, choices):
    if not isinstance(names, list) or not names or any(not isinstance(x, str) or x not in choices for x in names):
        raise BenchError(f"{label} must select from: {', '.join(choices)}")
    if len(set(names)) != len(names):
        raise BenchError(f"Duplicate {label} selections")


def make_plan(campaign, discovery, *, phases=None, targets=None, fast=False, exercise=False):
    selected = phases if phases is not None else campaign["phases"]
    if selected is None:
        selected = list(MEASUREMENTS)
    validate_names("phases", selected, tuple(PHASES))
    if "report" in selected:
        raise BenchError("Use 'report ARCHIVE' for the offline report phase")
    if any(x in selected for x in ("check", "calibrate")) and len(selected) != 1:
        raise BenchError("check and calibrate must each be selected alone")
    if exercise and any(name not in MEASUREMENTS for name in selected):
        raise BenchError("--exercise selects measurement phases; check/calibrate/report are separate commands")
    selected = [name for name in PHASES if name in selected]
    requested = targets if targets is not None else campaign["targets"]
    if requested is not None:
        validate_names("targets", requested, TARGETS)
    installed = discovery["targets"]
    resolved = [x for x in TARGETS if x in (requested if requested is not None else installed)]
    missing = [x for x in resolved if x not in installed]
    if missing:
        raise BenchError(f"Requested normal Wayland sessions unavailable: {', '.join(missing)}")
    if not resolved:
        raise BenchError("No supported normal Wayland session entries found")
    repetitions = 1 if fast else 3
    blocks = []
    for phase in selected:
        if phase == "check":
            continue
        for repetition in range(1, (1 if phase == "calibrate" else repetitions) + 1):
            offset = (campaign["seed"] + repetition - 1) % len(resolved)
            order = resolved[offset:] + resolved[:offset]
            for target in order:
                measurement, preparation, deadline = (0, 60, 900) if exercise else BUDGETS[phase]
                blocks.append({"id": f"{phase}-{target}-r{repetition}", "phase": phase, "target": target,
                               "repetition": repetition, "requirements": PHASES[phase]["requirements"],
                               "measurement_budget_seconds": measurement, "preparation_budget_seconds": preparation,
                               "deadline_seconds": deadline})
                if exercise:
                    blocks[-1].update(workload="p1-instrument-controls-v1", measurement_executed=False,
                                      requirements=list(PHASES["calibrate"]["requirements"]))
    nominal = 10 + sum(x["measurement_budget_seconds"] + x["preparation_budget_seconds"] for x in blocks)
    plan = {"schema": SCHEMA, "scenario_revision": "p1-instrument-controls-v1" if exercise else "draft-1", "mode": "fast" if fast else "normal",
            "repetitions": repetitions, "seed": campaign["seed"], "phases": selected,
            "targets": {x: installed[x] for x in resolved}, "display_policy": campaign["display"],
            "unselected_families": [x for x in MEASUREMENTS if x not in selected], "blocks": blocks,
            "takeover": bool(blocks), "suite_fingerprint": fingerprint(discovery["suite"]),
            "estimate": {"basis": "provisional planning allowances; uncalibrated",
            "minimum_seconds": nominal, "maximum_seconds": 10 + sum(x["deadline_seconds"] for x in blocks),
            "includes": (["fresh profile", "session launch", "calibration", "measurement", "cleanup", "report"]
                         if blocks else ["host discovery", "plan validation", "report"]),
            "retries": 0}, "environment_fingerprint": fingerprint(discovery["environment"])}
    if exercise:
        plan["execution_mode"] = "exercise"
        plan["estimate"]["basis"] = "provisional instrument-control allowances; uncalibrated"
        plan["estimate"]["includes"] = ["fresh profile", "session launch", "instrument controls", "cleanup", "report"]
    elif any(name in MEASUREMENTS for name in selected):
        from .workloads import PHASES as AVAILABLE, implementation
        plan["execution_mode"] = "measure"
        plan["scenario_revision"] = "fixed-measurement-phases-v1"
        plan["measurement_scope"] = "fixed-native-workloads; family coverage remains partial"
        for block in blocks:
            if block["phase"] in AVAILABLE:
                module = implementation(block["phase"])
                block.update(workload=module.VERSION, measurement_executed=True, measurement_spec=module.SPEC,
                             requirements=(list(PHASES[block["phase"]]["requirements"]) if block["phase"] in ("scaling", "overhead") else ["input", "presentation", "instrument-calibration"]) +
                             (["delayed-gpu-producer-explicit-and-implicit"] if block["phase"] == "slow-apps" else []),
                             deadline_seconds=900)
        plan["estimate"]["maximum_seconds"] = 10 + sum(x["deadline_seconds"] for x in blocks)
    plan["fingerprint"] = fingerprint(plan)
    return plan
