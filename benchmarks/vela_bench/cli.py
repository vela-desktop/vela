# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import json
import os
import subprocess
from pathlib import Path
import sys
import tomllib

from . import BenchError, VERSION
from .archive import create_check_archive, decode_json, inspect_archive
from .diagnostics import export_report
from .campaign import blocks_for, export_campaign, read_export, render_export
from .discovery import discover
from .model import load_campaign, make_plan, PHASES, TARGETS
from .report import compare_archives, render_report
from .host import Host, Layout, SAFE_ENV, install, personal_homes, require_current_launcher, require_root, setup_preview, trusted, uninstall, update_launcher
from .pilot import preview


def parser():
    root = argparse.ArgumentParser(description="Vela desktop benchmark suite (initial implementation)")
    root.add_argument("--version", action="version", version=VERSION)
    commands = root.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("setup", help="Provision the dedicated account or refresh installed P0 launcher code")
    setup.add_argument("--dry-run", action="store_true", help="Show persistent resources without changing the host")
    setup.add_argument("--update", action="store_true", help="Refresh owned code and supervision units while preserving the account and PAM isolation")
    setup.add_argument("--json", action="store_true", help="Print the complete installation record")
    setup.add_argument("--probe", type=Path, help="Install the built native vela-bench-probe executable")
    setup.add_argument("--tools-dir", type=Path, help="Install fixed input, presentation, display and capture instruments from a build tools directory")
    pilot = commands.add_parser("session-test", help="P0 lifecycle diagnostic; no performance measurements")
    pilot.add_argument("--dry-run", action="store_true")
    pilot.add_argument("--target", action="append", choices=("vela", "plasma"))
    pilot.add_argument("--seconds", type=int, default=30)
    pilot.add_argument("--yes", action="store_true", help="Acknowledge the printed takeover notice")
    pilot.add_argument("--probe", action="store_true", help="Run both fixed presentation diagnostics in each desktop")
    pilot.add_argument("--conditions", action="store_true", help="Apply and restore verified physical display, hardware brightness and power conditions")
    pilot.add_argument("--calibrate", action="store_true", help="Add common input, capture and collector controls; implies --probe --conditions")
    pilot.add_argument("--output", type=Path, default=Path("benchmark-results"), help="Parent for the automatic diagnostic report")
    report = commands.add_parser("pilot-report", help="Export the latest pilot's diagnostic JSON and offline HTML (root)")
    report.add_argument("--output", type=Path, default=Path("benchmark-results"))
    qualification = commands.add_parser("qualify", help="Run fixed P0 instrument and crash/hang acceptance cases from a TTY")
    qualification.add_argument("--target", action="append", choices=("vela", "plasma"))
    qualification.add_argument("--case", choices=("all", "calibration", "crash", "hang"), default="all")
    qualification.add_argument("--resume", help="Resume a root-owned campaign ID, retaining accepted blocks")
    qualification.add_argument("--yes", action="store_true")
    qualification.add_argument("--dry-run", action="store_true")
    qualification.add_argument("--seed", type=int, default=0)
    qualification.add_argument("--output", type=Path, default=Path("benchmark-results"))
    observe = commands.add_parser("observe", help="P3 native launcher/window/desktop observations from a TTY")
    observe.add_argument("--target", action="append", choices=("vela", "plasma"))
    observe.add_argument("--phase", action="append", choices=("responsiveness", "motion"))
    observe.add_argument("--fast", action="store_true")
    observe.add_argument("--dry-run", action="store_true", help="Show the fixed P3 plan without changing the host")
    observe.add_argument("--resume")
    observe.add_argument("--yes", action="store_true")
    observe.add_argument("--seed", type=int, default=0)
    observe.add_argument("--output", type=Path, default=Path("benchmark-results"))
    campaign_report = commands.add_parser("campaign-report", help="Export the latest qualification/calibration campaign")
    campaign_report.add_argument("--output", type=Path, default=Path("benchmark-results"))
    commands.add_parser("recover", help="Retry restoration of a stopped P0 pilot (root)")
    removal = commands.add_parser("uninstall", help="Remove owned setup resources, preserving result evidence")
    removal.add_argument("--dry-run", action="store_true")
    phases = commands.add_parser("phases", help="List phase IDs, prerequisites and implementation status")
    phases.add_argument("--json", action="store_true")
    for name in ("check", "plan", "run"):
        command = commands.add_parser(name)
        command.add_argument("campaign", nargs="?", help="TOML campaign; omit to discover installed targets")
        command.add_argument("--phase", action="append", choices=tuple(PHASES))
        command.add_argument("--target", action="append", choices=TARGETS)
        command.add_argument("--fast", action="store_true", help="One full repetition instead of three")
        if name in ("plan", "run"):
            command.add_argument("--exercise", action="store_true",
                                 help="Exercise scheduled phase slots with instrument controls only; no family measurements")
        command.add_argument("--json", action="store_true")
        if name == "run":
            command.add_argument("--output", type=Path, default=Path("benchmark-results"), help="Parent of a new private run directory")
            command.add_argument("--resume", help="Resume a calibration or exercise campaign ID")
            command.add_argument("--yes", action="store_true")
    for name in ("status", "report"):
        command = commands.add_parser(name)
        command.add_argument("archive", type=Path)
        command.add_argument("--json", action="store_true")
    compare = commands.add_parser("compare", help="Compare archived metadata; performance comparisons are not implemented yet")
    compare.add_argument("left", type=Path)
    compare.add_argument("right", type=Path)
    compare.add_argument("--json", action="store_true")
    return root


