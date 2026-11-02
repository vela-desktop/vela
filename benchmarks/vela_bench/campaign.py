# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Common durable block executor for qualification, calibration and P1 exercises."""

import fcntl
import json
import os
from pathlib import Path
import re
import html
import shutil
import uuid

from . import BenchError
from .archive import append_event, atomic_json, read_json
from .discovery import discover, file_hash
from .host import Layout, load_setup, require_workers_stopped, trusted
from .model import fingerprint, load_campaign, make_plan, MEASUREMENTS
from .qualification import audit_pilot, qualification_summary


def identity(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{32}", value):
        raise BenchError("Invalid campaign identity")
    return value


def block_identity(value):
    phases = "|".join(("calibrate", *MEASUREMENTS))
    if not isinstance(value, str) or not re.fullmatch(
            rf"(?:(?:{phases})-(?:vela|plasma)-r[1-3]|(?:crash|hang)-(?:vela|plasma))", value):
        raise BenchError("Invalid fixed campaign block identity")
    return value


def execution_selection(current, targets, *, case, fast, seed, phases, exercise):
    """Build trusted schedules from fixed selections, never an operator script."""
    if type(seed) is not int or not 0 <= seed <= 0xffffffff:
        raise BenchError("Campaign seed is outside uint32 range")
    if case != "observe" and not exercise and not phases:
        return {"kind": "calibration-campaign", "case": case, "fast": fast, "seed": seed,
                "blocks": blocks_for(targets, case, seed), "scenario_revision": "p0-instruments-v1"}
    if case == "observe":
        from .desktop import PHASES as AVAILABLE, SPEC, VERSION
        phases = phases or list(AVAILABLE)
        if exercise or any(phase not in AVAILABLE for phase in phases):
            raise BenchError("Desktop observation supports only responsiveness and motion")
        config = load_campaign(); config["seed"] = seed
        schedule = make_plan(config, {"targets": current["targets"], "environment": current.get("environment", {}),
            "suite": {"installed_files": current.get("launcher_files", {})}},
            phases=phases, targets=targets, fast=fast, exercise=True)
        for block in schedule["blocks"]:
            block.update(workload=VERSION, measurement_executed=True, measurement_spec=SPEC,
                         measurement_budget_seconds=120, requirements=["input", "capture", "native-scene", "instrument-calibration"])
        schedule.update(execution_mode="observe", scenario_revision=VERSION)
        schedule["estimate"]["basis"] = "provisional desktop action/capture allowances; physical acceptance pending"
        schedule["estimate"]["minimum_seconds"] = 10 + sum(b["measurement_budget_seconds"] + b["preparation_budget_seconds"] for b in schedule["blocks"])
        schedule["estimate"]["includes"] = ["fresh profile", "session launch", "instrument controls", "desktop actions and captures", "cleanup", "report"]
        schedule.pop("fingerprint", None); schedule["fingerprint"] = fingerprint(schedule)
        return {"kind":"execution-campaign", "case":"observe", "fast":fast, "seed":seed,
                "blocks":[dict(block, fault=None) for block in schedule["blocks"]],
                "execution":schedule, "scenario_revision":VERSION}
    if case != "calibrate":
        raise BenchError("Phase execution cannot select fault qualification cases")
    if not exercise:
        from .workloads import PHASES as AVAILABLE
        if any(phase not in AVAILABLE for phase in phases):
            raise BenchError("Selected measurement phases are not implemented; no sessions or settings were changed")
    config = load_campaign()
    config["seed"] = seed
    schedule = make_plan(config, {"targets": current["targets"], "environment": current.get("environment", {}),
        "suite": {"installed_files": current.get("launcher_files", {})}},
        phases=phases, targets=targets, fast=fast, exercise=exercise)
    return {"kind": "execution-campaign", "case": "exercise" if exercise else "measure", "fast": fast, "seed": seed,
            "blocks": [dict(block, fault=None) for block in schedule["blocks"]], "execution": schedule,
            "scenario_revision": schedule["scenario_revision"]}


def block_request(layout, run_id, block_id, target):
    directory = layout.base / "campaigns" / identity(run_id)
    trusted(directory, directory=True)
    for name in ("plan.json", "manifest.json"):
        trusted(directory / name)
    plan, manifest = read_json(directory / "plan.json"), read_json(directory / "manifest.json")
    if (plan.get("run_id") != run_id or plan.get("kind") != "execution-campaign"
            or plan.get("case") not in ("exercise", "measure", "observe") or manifest.get("plan_fingerprint") != fingerprint(plan)):
        raise BenchError("Exercise request does not match its immutable campaign")
    matches = [block for block in plan["blocks"] if block["id"] == block_identity(block_id)]
    measured = plan["case"] in ("measure", "observe")
    if plan["case"] == "observe":
        from .desktop import PHASES as AVAILABLE, SPEC, VERSION
    else:
        from .workloads import PHASES as AVAILABLE, implementation
        module = implementation(matches[0]["phase"]) if len(matches) == 1 else implementation("motion")
        SPEC, VERSION = module.SPEC, module.VERSION
    workload = VERSION if measured else "p1-instrument-controls-v1"
    if (len(matches) != 1 or matches[0]["target"] != target
            or matches[0].get("workload") != workload or matches[0].get("fault")
            or matches[0].get("measurement_executed") is not measured
            or (measured and (matches[0].get("phase") not in AVAILABLE or matches[0].get("measurement_spec") != SPEC))):
        raise BenchError("Exercise request does not identify one fixed instrument-control block")
    return {"schema": 1, "campaign_run_id": run_id, "plan_fingerprint": fingerprint(plan),
            "block": matches[0], "workload": workload, "measurement_executed": measured}


def audit_block(source, block, plan, layout):
    audit = audit_pilot(source, block["target"], block["fault"],
                        instruments=not (plan.get("case")=="measure" and block.get("phase")=="overhead"))
    if plan["kind"] == "execution-campaign":
        expected = block_request(layout, plan["run_id"], block["id"], block["target"])
        state = read_json(source / "state.json")
        if audit['status']!='passed':
            audit['error']=state.get('error') or audit.get('error') or 'Pilot did not pass lifecycle/instrument acceptance'
            return audit
        receipt = read_json(source / "block-workload.json")
        audit["checks"]["block_binding"] = state.get("campaign_block") == expected
        audit["checks"]["fixed_workload_complete"] = receipt == dict(expected, status="passed")
        audit["method"] = expected["workload"]
        if plan["case"] == "measure":
            from .workloads import audit_measurements
            audit["measurements"] = audit_measurements(source, block["target"],
                state["conditions_snapshot"]["profile"], block["phase"])
            audit["checks"]["measurement_raw_evidence"] = True
        if plan["case"] == "observe":
            from .desktop import audit_observation
            audit["desktop_observation"] = audit_observation(source, block["target"],
                state["conditions_snapshot"]["profile"], block["phase"])
            audit["checks"]["desktop_raw_evidence"] = True
        audit["status"] = "passed" if all(audit["checks"].values()) else "failed"
    return audit


def blocks_for(targets, case, seed=0):
    if (not isinstance(targets, list) or not targets or len(set(targets)) != len(targets)
            or any(target not in ("vela", "plasma") for target in targets)):
        raise BenchError("Campaign requires installed Vela/Plasma targets once each")
    if type(seed) is not int or not 0 <= seed <= 0xffffffff:
        raise BenchError("Campaign seed is outside uint32 range")
    if case not in ("calibrate", "all", "calibration", "crash", "hang"):
        raise BenchError("Unknown fixed qualification selection")
    blocks = []
    if case in ("calibrate", "all", "calibration"):
        for repetition in range(1, 2 if case == "calibrate" else 3):
            offset = (seed + repetition - 1) % len(targets)
            for target in targets[offset:] + targets[:offset]:
                blocks.append({"id": f"calibrate-{target}-r{repetition}", "target": target, "repetition": repetition, "fault": None})
    if case != "calibrate":
        for fault in ("crash", "hang"):
            if case not in ("all", fault):
                continue
            for target in targets:
                blocks.append({"id": f"{fault}-{target}", "target": target, "repetition": 1, "fault": fault})
    return blocks


def context(host, layout, targets):
    from .pilot import resolve_targets
    setup = load_setup(host, layout)
    inventory = discover()
    return {"account_tag": setup["account_tag"], "uid": setup["uid"], "launcher_files": setup["files"],
            "targets": resolve_targets(targets), "environment": inventory["environment"]}


def condition_identity(snapshot):
    brightness = snapshot["brightness"]
    return {"primary": snapshot["profile"]["primary"], "mode": snapshot["profile"]["mode"],
            "output_identities": snapshot.get("output_identities"),
            "brightness_endpoint": {key: brightness.get(key) for key in ("backend", "connector", "edid_sha256", "maximum", "bus", "device")},
            "power_sources": snapshot["power_sources"]}


def resolved_conditions(directory, plan):
    path = directory / "conditions.json"
    if not path.exists():
        return None
    trusted(path)
    record = read_json(path)
    value = record.get("conditions")
    if (record.get("schema") != 1 or not isinstance(value, dict)
            or record.get("plan_fingerprint") != fingerprint(plan)
            or record.get("conditions_fingerprint") != fingerprint(value)):
        raise BenchError("Resolved campaign conditions changed or belong to another plan")
    return value


def freeze_conditions(layout, run_id, snapshot):
    """Called by the supervised worker after its first active-VT snapshot.

    The declared plan stays immutable. This write-once resolution is bound
    to that plan and precedes any desktop launch or controlled setting.
    """
    directory = layout.base / "campaigns" / identity(run_id)
    trusted(directory, directory=True)
    plan = read_json(directory / "plan.json")
    manifest = read_json(directory / "manifest.json")
    if plan.get("run_id") != run_id or manifest.get("plan_fingerprint") != fingerprint(plan):
        raise BenchError("Campaign plan changed before hardware resolution")
    value = condition_identity(snapshot)
    previous = resolved_conditions(directory, plan)
    if previous is None:
        atomic_json(directory / "conditions.json", {"schema": 1, "plan_fingerprint": fingerprint(plan),
                    "conditions_fingerprint": fingerprint(value), "conditions": value})
    elif previous != value:
        raise BenchError("Hardware conditions differ from the immutable campaign resolution")
    return value


def completion(directory, block):
    path = directory / "blocks" / block_identity(block["id"]) / "complete.json"
    if not path.exists():
        return None
    trusted(path)
    record = read_json(path)
    if record.get("block") != block or record.get("status") != "complete":
        raise BenchError("Campaign completion identity changed")
    source = directory.parent.parent / "pilots" / identity(record.get("pilot_run_id"))
    for name, digest in record.get("evidence", {}).items():
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise BenchError("Unsafe completed-block evidence path")
        trusted(source / name)
        if file_hash(source / name) != digest:
            raise BenchError("Completed-block evidence changed; refusing resume")
    if not record.get("evidence") or record.get("audit", {}).get("status") != "passed":
        raise BenchError("Completed block has no accepted evidence")
    return record


def inspect_campaign(directory):
    directory = Path(directory)
    trusted(directory, directory=True)
    plan = read_json(directory / "plan.json")
    manifest = read_json(directory / "manifest.json")
    if (manifest.get("schema") != 1 or manifest.get("plan_fingerprint") != fingerprint(plan)
            or plan.get("run_id") != directory.name or plan.get("kind") not in ("calibration-campaign", "execution-campaign")):
        raise BenchError("Campaign plan identity or fingerprint changed")
    accepted = []
    for block in plan["blocks"]:
        record = completion(directory, block)
        if record:
            accepted.append({**block, "audit": record["audit"], "profile": record["profile"],
                             "pilot_run_id": record["pilot_run_id"], "status": "complete"})
        else:
            block_dir = directory / "blocks" / block["id"]
            attempts = list(block_dir.glob("attempt-*.json"))
            if attempts:
                last = max(attempts, key=lambda path: int(path.stem.split("-")[-1]))
                attempt = read_json(last)
                accepted.append({**block, "status": attempt["status"], "error": attempt.get("error"),
                                 "pilot_run_id": attempt.get("pilot_run_id"), "audit": attempt.get("audit", {})})
            else:
                accepted.append({**block, "status": "not-run"})
    scheduled = plan["kind"] == "execution-campaign"
    measured = scheduled and plan["case"] == "measure"
    qualification = ({"status": "not-requested", "checks": {}, "scope": "coarse-desktop-observation" if plan["case"]=="observe" else "neutral-client-measurements" if measured else "campaign-core-exercise"} if scheduled else
                     qualification_summary(accepted, list(plan["context"]["targets"])))
    conditions = resolved_conditions(directory, plan)
    if conditions is None and any(block["status"] == "complete" for block in accepted):
        raise BenchError("Completed campaign blocks have no resolved hardware conditions")
    result = {"schema": 1, "kind": plan["kind"], "run_id": plan["run_id"], "plan": plan,
            "resolved_conditions": conditions,
            "stage": manifest["stage"], "blocks": accepted, "qualification": qualification,
            "selection_complete": all(row["status"] == "complete" for row in accepted),
            "full_suite_complete": False, "performance_comparison_available": False}
    observed = scheduled and plan["case"] == "observe"
    if scheduled and not measured and not observed:
        result.update(execution_mode="exercise", measurement_blocks_executed=0,
                      families={name: ("exercised-not-measured" if all(b["status"] == "complete" for b in accepted if b.get("phase") == name)
                                       else "exercise-incomplete") if name in plan["execution"]["phases"] else "not-selected"
                                for name in MEASUREMENTS})
    if measured:
        from .measurements import aggregate
        valid = [row for row in accepted if row["status"] == "complete"]
        result.update(execution_mode="measure", measurement_blocks_executed=len(valid),
                      measurement_scope="fixed-native-workloads", measurements=aggregate(accepted),
                      families={name: (("partial-pattern-only" if name=="scaling" else "partial-session-resources" if name=="overhead" else "partial-client-only") if any(b.get("phase") == name for b in valid)
                                       else "measurement-incomplete") if name in plan["execution"]["phases"] else "not-selected"
                                for name in MEASUREMENTS})
    if observed:
        valid = [row for row in accepted if row["status"] == "complete"]
        result.update(execution_mode="observe", measurement_blocks_executed=len(valid),
                      measurement_scope="coarse-desktop-observation", observations=[
                          {"id":row["id"],"target":row["target"],"phase":row["phase"],
                           "result":row["audit"]["desktop_observation"]} for row in valid],
                      families={name: ("partial-desktop-observation" if any(b.get("phase")==name for b in valid)
                                else "observation-incomplete") if name in plan["execution"]["phases"] else "not-selected" for name in MEASUREMENTS})
    return result


def execute(host, targets=None, *, case="calibrate", resume=None, assume_yes=False, fast=False, seed=0,
            phases=None, exercise=False, layout=Layout()):
    from .pilot import begin, resolve_targets, tty_preconditions
    selected = list(resolve_targets(targets))
    tty_preconditions(host)
    require_workers_stopped(host)
    setup = load_setup(host, layout)
    descriptor = os.open(layout.base / "campaign.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise BenchError("Another calibration/qualification campaign is active") from exc
        root = layout.base / "campaigns"
        root.mkdir(mode=0o700, exist_ok=True)
        trusted(root, directory=True)
        current = context(host, layout, selected)
        selection = execution_selection(current, selected, case=case, fast=fast, seed=seed, phases=phases, exercise=exercise)
        if selection["case"] == "observe":
            scene = layout.helper.parent / "vela-bench-scene"
            if str(scene) not in setup["files"]:
                raise BenchError("Desktop observation requires the installed vela-bench-scene instrument before takeover")
            trusted(scene)
        if selection["case"] == "measure" and "scaling" in selection["execution"]["phases"]:
            pattern = layout.helper.parent / "vela-pattern"
            if str(pattern) not in setup["files"]:
                raise BenchError("Scaling requires installed vela-pattern; run setup --tools-dir BUILD/tools")
            trusted(pattern)
        if selection['case']=='measure' and 'overhead' in selection['execution']['phases']:
            if str(layout.probe) not in setup['files']:
                raise BenchError('Resource animation requires installed vela-bench-probe before takeover')
            trusted(layout.probe)
        if selection["case"] == "measure" and "slow-apps" in selection["execution"]["phases"]:
            producer = layout.helper.parent / "vela-slowgpu"
            if str(producer) not in setup["files"]:
                raise BenchError("Selected slow-apps phase requires the installed vela-slowgpu instrument before takeover")
            trusted(producer)
        if resume:
            directory = root / identity(resume)
            previous = inspect_campaign(directory)
            plan = previous["plan"]
            if plan["context"] != current or any(plan.get(key) != value for key, value in selection.items()):
                raise BenchError("Resume requires identical installed versions, host, account, selection and mode")
        else:
            run_id = uuid.uuid4().hex
            directory = root / run_id
            directory.mkdir(mode=0o700)
            plan = {"schema": 1, "run_id": run_id, **selection,
                    "context": current, "conditions": "primary/native/fixed/scale1/brightness100/performance"}
            atomic_json(directory / "plan.json", plan)
            atomic_json(directory / "manifest.json", {"schema": 1, "plan_fingerprint": fingerprint(plan), "stage": "planned"})
        atomic_json(layout.base / "campaign-active.json", {"run_id": plan["run_id"]})
        pending = [block for block in plan["blocks"] if completion(directory, block) is None]
        print(f"Campaign {plan['run_id']}: {len(pending)} pending blocks; completed blocks are retained.", flush=True)
        print(f"Provisional duration: about {len(pending)}–{len(pending) * 3} minutes; hard per-block budget 900 seconds.", flush=True)
        print("This takes over the machine, changes/restores hardware conditions and launches fresh isolated desktops.", flush=True)
        print("After START, the supervised worker activates the greeter VT to resolve and freeze the hardware profile.", flush=True)
        if any(block["fault"] for block in pending):
            print("Fault qualification kills its worker or freezes only benchmark-UID processes; PID 1 must restore the machine.", flush=True)
        measured = plan.get("case") == "measure"
        observed = plan.get("case") == "observe"
        if observed:
            print("P3 native launcher/window/desktop actions; coarse captures only, no frame-level motion ranking.", flush=True)
        if not observed:
            print("Fixed native measurements; full application-corpus and family coverage remain incomplete." if measured else
                  "These are instrument/feasibility checks, not the five performance benchmarks.", flush=True)
        if exercise:
            print(f"P1 exercise: {', '.join(plan['execution']['phases'])}; {plan['execution']['repetitions']} complete repetitions per target.", flush=True)
            print("Every phase slot runs the same fixed instrument controls. Measurement scenarios are NOT executed.", flush=True)
        if not assume_yes and pending and input("Type START to run all selected blocks: ") != "START":
            raise BenchError("Campaign not authorized")
        manifest = read_json(directory / "manifest.json")
        manifest["stage"] = "running"
        manifest.pop("error", None)
        atomic_json(directory / "manifest.json", manifest)
        active_phase = None
        for block in pending:
            block_dir = directory / "blocks" / block["id"]
            block_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            attempts = max((int(path.stem.split("-")[-1]) for path in block_dir.glob("attempt-*.json")), default=0) + 1
            attempt = {"attempt": attempts, "block": block, "pilot_run_id": uuid.uuid4().hex, "status": "running"}
            attempt_path = block_dir / f"attempt-{attempts:04d}.json"
            atomic_json(attempt_path, attempt)
            phase = block.get("phase", "qualify" if block["fault"] else "calibrate")
            if phase != active_phase:
                active_phase = phase
                append_event(directory / "events.jsonl", {"event": "phase-start", "phase": phase})
                print(f"[phase {phase}]", flush=True)
            append_event(directory / "events.jsonl", {"event": "block-start", **attempt})
            print(f"[{block['id']}] fresh profile/session/calibration; attempt {attempts}", flush=True)
            try:
                if context(host, layout, selected) != plan["context"]:
                    raise BenchError("Installed versions or environment changed during campaign")
                options = {"campaign_block_id": block["id"]} if exercise or measured or observed else {}
                outcome = begin(host, [block["target"]], seconds=5, assume_yes=True, layout=layout,
                      presentation_probe=not block["fault"] and not (measured and phase=="overhead"), controlled=True,
                      calibrate=not block["fault"] and not (measured and phase=="overhead"),
                      countdown=False, run_id=attempt["pilot_run_id"], fault=block["fault"], campaign_run_id=plan["run_id"], **options)
                if outcome == 130:
                    raise KeyboardInterrupt
                source = layout.base / "pilots" / attempt["pilot_run_id"]
                audit = audit_block(source, block, plan, layout)
                attempt["audit"] = audit
                if audit["status"] != "passed":
                    raise BenchError(audit.get('error') or "Selected block did not pass its acceptance checks")
                state = read_json(source / "state.json")
                if condition_identity(state["conditions_snapshot"]) != resolved_conditions(directory, plan):
                    raise BenchError("Executed hardware condition differs from the immutable campaign condition")
                evidence = {}
                for path in source.iterdir():
                    if path.is_file() and not path.is_symlink():
                        trusted(path)
                        with path.open("rb") as stream:
                            os.fsync(stream.fileno())
                        evidence[path.name] = file_hash(path)
                source_fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(source_fd)
                finally:
                    os.close(source_fd)
                attempt.update(status="complete", evidence=evidence, profile=state["conditions_snapshot"]["profile"])
                atomic_json(block_dir / "complete.json", attempt)
                atomic_json(attempt_path, attempt)
                append_event(directory / "events.jsonl", {"event": "block-complete", "block_id": block["id"], "attempt": attempts})
                print(f"Block passed: {block['id']}; acceptance checks and restoration verified.", flush=True)
                observation = audit.get("desktop_observation")
                if observation:
                    if block["phase"] == "responsiveness":
                        bounds = [a["metrics"]["first_observed_response_upper_ms"] for a in observation["actions"]
                                  if a["kind"] == "launcher-open" and a["id"] != "launcher-0"]
                        print(f"  P3 launcher: {len(bounds)} measured openings; first-observation upper bounds {min(bounds):.1f}–{max(bounds):.1f} ms; app launch and desktop hide/restore verified.", flush=True)
                    else:
                        metric = observation["actions"][0]["metrics"]
                        print(f"  P3 window drag: {metric['distance_pixels']:.1f} physical pixels; {len(metric['observed_positions'])} identified observations; coarse capture only.", flush=True)
                for scenario, sample in audit.get("measurements", {}).get("scenarios", {}).items():
                    if 'resources' in sample:
                        metric=sample['resources']
                        print(f"  P4 {scenario}: UID CPU {metric['cpu_percent_one_logical_cpu']:.2f}% of one logical CPU; median PSS {metric['pss_mib_median']:.2f} MiB; {metric['samples']} samples; capture off.",flush=True)
                    if phase=='scaling' and 'metrics' in sample:
                        metric=sample['metrics']
                        print(f"  P4 {scenario}: {metric['different_pixels']} different pixels, {metric['offscreen_pixels']} offscreen; original capture retained.",flush=True)
                    if "input" in sample:
                        metric = sample["input"]["injection_to_presentation_upper_ms"]
                        print(f"  P2 {scenario}: 100 measured input actions; software presentation upper bound p50 {metric['p50']:.3f}, p95 {metric['p95']:.3f} ms.", flush=True)
                    if "motion" in sample:
                        metric = sample["motion"]
                        print(f"  P2 {scenario}: {metric['presented']} presented, {metric['discarded']} discarded; animation interval p95 {metric['presentation_intervals_ms']['p95']:.3f} ms.", flush=True)
                    if "producer" in sample:
                        metric = sample["producer"]
                        print(f"  P2 {scenario}: {metric['completed']} GPU jobs; measured GPU duration p50 {metric['gpu_duration_ms']['p50']:.3f} ms; visible production verified.", flush=True)
                remaining_phase = [row for row in plan["blocks"] if row.get("phase", "qualify" if row["fault"] else "calibrate") == phase]
                if all(completion(directory, row) is not None for row in remaining_phase):
                    append_event(directory / "events.jsonl", {"event": "phase-complete", "phase": phase})
            except (BenchError, OSError, ValueError, KeyboardInterrupt) as exc:
                cancelled = isinstance(exc, KeyboardInterrupt)
                attempt.update(status="interrupted" if cancelled else "failed", error="Operator interrupted the campaign" if cancelled else str(exc))
                atomic_json(attempt_path, attempt)
                manifest.update(stage="interrupted", error=attempt["error"])
                atomic_json(directory / "manifest.json", manifest)
                append_event(directory / "events.jsonl", {"event": "block-interrupted" if cancelled else "block-failed", "block_id": block["id"], "error": attempt["error"]})
                print(f"Block {attempt['status']}: {attempt['error']}. Resume ID: {plan['run_id']}", flush=True)
                return 130 if cancelled else 1
        result = inspect_campaign(directory)
        acceptable = result["selection_complete"]
        if case in ("all", "calibration"):
            checks = result["qualification"]["checks"]
            acceptable = acceptable and all(value for name, value in checks.items()
                if case == "all" or name.endswith(("/independent-calibrations", "/A-A-repeatability")))
        manifest["stage"] = "done" if acceptable else "qualification-failed"
        atomic_json(directory / "manifest.json", manifest)
        print(f"Selection complete: {result['selection_complete']}; P0 qualification: {result['qualification']['status']}", flush=True)
        return 0 if acceptable else 1
    finally:
        os.close(descriptor)


def latest(layout=Layout()):
    pointer = layout.base / "campaign-active.json"
    trusted(pointer)
    return inspect_campaign(layout.base / "campaigns" / identity(read_json(pointer).get("run_id")))


def render_export(directory, data):
    escape = lambda value: html.escape(str(value), quote=True)
    exercise = data.get("execution_mode") == "exercise"
    measured = data.get("execution_mode") == "measure"
    observed = data.get("execution_mode") == "observe"
    document = """<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src 'self'; style-src 'unsafe-inline'">
<title>Vela campaign evidence</title><style>
body{font:16px/1.5 system-ui;max-width:75rem;margin:3rem auto;padding:0 1rem;background:#fafbfc;color:#18232b}
table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:.5rem;border-bottom:1px solid #ccd4dc}
pre{white-space:pre-wrap;overflow-wrap:anywhere}img{max-width:100%}
</style>"""
    document += ("<h1>P3 desktop observation</h1><p>Native launcher, window and desktop actions verified through coarse output captures. No frame-level motion or optical latency claims.</p>" if observed else
                 "<h1>Native benchmark measurements</h1><p>Fixed client, scaling-pattern and session-resource scenarios. Additional shell response, window movement, "
                 "dedicated contention workloads and full application-corpus coverage are incomplete. Software timestamps are not optical latency.</p>" if measured else
                 "<h1>P1 campaign core exercise</h1><p>Scheduled phase slots ran fixed instrument controls only. "
                 "Zero measurement blocks executed. The named benchmark families were not measured.</p>" if exercise else
                 "<h1>Qualification and calibration</h1><p>Instrument acceptance evidence.</p>")
    document += "<p>No comparative desktop performance ranking; the five-family suite is incomplete.</p>"
    document += f"<p>Run: {escape(data['run_id'])}. Stage: {escape(data['stage'])}. Selection complete: {escape(data['selection_complete'])}. P0: {escape(data['qualification']['status'])}.</p>"
    if exercise or measured or observed:
        document += "<h2>Family coverage</h2><pre>" + escape(json.dumps(data["families"], indent=2)) + "</pre>"
    if observed:
        document += "<h2>Verified desktop actions</h2><table><tr><th>Block</th><th>Action</th><th>Observation</th><th>Captures</th></tr>"
        for block in data["observations"]:
            for action in block["result"]["actions"]:
                metric = action["metrics"]
                detail = (f"First observed response: 0–{metric['first_observed_response_upper_ms']:.1f} ms" if action["kind"]=="launcher-open" else
                          f"Identified movement: {metric['distance_pixels']:.1f} physical pixels" if action["kind"]=="drag" else "Final state verified")
                links = " · ".join("<a href='"+escape(block["id"]+'/'+image['image_file'])+"'>"+label+"</a>"
                                    for image,label in ((action['captures'][0],'before'),(action['captures'][-1],'after')))
                document += "<tr>" + "".join("<td>"+escape(value)+"</td>" for value in (block["id"],action["id"],detail))+"<td>"+links+"</td></tr>"
        document += "</table><p>launcher-0 is warm-up. Captures are evidence of coarse state changes, not frame cadence.</p>"
    if measured:
        document += "<h2>Independent block statistics</h2><p>Each row is a scenario/target. Values are the median and range of independent block statistics, in the displayed units; actions/frames are not pooled as repetitions. No p99 or significance claim.</p>"
        document += "<table><tr><th>Phase</th><th>Target</th><th>Scenario</th><th>Blocks</th><th>Statistic</th><th>Median</th><th>Range</th></tr>"
        for group in data["measurements"]:
            for metric, values in group["block_statistics"].items():
                document += "<tr>" + "".join(f"<td>{escape(value)}</td>" for value in
                    (group["phase"], group["target"], group["scenario"], group["independent_blocks"], metric + ' (' + values['unit'] + ')',
                     f"{values['median']:.3f}", f"{values['min']:.3f}–{values['max']:.3f}")) + "</tr>"
        document += "</table><h2>Raw-derived scenario results and paired load effects</h2><pre>" + escape(json.dumps(data["measurements"], indent=2)) + "</pre>"
        document += '<h2>Original scaling captures</h2>'
        for block in data['blocks']:
            if block.get('status')=='complete' and block.get('phase')=='scaling':
                for name, sample in block['audit']['measurements']['scenarios'].items():
                    link=block['id']+'/'+sample['image_file']
                    document += "<p><a href='"+escape(link)+"'>"+escape(block['id']+' / '+name)+"</a></p>"
    document += "<table><tr><th>Block</th><th>Phase</th><th>Target</th><th>Repetition</th><th>State</th><th>Acceptance</th></tr>"
    for block in data["blocks"]:
        document += "<tr>" + "".join(f"<td>{escape(value)}</td>" for value in
            (block["id"], block.get("phase", "qualify" if block.get("fault") else "calibrate"), block["target"],
             block["repetition"], block["status"], block.get("audit", {}).get("status", "not-run"))) + "</tr>"
        if block.get("error"):
            document += f"<tr><td colspan='6'>{escape(block['error'])}</td></tr>"
    document += "</table><h2>" + ("Qualification scope" if exercise or measured else "P0 checks and repeatability") + "</h2><pre>" + escape(json.dumps(data["qualification"], indent=2)) + "</pre>"
    for block in data["blocks"]:
        calibration = block.get("audit", {}).get("calibration", {})
        if calibration:
            document += f"<h2>{escape(block['id'])}: instrument controls</h2>"
            document += "<table><tr><th>Condition</th><th>Actions</th><th>Input → reported presentation p50 (ms)</th><th>Reserved UID CPU (ms)</th></tr>"
            for name, sample in calibration.items():
                accounting, metrics = sample["session_accounting"], sample["metrics"]
                cpu = (accounting["cpu_usage_end_usec"] - accounting["cpu_usage_begin_usec"]) / 1000
                document += "<tr>" + "".join(f"<td>{escape(value)}</td>" for value in
                    (name, metrics["actions"], f"{metrics['injection_to_presentation_upper_ms']['p50']:.3f}", f"{cpu:.3f}")) + "</tr>"
            document += "</table><p>Small instrument samples; CPU totals include each condition's complete workload and setup. No p99 or desktop ranking is inferred.</p>"
        document += f"<h2>{escape(block['id'])}</h2><pre>{escape(json.dumps(block.get('audit', {}), indent=2))}</pre>"
        for image in (f"{block['id']}/{block['target']}-capture-00.png", f"{block['id']}/{block['target']}-capture-11.png"):
            if (directory / image).is_file():
                document += f"<p><img src='{escape(image)}' alt='Original isolated-session calibration capture'></p>"
    document += "<p>Capture is calibrated for coarse state observations only; fine-grained desktop motion remains unsupported.</p></html>\n"
    (directory / "report.html").write_text(document, encoding="utf-8")


def export_campaign(parent, data, layout=Layout()):
    parent = Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    directory = parent / ("campaign-" + identity(data["run_id"]))
    directory.mkdir(mode=0o755)
    hashes = {}

    def copy_evidence(path, output):
        trusted(path)
        before = file_hash(path)
        shutil.copyfile(path, output)
        output.chmod(0o644)
        if file_hash(output) != before or file_hash(path) != before:
            raise BenchError("Source evidence changed during export")
        hashes[str(output.relative_to(directory))] = before

    campaign = layout.base / "campaigns" / data["run_id"]
    if campaign.exists():
        trusted(campaign, directory=True)
        for name in ("events.jsonl", "plan.json", "conditions.json"):
            if (campaign / name).exists():
                copy_evidence(campaign / name, directory / ("campaign-" + name))
        journal = directory / "attempts"
        journal.mkdir(mode=0o755)
        for block in data["blocks"]:
            source = campaign / "blocks" / block_identity(block["id"])
            if not source.exists():
                continue
            trusted(source, directory=True)
            for path in source.iterdir():
                if re.fullmatch(r"(?:attempt-[0-9]+|complete)\.json", path.name):
                    copy_evidence(path, journal / (block["id"] + "-" + path.name))
    for block in data["blocks"]:
        block_identity(block["id"])
        if not block.get("pilot_run_id"):
            continue
        source = layout.base / "pilots" / identity(block["pilot_run_id"])
        if not source.exists():
            continue
        trusted(source, directory=True)
        target = directory / block["id"]
        target.mkdir(mode=0o755)
        for path in source.iterdir():
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", path.name) or path.suffix not in (".json", ".jsonl", ".png", ".stderr"):
                continue
            output = target / path.name
            copy_evidence(path, output)
    atomic_json(directory / "summary.json", data)
    (directory / "summary.json").chmod(0o644)
    hashes["summary.json"] = file_hash(directory / "summary.json")
    kind = "execution-campaign-export" if data["kind"] == "execution-campaign" else "calibration-campaign-export"
    atomic_json(directory / "manifest.json", {"schema": 1, "kind": kind, "run_id": data["run_id"], "files": hashes})
    (directory / "manifest.json").chmod(0o644)
    render_export(directory, data)
    (directory / "report.html").chmod(0o644)
    return directory


def read_export(directory):
    directory = Path(directory)
    manifest = read_json(directory / "manifest.json")
    if manifest.get("kind") not in ("calibration-campaign-export", "execution-campaign-export") or manifest.get("schema") != 1:
        raise BenchError("Not a supported campaign export")
    if "summary.json" not in manifest.get("files", {}):
        raise BenchError("Export has no hashed summary")
    for name, digest in manifest["files"].items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or len(relative.parts) > 2 or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in relative.parts):
            raise BenchError("Unsafe exported evidence path")
        path = directory / relative
        if path.is_symlink() or file_hash(path) != digest:
            raise BenchError("Exported campaign evidence changed")
    data = read_json(directory / "summary.json")
    if data.get("run_id") != manifest.get("run_id") or data.get("full_suite_complete") is not False:
        raise BenchError("Exported campaign identity/coverage changed")
    if manifest["kind"] != data.get("kind", "") + "-export":
        raise BenchError("Exported campaign kind changed")
    if data.get("execution_mode") == "measure":
        from .measurements import aggregate
        from .workloads import audit_measurements
        valid = [block for block in data["blocks"] if block.get("status") == "complete"]
        for block in valid:
            actual = audit_measurements(directory / block_identity(block["id"]), block["target"], block["profile"], block["phase"])
            if actual != block.get("audit", {}).get("measurements"):
                raise BenchError("Exported measurement audit differs from raw evidence")
        if (data.get("measurement_blocks_executed") != len(valid) or data.get("measurements") != aggregate(data["blocks"])
                or data.get("performance_comparison_available") is not False):
            raise BenchError("Exported measurement aggregation or coverage changed")
    if data.get("execution_mode") == "observe":
        from .desktop import audit_observation
        valid = [block for block in data["blocks"] if block.get("status") == "complete"]
        observations = []
        for block in valid:
            actual = audit_observation(directory / block_identity(block["id"]), block["target"], block["profile"], block["phase"])
            if actual != block.get("audit", {}).get("desktop_observation"):
                raise BenchError("Exported desktop audit differs from captured evidence")
            observations.append({"id":block["id"],"target":block["target"],"phase":block["phase"],"result":actual})
        if data.get("observations") != observations or data.get("measurement_blocks_executed") != len(valid):
            raise BenchError("Exported desktop observation summary changed")
    return data