def print_plan(plan):
    print(f"Mode: {plan['mode']}; repetitions: {plan['repetitions']}")
    print(f"Targets: {', '.join(plan['targets'])}")
    print(f"Phases: {', '.join(plan['phases'])}; independent blocks: {len(plan['blocks'])}")
    budget = plan["estimate"]
    print(f"Estimated duration: {budget['minimum_seconds'] / 60:.1f}–{budget['maximum_seconds'] / 60:.1f} minutes")
    print(f"Estimate basis: {budget['basis']}")
    print(f"Plan fingerprint: {plan['fingerprint']}")
    if plan["takeover"]:
        print("Planned live execution requires a real TTY, isolated sessions, display control and restoration.")
        print("Neutral-client input/animation and paired GPU-producer measurements are implemented; full family coverage remains incomplete.")
        if plan.get("execution_mode") == "exercise":
            print("P1 exercise: every phase slot runs fixed instrument controls only; no family measurements.")
        elif plan.get("execution_mode") == "measure":
            print("P2 client-only scope; native shell/window scenarios and complete-family comparisons remain unavailable.")
    else:
        print("Host-side checks only. No session takeover or settings changes.")


def export_latest_pilot(helper, output):
    result = subprocess.run(["/usr/bin/python3", "-I", str(helper), "results"], env=SAFE_ENV,
                            text=True, capture_output=True, timeout=20)
    if result.returncode:
        raise BenchError(result.stderr.strip() or "Cannot inspect pilot results")
    data = decode_json(result.stdout)
    directory = export_report(output, data)
    # Only derived output belongs to the operator; root evidence is unchanged.
    uid, gid = os.environ.get("SUDO_UID", ""), os.environ.get("SUDO_GID", "")
    if uid.isdigit() and gid.isdigit() and int(uid) > 0:
        for path in (directory, *directory.iterdir()):
            os.chown(path, int(uid), int(gid))
    diagnostics = ("not requested" if not data.get("diagnostics_requested") else
                   "complete" if data["diagnostics_complete"] else "incomplete")
    print(f"Pilot: {data['result']}; presentation diagnostics: {diagnostics}")
    print(f"Report: {directory / 'report.html'}")


def export_latest_campaign(helper, output):
    result = subprocess.run(["/usr/bin/python3", "-I", str(helper), "campaign-results"], env=SAFE_ENV,
                            text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise BenchError(result.stderr.strip() or "Cannot inspect calibration campaign")
    data = decode_json(result.stdout)
    directory = export_campaign(output, data)
    uid, gid = os.environ.get("SUDO_UID", ""), os.environ.get("SUDO_GID", "")
    if uid.isdigit() and gid.isdigit() and int(uid) > 0:
        for path in (directory, *directory.rglob("*")):
            os.chown(path, int(uid), int(gid))
    print(f"Report: {directory / 'report.html'}")


def run_calibration_campaign(args, targets, case, seed=0, *, phases=None, exercise=False):
    require_root()
    helper = Layout().helper
    trusted(helper)
    require_current_launcher()
    command = ["/usr/bin/python3", "-I", str(helper), "campaign", "--case", case, "--seed", str(seed)]
    for target in targets:
        if target not in ("vela", "plasma"):
            raise BenchError("Calibration is currently implemented only for Vela/Plasma")
        command += ["--target", target]
    if args.resume:
        command += ["--resume", args.resume]
    if args.yes:
        command.append("--yes")
    if getattr(args, "fast", False):
        command.append("--fast")
    if exercise:
        command.append("--exercise")
    for phase in phases or []:
        command += ["--phase", phase]
    status = subprocess.call(command, env=SAFE_ENV)
    if status in (0, 1):
        export_latest_campaign(helper, args.output)
    return status


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.command == "observe":
            if args.dry_run:
                from .campaign import execution_selection
                current = discover()
                selected = args.target or [name for name in ("vela", "plasma") if name in current["targets"]]
                print(json.dumps(execution_selection(current,selected,case="observe",fast=args.fast,seed=args.seed,
                    phases=args.phase,exercise=False),indent=2))
                return 0
            return run_calibration_campaign(args, args.target or ["vela", "plasma"], "observe", args.seed,
                                            phases=args.phase or ["responsiveness", "motion"])
        if args.command == "qualify":
            if args.dry_run:
                selected = list(preview(args.target)["targets"])
                print(json.dumps({"kind": "p0-qualification", "blocks": blocks_for(selected, args.case, args.seed),
                                  "hardware_required": True, "changes": "controlled hardware, fresh sessions, selected worker/UID faults",
                                  "qualification_proven": False}, indent=2))
                return 0
            return run_calibration_campaign(args, args.target or list(preview()["targets"]), args.case, args.seed)
        if args.command == "campaign-report":
            require_root()
            trusted(Layout().helper)
            require_current_launcher()
            export_latest_campaign(Layout().helper, args.output)
            return 0
        if args.command == "setup":
            if args.dry_run:
                result = ({"operation": "update installed launcher code", "account_and_policies": "preserved",
                           "requires": "stopped workers and reserved UID; terminal or cleanup-failed pilot; trusted existing setup"} if args.update else setup_preview())
                if args.probe:
                    result["native_probe_source"] = str(args.probe)
                if args.tools_dir:
                    result["native_tools_source"] = str(args.tools_dir)
            else:
                require_root()
                result = (update_launcher(Host(), probe=args.probe, tools_dir=args.tools_dir) if args.update else
                          install(Host(), private_homes=personal_homes(), probe=args.probe, tools_dir=args.tools_dir))
            if args.dry_run or args.json:
                print(json.dumps(result, indent=2))
            else:
                print(f"Setup ready: {result['account']} (UID {result['uid']})")
                print(f"Home: {result['home']}")
                print("Launcher code: up to date with this checkout")
                if args.probe or args.tools_dir:
                    print(f"Native probe installed: {Layout().probe}")
                print("Session test has not started.")
            return 0
        if args.command == "uninstall":
            if args.dry_run:
                result = {"remove": setup_preview()["persistent_resources"][1:] +
                          [str(Layout().base / name) for name in ("home", "tmp", "var-tmp")], "account": "vela-benchmark",
                          "preserve": ["pilot evidence and ownership record under /var/lib/vela-benchmark",
                                       "benchmark result archives", "normal system logs"]}
            else:
                require_root()
                result = uninstall(Host())
            print(json.dumps(result, indent=2))
            return 0
        if args.command in ("session-test", "recover", "pilot-report"):
            if args.command == "session-test" and args.dry_run:
                print(json.dumps(preview(args.target, args.seconds, args.probe, args.conditions, args.calibrate), indent=2))
                return 0
            require_root()
            helper = Layout().helper
            trusted(helper)
            if args.command == "pilot-report":
                require_current_launcher()
                export_latest_pilot(helper, args.output)
                return 0
            command = ["/usr/bin/python3", "-I", str(helper), args.command]
            if args.command == "session-test":
                require_current_launcher()
                command += ["--seconds", str(args.seconds)]
                for target in args.target or []:
                    command += ["--target", target]
                if args.yes:
                    command.append("--yes")
                if args.probe:
                    command.append("--probe")
                if args.conditions:
                    command.append("--conditions")
                if args.calibrate:
                    command.append("--calibrate")
            status = subprocess.call(command, env=SAFE_ENV)
            if args.command == "session-test" and status in (0, 1):
                export_latest_pilot(helper, args.output)
            return status
        if args.command == "phases":
            from .measurements import PHASES as AVAILABLE
            data = {name: dict(info, implemented=name in ("check", "calibrate", "report"),
                               client_measurements_available=name in AVAILABLE,
                               exercise_available=name not in ("check", "calibrate", "report")) for name, info in PHASES.items()}
            if args.json:
                print(json.dumps(data, indent=2))
            else:
                for name, info in data.items():
                    state = "available" if info["implemented"] else "planned"
                    if info["client_measurements_available"]:
                        state = "client-only"
                    print(f"{name:16} {state:10} {info['description']}")
                    print(f"  requires: {', '.join(info['requirements']) or 'none'}; takeover: {info['takeover']}")
                    if info["exercise_available"]:
                        print("  --exercise available: fixed instrument controls only")
            return 0
        if args.command in ("plan", "check", "run"):
            campaign = load_campaign(args.campaign)
            phases = args.phase
            if args.command == "check":
                if phases and phases != ["check"]:
                    raise BenchError("The check command only accepts --phase check")
                phases = ["check"]
            discovery = discover()
            plan = make_plan(campaign, discovery, phases=phases, targets=args.target, fast=args.fast,
                             exercise=getattr(args, "exercise", False))
            if args.command == "run":
                if args.exercise:
                    return run_calibration_campaign(args, list(plan["targets"]), "calibrate", campaign["seed"],
                                                    phases=plan["phases"], exercise=True)
                if plan["phases"] == ["calibrate"]:
                    return run_calibration_campaign(args, list(plan["targets"]), "calibrate", campaign["seed"])
                from .measurements import PHASES as AVAILABLE
                if all(phase in AVAILABLE for phase in plan["phases"]):
                    return run_calibration_campaign(args, list(plan["targets"]), "calibrate", campaign["seed"], phases=plan["phases"])
                if args.resume:
                    raise BenchError("Requested phase cannot be resumed: its measurement scenarios are not implemented")
                if plan["takeover"]:
                    raise BenchError("Selected measurement scenarios are not implemented. No sessions or settings were changed. "
                                     "Select responsiveness, motion or slow-apps for P2 client measurements, or use --exercise for schedule controls.")
                directory = create_check_archive(args.output, plan, discovery)
                summary = render_report(directory)
                if args.json:
                    print(json.dumps({"archive": str(directory.resolve()), "summary": summary}, indent=2))
                else:
                    print_plan(plan)
                    print(f"Archive: {directory.resolve()}")
                    print(f"Report: {(directory / 'report.html').resolve()}")
                return 0
            if args.json:
                print(json.dumps(plan if args.command == "plan" else {"plan": plan, "discovery": discovery}, indent=2))
            else:
                print_plan(plan)
                for limitation in discovery["limitations"]:
                    print(f"Note: {limitation}")
                for rejected in discovery["rejected_sessions"]:
                    print(f"Excluded {rejected['target']}: {rejected['reason']}")
            return 0
        if args.command == "compare":
            result = compare_archives(args.left, args.right)
            print(json.dumps(result, indent=2) if args.json else result["reason"] + "\n" + json.dumps(result["differences"], indent=2))
            return 0
        if args.command in ("status", "report") and (args.archive / "manifest.json").exists():
            from .archive import read_json
            if read_json(args.archive / "manifest.json").get("kind") in ("calibration-campaign-export", "execution-campaign-export"):
                result = read_export(args.archive)
                if args.command == "report":
                    render_export(args.archive, result)
                print(json.dumps(result, indent=2) if args.json else
                      f"Stage: {result['stage']}; selection complete: {result['selection_complete']}; P0: {result['qualification']['status']}; full suite: False")
                return 0
        if args.command == "report":
            result = render_report(args.archive)
        else:
            _, _, result = inspect_archive(args.archive)
        print(json.dumps(result, indent=2) if args.json else f"Check: {result['check_status']}; "
              f"performance measurements: none; full suite complete: {result['full_suite_complete']}")
        return 0
    except (BenchError, OSError, tomllib.TOMLDecodeError, subprocess.TimeoutExpired) as exc:
        print(f"vela-bench: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
