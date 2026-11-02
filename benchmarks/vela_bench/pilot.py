# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Supervised TTY → installed desktop sessions → TTY feasibility pilot.

Optional fixed controls qualify input, presentation, coarse capture and
hardware restoration. Occupied graphical user sessions are refused.
The privileged launcher accepts only fixed operations, never shell commands.
"""

import argparse
import array
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import signal
import stat
import sys
import time
import uuid

from . import BenchError
from .archive import append_event, atomic_json, read_json
from .discovery import sessions
from .diagnostics import SCENARIOS, pilot_results, run_probe
from .conditions import apply_conditions, restore_conditions, snapshot_conditions, verify_conditions
from .calibration import capture_worker, prepare_fault_input, run_input
from .host import (ACCOUNT, Host, Layout, PILOT_UNIT, SESSION_UNIT, check_account,
                   control_lock, isolation_properties, load_setup, require_root, reset_profile, stop_user, trusted,
                   PILOT_SECONDS, RECOVERY_SECONDS, FAULT_UNIT, require_workers_stopped)

VT_GETSTATE, VT_OPENQRY = 0x5603, 0x5600
TERMINAL = ("done", "failed", "cancelled")


def properties(text):
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def logind_sessions(host):
    rows = json.loads(host.run(["loginctl", "list-sessions", "--json=short", "--no-pager"]).stdout)
    result = []
    for row in rows:
        name = str(row["session"])
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise BenchError("Invalid logind session identity")
        info = host.run(["loginctl", "show-session", name, "--all", "--no-pager"]).stdout
        result.append(dict(properties(info), Id=name))
    return result


def require_no_graphical_sessions(sessions_, *, after_authorization=False):
    """Allow greeters and logged-out remnants, never an online desktop.

    logind retains Type=wayland/x11 while a session is closing, even after
    logout, if processes such as a keyring daemon remain. Active=no alone
    is insufficient: an online desktop on another VT must still block us.
    """
    blocked = [session for session in sessions_
               if session.get("Type") in ("wayland", "x11") and session.get("Class") != "greeter"
               and not (session.get("State") == "closing" and session.get("Active") == "no")]
    if blocked:
        details = [{key: session.get(key, "unknown")
                    for key in ("Id", "Name", "Type", "State", "Active", "Service", "TTY")}
                   for session in blocked]
        reason = ("A graphical user session appeared after authorization" if after_authorization
                  else "A graphical user session is still open; P0 refuses to terminate personal desktops")
        raise BenchError(f"{reason}. Blocking sessions: {json.dumps(details, ensure_ascii=True)}")


def tty_preconditions(host, fd=0):
    try:
        tty = os.ttyname(fd)
    except OSError as exc:
        raise BenchError("Session pilot must start from a real local Linux TTY") from exc
    direct = re.fullmatch(r"/dev/tty([1-9][0-9]*)", tty)
    if not direct and not re.fullmatch(r"/dev/pts/[0-9]+", tty):
        raise BenchError("Session pilot requires /dev/ttyN, outside the desktop")
    # sudo use_pty wraps even a real VT in /dev/pts/N. Resolve the caller's
    # own logind session, not environment hints or some other active login.
    # Keep stdin on the sudo PTY so its input/output policy stays in effect.
    caller = properties(host.run(["loginctl", "show-session", "self", "--all", "--no-pager"]).stdout)
    match = re.fullmatch(r"tty([1-9][0-9]*)", caller.get("TTY", ""))
    if (not match or not 1 <= int(match[1]) <= 63 or caller.get("Type") != "tty"
            or caller.get("Active") != "yes" or caller.get("Remote") != "no" or caller.get("Seat") != "seat0"
            or not re.fullmatch(r"[A-Za-z0-9_-]+", caller.get("Id", ""))):
        raise BenchError("Session pilot requires its own active local logind /dev/ttyN session on seat0")
    origin = int(match[1])
    if direct and int(direct[1]) != origin:
        raise BenchError("Input TTY does not match the caller's logind session")
    sessions_ = logind_sessions(host)
    require_no_graphical_sessions(sessions_)
    own = [x for x in sessions_ if x.get("Id") == caller["Id"] and x.get("TTY") == f"tty{origin}" and x.get("Active") == "yes"
           and x.get("Remote") == "no" and x.get("Seat") == "seat0" and x.get("Type") == "tty"]
    if len(own) != 1:
        raise BenchError("Cannot verify one active local logind TTY session on seat0")
    terminal = fd if direct else os.open(f"/dev/tty{origin}", os.O_RDWR | os.O_NOCTTY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(terminal)
        if not stat.S_ISCHR(info.st_mode) or os.major(info.st_rdev) != 4 or os.minor(info.st_rdev) != origin:
            raise BenchError("Origin TTY is not the expected Linux virtual-console device")
        state = array.array("H", [0, 0, 0])
        fcntl.ioctl(terminal, VT_GETSTATE, state, True)
        if state[0] != origin:
            raise BenchError("Start the pilot on the active TTY")
        query = array.array("i", [0])
        fcntl.ioctl(terminal, VT_OPENQRY, query, True)
    finally:
        if not direct:
            os.close(terminal)
    if query[0] <= 0 or query[0] == origin:
        raise BenchError("No free virtual terminal available")
    return {"origin_vt": origin, "target_vt": query[0], "origin_session": own[0]["Id"]}


def resolve_targets(requested=None, *, check_trust=True):
    available, rejected = sessions()
    chosen = requested if requested is not None else [x for x in ("vela", "plasma") if x in available]
    if not chosen or len(set(chosen)) != len(chosen) or any(x not in ("vela", "plasma") for x in chosen):
        raise BenchError("P0 selects installed Vela and/or Plasma once each; GNOME is not qualified")
    missing = [x for x in chosen if x not in available]
    if missing:
        raise BenchError(f"Requested session unavailable: {', '.join(missing)}; {rejected}")
    for target in chosen if check_trust else []:
        descriptor = available[target]
        trusted(descriptor["session_file"])
        for identity in descriptor["executables"]:
            trusted(identity["path"])
        for identity in descriptor.get("components", {}).values():
            if identity is not None:
                trusted(identity["path"])
    return {x: available[x] for x in ("vela", "plasma") if x in chosen}


def preview(targets=None, seconds=30, presentation_probe=False, controlled=False, calibrate=False):
    if type(seconds) is not int or not 5 <= seconds <= 120:
        raise BenchError("Pilot dwell time must be 5–120 seconds per desktop")
    selected = resolve_targets(targets, check_trust=False)
    return {"kind": "session-lifecycle-pilot", "targets": selected, "target_order": list(selected),
            "seconds_per_target": seconds, "presentation_probe": presentation_probe or calibrate,
            "controlled_conditions": controlled or calibrate, "instrument_calibration": calibrate,
            "estimated_seconds": {"minimum": len(selected) * (seconds + (7 if presentation_probe else 0)) + 10,
                                  "maximum": PILOT_SECONDS + RECOVERY_SECONDS},
            "privileged_path_validation": "deferred to live preflight",
            "requires": ["completed setup", "active local seat0 TTY", "no graphical user sessions", "unused VT"],
            "changes": ["stop/restart an initially active display manager", "switch virtual terminals",
                        "reset reserved UID home/tmp between targets", "start/stop complete installed desktop entries"],
            "limits": ["not a performance campaign", "hardware control requires --conditions or --calibrate",
                       "Arch-style PAM and Plasma Wayland greeter for hardware controls",
                       "coarse capture only; fine-grained desktop motion is unsupported",
                       "P0 acceptance requires an archived passing real-TTY qualification"]}


def state_directory(layout=Layout()):
    trusted(layout.base / "active.json")
    pointer = read_json(layout.base / "active.json")
    run_id = pointer.get("run_id") if isinstance(pointer, dict) else None
    if not isinstance(run_id, str) or not re.fullmatch(r"[0-9a-f]{32}", run_id):
        raise BenchError("Invalid pilot identity")
    directory = layout.base / "pilots" / run_id
    trusted(directory, directory=True)
    return directory


def update(directory, state, stage, **fields):
    state.update(stage=stage, **fields)
    atomic_json(directory / "state.json", state)
    append_event(directory / "events.jsonl", {"event": stage, **fields})


def create_fixture(state):
    fixture = Path(state["fixture"])
    if fixture.parent != Path("/home") or fixture.name != ".vela-benchmark-" + state["run_id"]:
        raise BenchError("Invalid fixture creation path")
    fixture.mkdir(mode=0o755)
    fixture.chmod(0o755)
    sentinel = fixture / "sentinel"
    sentinel.write_text("Synthetic benchmark isolation fixture\n")
    sentinel.chmod(0o644)


def begin(host, targets=None, seconds=30, assume_yes=False, layout=Layout(), presentation_probe=False, controlled=False,
          calibrate=False, countdown=True, run_id=None, fault=None, campaign_run_id=None, campaign_block_id=None):
    if campaign_run_id is not None and not re.fullmatch(r"[0-9a-f]{32}", campaign_run_id):
        raise BenchError("Invalid campaign identity for controlled pilot")
    record = load_setup(host, layout)
    presentation_probe, controlled = presentation_probe or calibrate, controlled or calibrate
    plan = preview(targets, seconds, presentation_probe, controlled, calibrate)
    request = None
    if campaign_block_id is not None:
        from .campaign import block_request
        if campaign_run_id is None or len(plan["targets"]) != 1 or fault:
            raise BenchError("Campaign exercises require one target, fresh calibration and no fault")
        request = block_request(layout, campaign_run_id, campaign_block_id, next(iter(plan["targets"])))
        if not calibrate and not (request["measurement_executed"] and request["block"]["phase"]=="overhead" and controlled and not presentation_probe):
            raise BenchError("Only resource blocks may omit input/capture calibration")
        if request["measurement_executed"] and request["block"]["phase"] == "slow-apps":
            producer = layout.helper.parent / "vela-slowgpu"
            if str(producer) not in record["files"]:
                raise BenchError("Install the bounded GPU producer first: setup --tools-dir BUILD/tools")
            trusted(producer)
    if fault not in (None, "crash", "hang") or (fault and len(plan["targets"]) != 1):
        raise BenchError("Fault qualification selects exactly one desktop and a fixed fault")
    unit = FAULT_UNIT if fault else PILOT_UNIT
    if presentation_probe:
        if str(layout.probe) not in record["files"]:
            raise BenchError("Install the native probe first: sudo ./scripts/vela-bench setup --probe BUILD/tools/vela-bench-probe")
        trusted(layout.probe)
    resolve_targets(list(plan["targets"]))
    tty = tty_preconditions(host)
    conditions = None
    if controlled:
        needed = [layout.helper.parent / "vela-bench-desktop"]
        if "vela" in plan["targets"]:
            needed.append(Path("/usr/bin/wlr-randr"))
        for path in needed:
            trusted(path)
        if str(layout.helper.parent / "vela-bench-desktop") not in record["files"]:
            raise BenchError("Install controlled-session instruments first: setup --tools-dir BUILD/tools")
    if calibrate or fault:
        for name in (("vela-bench-input", "vela-screen-dump") if calibrate else ("vela-bench-input",)):
            path = layout.helper.parent / name
            if str(path) not in record["files"]:
                raise BenchError("Install calibration instruments first: setup --tools-dir BUILD/tools")
            trusted(path)
        if not Path("/dev/uinput").is_char_device():
            raise BenchError("Missing kernel uinput device; enable uinput before qualification")
    user = check_account(record, host, layout)
    if host.pids(user.pw_uid) or layout.path(f"run/user/{user.pw_uid}").exists():
        raise BenchError("Reserved account is in use; not starting a concurrent pilot")
    print(f"P0 session pilot: {' → '.join(plan['targets'])} → tty{tty['origin_vt']}", flush=True)
    print(f"Estimated duration: {plan['estimated_seconds']['minimum']}–{PILOT_SECONDS + RECOVERY_SECONDS} seconds "
          f"({PILOT_SECONDS} s pilot + {RECOVERY_SECONDS} s recovery deadline).", flush=True)
    print("The display manager may stop; disposable benchmark state will be replaced. No personal sessions will be terminated.", flush=True)
    print("This pilot does not establish qualified comparative performance results.", flush=True)
    if controlled:
        print("After START, the supervised worker briefly activates the greeter VT to snapshot original outputs. "
              "It then applies primary/native/fixed/scale1, hardware brightness 100%, performance, with verified restoration.", flush=True)
    if presentation_probe:
        print("Presentation diagnostics: 120-frame animation, then 120 frames with a known 40 ms client sleep, per desktop.", flush=True)
    if calibrate:
        print("Calibration: persistent uinput F24 actions, native output captures, capture-off/on and known-delay controls.", flush=True)
    if not assume_yes and input("Type START to reserve the machine: ") != "START":
        raise BenchError("Pilot not authorized")
    for remaining in range(5 if countdown else 0, 0, -1):
        print(f"Starting in {remaining}s; Ctrl+C cancels.", flush=True)
        time.sleep(1)
    with control_lock(layout):
        active = layout.base / "active.json"
        if active.exists():
            previous = read_json(state_directory(layout) / "state.json")
            if previous.get("stage") not in TERMINAL:
                raise BenchError("An active/incompletely restored pilot exists; run recover before starting another")
        # Recheck after the notice; another login must not slip into takeover.
        if tty_preconditions(host) != tty:
            raise BenchError("TTY/logind conditions changed during authorization")
        result = host.run(["systemctl", "show", "display-manager.service", "--property=ActiveState", "--value"], check=False)
        manager_state = result.stdout.strip()
        if manager_state not in ("active", "inactive", "failed"):
            raise BenchError("Display manager is transitioning; try again after it settles")
        run_id = run_id or uuid.uuid4().hex
        if not re.fullmatch(r"[0-9a-f]{32}", run_id):
            raise BenchError("Invalid fixed pilot run identity")
        directory = layout.base / "pilots" / run_id
        directory.mkdir(mode=0o700)
        state = {"schema": 1, "run_id": run_id, "plan": plan, **tty,
                 "launcher_files": dict(record["files"]),
                 "conditions_snapshot": conditions,
                 "conditions_restore_required": False,
                 "campaign_run_id": campaign_run_id,
                 "campaign_block": request,
                 "fault": fault, "unit": unit,
                 "display_manager_was_active": manager_state == "active", "restore_required": False,
                 "cleanup_errors": [], "result": "pending", "fixture": f"/home/.vela-benchmark-{run_id}",
                 "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "masked_units": []}
        update(directory, state, "queued")
        atomic_json(active, {"run_id": run_id})
        # --no-block lets the worker obtain the same control lock after this
        # operation returns. PID 1 owns both runtime and recovery deadlines.
        try:
            require_no_graphical_sessions(logind_sessions(host), after_authorization=True)
            host.run(["systemctl", "reset-failed", unit], check=False)
            host.run(["systemctl", "start", "--no-block", unit])
        except BenchError as exc:
            update(directory, state, "failed", result="failed", error=str(exc))
            raise
    print(f"Pilot evidence: {directory}. Cancel: sudo systemctl stop {unit}", flush=True)
    last = None
    deadline = time.monotonic() + PILOT_SECONDS + RECOVERY_SECONDS + 30
    started_waiting = time.monotonic()
    try:
        while True:
            current = read_json(directory / "state.json")
            stage = current.get("stage")
            progress = (stage, current.get("target", ""), current.get("scenario", "") if stage in ("probe", "calibrate", "measure", "exercise") else "")
            if progress != last:
                last = progress
                print(f"[{stage}] {progress[1]} {progress[2]}".rstrip(), flush=True)
            if stage in TERMINAL or stage == "cleanup-failed":
                if current.get("error"):
                    print(current["error"], file=sys.stderr)
                for error in current.get("cleanup_errors", []):
                    print(error, file=sys.stderr)
                restoration = "failed" if stage == "cleanup-failed" or current.get("cleanup_errors") else "complete"
                print(f"Pilot result: {current.get('result')}; restoration: {restoration}.", flush=True)
                if presentation_probe:
                    results = pilot_results(directory, current)
                    for row in results["samples"]:
                        metrics = row.get("metrics")
                        detail = (f"{metrics['presented']} presented, {metrics['discarded']} discarded; "
                                  f"submission → presentation p95 {metrics['submission_to_presentation_ms']['p95']:.3f} ms"
                                  if metrics else row.get("error", "no samples"))
                        print(f"  {row['target']}/{row['scenario']}: {row['status']}; {detail}", flush=True)
                    print("The coordinator will export the diagnostic report after restoration.", flush=True)
                return 0 if stage == "done" else 1
            if time.monotonic() >= deadline:
                host.run(["systemctl", "stop", unit], timeout=RECOVERY_SECONDS + 20)
                raise BenchError("Pilot exceeded its supervision/recovery budget; inspect persistent evidence")
            if time.monotonic() - started_waiting > 2:
                service = host.run(["systemctl", "show", unit, "--property=ActiveState", "--value"], timeout=5).stdout.strip()
                if service in ("inactive", "failed"):
                    recover(host, layout)
                    raise BenchError("Pilot unit ended before publishing completion; restoration was retried")
            time.sleep(.5)
    except KeyboardInterrupt:
        host.run(["systemctl", "stop", unit], timeout=RECOVERY_SECONDS + 20)
        return 130


def session_command(target, vt, record, layout=Layout(), *, directory=None):
    if target not in ("vela", "plasma") or type(vt) is not int or not 1 <= vt <= 63:
        raise BenchError("Invalid fixed session operation")
    props = {"Description": "Vela benchmark P0 session " + record["account_tag"],
             "PAMName": "vela-benchmark", "TTYPath": f"/dev/tty{vt}", "StandardInput": "tty-fail",
             "StandardOutput": "journal", "StandardError": "journal", "TTYReset": "yes", "TTYVHangup": "yes",
             "RemainAfterExit": "yes", "UMask": "0077",
             "RuntimeMaxSec": "300", "TimeoutStartSec": "45", "TimeoutStopSec": "10", "KillMode": "control-group",
             "WorkingDirectory": str(layout.home), **isolation_properties(record["uid"], layout, record["private_homes"])}
    if directory is not None:
        directory = Path(directory)
        if directory.parent != layout.base / "pilots" or not re.fullmatch(r"[0-9a-f]{32}", directory.name):
            raise BenchError("Invalid owned session evidence directory")
        trusted(directory, directory=True)
        # PID 1 opens these descriptors before dropping UID. Capture launcher
        # errors even if PAM migrates the process into another journal scope.
        props["StandardOutput"] = props["StandardError"] = "append:" + str(directory / f"{target}-launch.stderr")
    # Keep exit information until our explicit stop. --collect discarded it
    # before the first poll, making a missing unit look like exit status 0.
    command = ["systemd-run", "--quiet", "--service-type=exec", "--unit=" + SESSION_UNIT, "--uid=" + ACCOUNT]
    command += [f"--property={key}={value}" for key, value in props.items()]
    command += [f"--setenv={key}={value}" for key, value in {"XDG_SESSION_TYPE": "wayland", "XDG_SESSION_CLASS": "user",
                 "XDG_SEAT": "seat0", "XDG_VTNR": str(vt), "XDG_SESSION_DESKTOP": target,
                 "XDG_CURRENT_DESKTOP": "Vela:KDE" if target == "vela" else "KDE"}.items()]
    return command + ["/usr/bin/python3", "-I", str(layout.helper), "_session", target]


def launch_session(host, target, vt, record, layout, directory):
    began=int(time.time())-1
    try:
        host.run(session_command(target,vt,record,layout,directory=directory),timeout=50)
    except BenchError as exc:
        evidence={'schema':1,'status':'failed','error':str(exc),'unit':SESSION_UNIT}
        for name,command in (
            ('properties',['systemctl','show',SESSION_UNIT,'--property=LoadState','--property=ActiveState',
                           '--property=Result','--property=ExecMainCode','--property=ExecMainStatus']),
            ('journal',['journalctl','--no-pager','-b','--unit='+SESSION_UNIT,'--since=@'+str(began),'-n','30','-o','short-precise'])):
            try:
                result=host.run(command,check=False,timeout=5)
                evidence[name]=properties(result.stdout) if name=='properties' else result.stdout[-8192:]
                if result.returncode: evidence[name+'_error']=getattr(result,'stderr','')[-1000:]
            except (BenchError,OSError) as diagnostic:
                evidence[name+'_error']=str(diagnostic)
        atomic_json(directory/f'{target}-launch-failure.json',evidence)
        code=evidence.get('properties',{}).get('ExecMainStatus','unavailable')
        detail=evidence.get('journal','').strip()
        raise BenchError(f'{target} session launch failed (ExecMainStatus={code}): {exc}'
                         + ('; session journal: '+detail if detail else '')) from exc


def reserve_vt(host, directory, state, layout=Layout()):
    # logind may automatically start a getty when a free VT becomes active.
    # Never vhangup a login prompt or remove a pre-existing administrator mask.
    for name in ("getty", "autovt"):
        unit = f"{name}@tty{state['target_vt']}.service"
        info = properties(host.run(["systemctl", "show", unit, "--property=ActiveState", "--property=UnitFileState"], check=False).stdout)
        if info.get("ActiveState") not in ("inactive", "failed"):
            raise BenchError(f"Target VT is occupied by {unit}")
        # UnitFileState belongs to the loaded canonical unit, not necessarily
        # to this requested alias name. Masking getty@ttyN can make a query of
        # autovt@ttyN report masked while its own template still starts a getty.
        # Preserve explicit administrator masks, but reserve BOTH path names.
        paths=[layout.path(parent)/unit for parent in ('etc/systemd/system','run/systemd/system')]
        def masked(path):
            return ((path.is_symlink() and os.readlink(path)=='/dev/null')
                    or (not path.is_symlink() and path.is_file() and path.stat().st_size==0))
        if not any(masked(path) for path in paths):
            state["masked_units"].append(unit)
            update(directory, state, "reserve-vt")
            host.run(["systemctl", "mask", "--runtime", unit])
            if not masked(paths[1]):
                raise BenchError(f'Target VT runtime mask was not created for {unit}')
    for name in ('getty','autovt'):
        unit=f"{name}@tty{state['target_vt']}.service"
        info=properties(host.run(['systemctl','show',unit,'--property=LoadState','--property=ActiveState'],check=False).stdout)
        if info.get('LoadState')!='masked' or info.get('ActiveState') not in ('inactive','failed'):
            raise BenchError(f'Target VT reservation did not hold for {unit}; refusing takeover')


def select_login_session(sessions_, uid):
    owned = [x for x in sessions_ if x.get("User") == str(uid)]
    managers = [x for x in owned if x.get("Class") in ("manager", "manager-early")]
    candidates = [x for x in owned if x not in managers]
    if len(candidates) != 1:
        raise BenchError("Expected exactly one reserved-UID login session besides its user manager")
    return candidates[0], managers


def check_profile_environment(environment, home, runtime):
    expected = {"HOME": home, "XDG_RUNTIME_DIR": runtime, "XDG_CONFIG_HOME": home + "/.config",
                "XDG_DATA_HOME": home + "/.local/share", "XDG_STATE_HOME": home + "/.local/state",
                "XDG_CACHE_HOME": home + "/.cache", "DBUS_SESSION_BUS_ADDRESS": f"unix:path={runtime}/bus"}
    for key, value in expected.items():
        actual = environment.get(key)
        if (key in ("HOME", "XDG_RUNTIME_DIR") or actual) and actual != value:
            raise BenchError(f"Unexpected {key} in session/user-manager environment")


def verify_session(host, record, state, layout=Layout()):
    uid = record["uid"]
    session, managers = select_login_session(logind_sessions(host), uid)
    for key, value in {"Active": "yes", "Remote": "no", "Type": "wayland", "Class": "user", "Seat": "seat0",
                       "VTNr": str(state["target_vt"])}.items():
        if session.get(key) != value:
            raise BenchError(f"Session {key}: expected {value}, observed {session.get(key)}")
    runtime = layout.path(f"run/user/{uid}")
    info = runtime.stat()
    if info.st_uid != uid or stat.S_IMODE(info.st_mode) != 0o700 or not stat.S_ISSOCK((runtime / "bus").stat().st_mode):
        raise BenchError("Invalid native runtime directory/user bus")
    manager = host.run(["systemctl", "show", f"user@{uid}.service", "--property=MainPID", "--value"]).stdout.strip()
    if not manager.isdigit() or int(manager) <= 1:
        raise BenchError("Missing independent user manager")
    leader = session.get("Leader", "")
    if not leader.isdigit() or int(leader) <= 1:
        raise BenchError("Missing graphical session leader")
    # The fixture is deliberately world-readable in the host view. Prove
    # exclusion in BOTH process trees, without inspecting personal documents.
    for pid in (leader, manager):
        view = Path(f"/proc/{pid}/root")
        if (view / state["fixture"].lstrip("/")).exists():
            raise BenchError("Synthetic personal-home fixture visible in target/user-manager namespace")
        if not (view / str(layout.home).lstrip("/") / ".pilot-profile").is_file():
            raise BenchError("Managed NSS home unavailable in target/user-manager namespace")
        raw = Path(f"/proc/{pid}/environ").read_bytes()
        environment = dict(item.decode("utf-8", errors="replace").split("=", 1) for item in raw.split(b"\0") if b"=" in item)
        check_profile_environment(environment, str(layout.home), str(runtime))
    bus = f"unix:path={runtime}/bus"
    query = host.run(["setpriv", f"--reuid={uid}", f"--regid={record['gid']}", "--clear-groups", "busctl",
                      "--address=" + bus, "get-property", "org.freedesktop.systemd1",
                      "/org/freedesktop/systemd1/unit/graphical_2dsession_2etarget", "org.freedesktop.systemd1.Unit", "ActiveState"])
    if shlex.split(query.stdout) != ["s", "active"]:
        raise BenchError("The full graphical session target is not active")
    return {"session": session, "manager_sessions": managers, "user_manager_pid": int(manager), "runtime": str(runtime),
            "fixture_hidden_from_both_trees": True, "graphical_session_target": "active"}


def freeze_reserved_uid(host, uid):
    # Open the pidfd before reading its credentials. Even if /proc/PID is
    # reused, signaling this fd cannot target the replacement process.
    for pid in host.pids(uid):
        try:
            fd = os.pidfd_open(pid)
            try:
                status = Path(f"/proc/{pid}/status").read_text()
                uids = next(line.split()[1:] for line in status.splitlines() if line.startswith("Uid:"))
                if set(map(int, uids)) == {uid}:
                    signal.pidfd_send_signal(fd, signal.SIGSTOP)
            finally:
                os.close(fd)
        except (ProcessLookupError, FileNotFoundError):
            pass


def preserve_session_log(directory, state, record, layout):
    """Read only the reserved profile's fixed log, without following links.

    Called after stopping the UID and before deleting its profile. Preserve
    the first copy across repeated recovery attempts, including missing logs.
    """
    if state.get("target") != "vela":
        return
    destination = directory / "vela-session-log.json"
    if destination.exists():
        return
    evidence = {"schema": 1, "source": ".local/state/vela/vela.log", "status": "missing"}
    opened = []
    try:
        fd = os.open(layout.home, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        opened.append(fd)
        for component in (".local", "state", "vela"):
            fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            opened.append(fd)
        fd = os.open("vela.log", os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=fd)
        opened.append(fd)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != record["uid"] or info.st_nlink != 1:
            raise BenchError("Reserved session log must be an owned regular file without hard links")
        limit = 131072
        os.lseek(fd, max(0, info.st_size - limit), os.SEEK_SET)
        raw = os.read(fd, limit)
        evidence.update(status="collected", truncated=info.st_size > limit, text=raw.decode("utf-8", errors="replace"))
    except FileNotFoundError:
        pass
    except (OSError, BenchError) as exc:
        evidence.update(status="unavailable", error=str(exc))
    finally:
        for fd in reversed(opened):
            os.close(fd)
    atomic_json(destination, evidence)


def preserve_session_stop(directory, state, evidence):
    target = state.get("target")
    if target in ("vela", "plasma") and evidence is not None:
        path = directory / f"{target}-session-stop.json"
        if not path.exists():
            atomic_json(path, evidence)


def wait_session(host, record, state, directory, layout):
    deadline = time.monotonic() + 45
    last_error = "session not ready"
    while time.monotonic() < deadline:
        try:
            return verify_session(host, record, state, layout)
        except (BenchError, OSError) as exc:
            last_error = str(exc)
        unit = properties(host.run(["systemctl", "show", SESSION_UNIT, "--property=LoadState",
            "--property=ActiveState", "--property=SubState", "--property=Result", "--property=ExecMainCode", "--property=ExecMainStatus",
            "--property=MainPID", "--property=ExecStart", "--property=InvocationID"],
            timeout=5, check=False).stdout)
        if (unit.get("LoadState") == "not-found" or unit.get("ActiveState") in ("inactive", "failed")
                or unit.get("SubState") == "exited"):
            known = unit.get("LoadState") != "not-found"
            atomic_json(directory / f"{state['target']}-session-exit.json", {"schema": 1, "unit": unit,
                "exit_status_available": known, "readiness_error": last_error})
            status = (f"code={unit.get('ExecMainCode')}, status={unit.get('ExecMainStatus')}, result={unit.get('Result')}"
                      if known else "unit disappeared; actual exit status unavailable")
            raise BenchError(f"{state['target']} session exited before readiness ({status}); {last_error}")
        time.sleep(.5)
    raise BenchError(f"Session readiness timed out: {last_error}")


def worker(host, layout=Layout()):
    record = load_setup(host, layout)
    with control_lock(layout, wait=True):
        directory = state_directory(layout)
        state = read_json(directory / "state.json")
        if state.get("stage") != "queued":
            raise BenchError("Pilot is not queued")
        try:
            request = state.get("campaign_block")
            if request:
                from .campaign import block_request
                targets = list(state["plan"]["targets"])
                resource_only = (request.get('measurement_executed') is True and request.get('block',{}).get('phase')=='overhead'
                                 and state['plan'].get('instrument_calibration') is False and state['plan'].get('presentation_probe') is False)
                if (len(targets) != 1 or (not state["plan"].get("instrument_calibration") and not resource_only)
                        or not state["plan"].get("controlled_conditions") or state.get("fault")
                        or request != block_request(layout, state["campaign_run_id"], request["block"]["id"], targets[0])):
                    raise BenchError("Worker exercise request changed before takeover")
            require_no_graphical_sessions(logind_sessions(host), after_authorization=True)
            state["restore_required"] = True
            update(directory, state, "takeover")
            create_fixture(state)
            if state["plan"].get("controlled_conditions"):
                if not state["display_manager_was_active"]:
                    raise BenchError("Controlled snapshot requires an initially active display manager")
                # Persist restore_required before any VT switch. ExecStopPost
                # restores the origin even if this worker dies during probing.
                update(directory, state, "snapshot")
                snapshot = snapshot_conditions(host, layout, logind_sessions(host), activate=True)
                update(directory, state, "snapshot", conditions_snapshot=snapshot)
                if state.get("campaign_run_id"):
                    from .campaign import freeze_conditions
                    freeze_conditions(layout, state["campaign_run_id"], snapshot)
                # Probing may take time on a resumed DRM compositor. Never
                # stop a personal session that appeared during acquisition.
                require_no_graphical_sessions(logind_sessions(host), after_authorization=True)
                update(directory, state, "takeover", conditions_restore_required=True)
            if state["display_manager_was_active"]:
                host.run(["systemctl", "stop", "display-manager.service"], timeout=45)
            reserve_vt(host, directory, state, layout)
            # JSON object key sorting must never reorder the planned targets.
            order = state["plan"].get("target_order", list(state["plan"]["targets"]))
            diagnostic_failures = []
            for target in order:
                identity = state["plan"]["targets"][target]
                if resolve_targets([target])[target] != identity:
                    raise BenchError("Installed target identity changed after authorization")
                update(directory, state, "prepare", target=target)
                stop_user(host, record, layout)
                reset_profile(host, record, layout)
                if request and request["workload"] == "p3-desktop-observation-v1":
                    from .desktop import prepare_scene
                    prepare_scene(layout, record)
                marker = layout.home / ".pilot-profile"
                marker.write_text(target + "\n")
                os.chown(marker, record["uid"], record["gid"])
                update(directory, state, "launch", target=target)
                descriptor = os.open(directory / f"{target}-launch.stderr", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                os.close(descriptor)
                host.run(["chvt", str(state["target_vt"])])
                launch_session(host,target,state['target_vt'],record,layout,directory)
                evidence = wait_session(host, record, state, directory, layout)
                atomic_json(directory / f"{target}-session.json", evidence)
                if state.get("conditions_snapshot"):
                    update(directory, state, "configure", target=target)
                    observed = apply_conditions(host, record, layout, target, state["conditions_snapshot"])
                    atomic_json(directory / f"{target}-conditions.json", observed)
                if state.get("fault"):
                    fault_input, sysname = prepare_fault_input(host, layout, directory)
                    update(directory, state, "fault", target=target, fault_injected=state["fault"], fault_uinput_sysfs=sysname)
                    if state["fault"] == "crash":
                        os.kill(os.getpid(), signal.SIGKILL)
                        raise BenchError("Crash fixture unexpectedly returned")
                    freeze_reserved_uid(host, record["uid"])
                    while True:
                        signal.pause()  # PID 1's fixed 90-second worker deadline must recover it.
                update(directory, state, "observe", target=target)
                deadline = time.monotonic() + state["plan"]["seconds_per_target"]
                while time.monotonic() < deadline:
                    verify_session(host, record, state, layout)
                    time.sleep(1)
                if state["plan"].get("presentation_probe"):
                    for scenario in SCENARIOS:
                        verify_session(host, record, state, layout)
                        update(directory, state, "probe", target=target, scenario=scenario)
                        outcome = run_probe(host, record, directory, target, scenario, layout)
                        if outcome["status"] != "complete":
                            diagnostic_failures.append(f"{target}/{scenario}: {outcome.get('error', 'failed')}")
                        verify_session(host, record, state, layout)
                if state.get("conditions_snapshot"):
                    verify_conditions(host, record, layout, target, state["conditions_snapshot"])
                def health():
                    verify_session(host, record, state, layout)
                    if host.run(["fgconsole"], timeout=5).stdout.strip() != str(state["target_vt"]):
                        raise BenchError("Measurement lost the active benchmark VT")
                if state["plan"].get("instrument_calibration"):
                    calibration = []
                    for condition in ("input", "input-capture", "input-delay"):
                        update(directory, state, "calibrate", target=target, scenario=condition)
                        sample = run_input(host, record, directory, target, layout, health,
                            delay=40 if condition == "input-delay" else 0,
                            capture_profile=state["conditions_snapshot"]["profile"] if condition == "input-capture" else None)
                        calibration.append(sample)
                        if sample["status"] != "passed" or sample["session_accounting"]["status"] != "passed":
                            diagnostic_failures.append(f"{target}/{condition}: {sample.get('error', 'session accounting unavailable')}")
                    if calibration[0]["status"] == calibration[2]["status"] == "passed":
                        baseline = calibration[0]["metrics"]["injection_to_presentation_upper_ms"]["p50"]
                        delayed = calibration[2]["metrics"]["injection_to_presentation_upper_ms"]["p50"]
                        if delayed - baseline < 25:
                            diagnostic_failures.append(f"{target}: known 40 ms defect not distinguished by input timing")
                    atomic_json(directory / f"{target}-calibration.json", {"schema": 1, "conditions": calibration,
                        "sensitivity_minimum_difference_ms": 25, "comparative_benchmark": False})
                    verify_conditions(host, record, layout, target, state["conditions_snapshot"])
                if request:
                    phase = request["block"]["phase"]
                    update(directory, state, "measure" if request["measurement_executed"] else "exercise", target=target, scenario=phase)
                    if request["measurement_executed"] and not diagnostic_failures:
                        if request["workload"] == "p3-desktop-observation-v1":
                            from .desktop import run_observation as run_workload
                        else:
                            from .workloads import implementation
                            run_workload = implementation(phase).run_workload
                        measured = run_workload(host, record, directory, target, layout,
                            state["conditions_snapshot"]["profile"], phase, health,
                            progress=lambda label: update(directory, state, "measure", target=target, scenario=phase + '/' + label))
                        if measured["status"] != "passed":
                            diagnostic_failures.append(f"{target}/{phase}: " + "; ".join(measured["failures"]))
                        verify_conditions(host, record, layout, target, state["conditions_snapshot"])
                    atomic_json(directory / "block-workload.json", dict(request,
                                status="failed" if diagnostic_failures else "passed"))
                update(directory, state, "teardown", target=target)
                stopped = stop_user(host, record, layout)
                preserve_session_stop(directory, state, stopped)
                preserve_session_log(directory, state, record, layout)
            fields = {"result": "failed" if diagnostic_failures else "passed", "lifecycle_result": "passed"}
            if diagnostic_failures:
                fields["error"] = "Presentation diagnostics failed: " + "; ".join(diagnostic_failures)
            update(directory, state, "restoring", **fields)
        except BaseException as exc:
            cause = exc
            while cause is not None:
                if isinstance(getattr(cause, "operation", None), dict):
                    atomic_json(directory / "failed-operation.json", cause.operation)
                    break
                cause = cause.__cause__
            update(directory, state, "restoring", result="failed", error=str(exc))
            raise
    # ExecStopPost owns cleanup, including when this process is killed.


def restore_display_manager(host, directory, state):
    """Retry one rate-limited start only after a verified clean stop.

    Short qualification cases can exhaust an installed manager's start
    budget without any manager crash. Never reset a pre-existing failure or
    keep resetting a genuinely failing service. Do not change host limits.
    """
    def observe():
        return properties(host.run(["systemctl", "show", "display-manager.service",
            "--property=ActiveState", "--property=Result"], timeout=5, check=False).stdout)
    before = observe()
    evidence = {"schema": 1, "before": before, "rate_limit_reset": False, "status": "pending"}
    path = directory / "display-manager-recovery.json"
    atomic_json(path, evidence)
    try:
        if before.get("ActiveState") != "active":
            try:
                host.run(["systemctl", "start", "display-manager.service"], timeout=45)
            except BenchError as exc:
                blocked = observe()
                evidence.update(first_error=str(exc), blocked=blocked)
                if (before != {"ActiveState": "inactive", "Result": "success"}
                        or blocked.get("Result") != "start-limit-hit" or blocked.get("ActiveState") != "failed"
                        or state.get("display_manager_rate_limit_reset")):
                    raise
                # Persist the single retry before resetting this one service's
                # transient counter. A second recovery cannot start a loop.
                update(directory, state, "restoring", display_manager_rate_limit_reset=True)
                evidence["rate_limit_reset"] = True
                atomic_json(path, evidence)
                host.run(["systemctl", "reset-failed", "display-manager.service"], timeout=5)
                host.run(["systemctl", "start", "display-manager.service"], timeout=45)
        evidence["status"] = "passed"
    except BenchError as exc:
        evidence.update(status="failed", error=str(exc))
        raise
    finally:
        atomic_json(path, evidence)


def restore_hardware_conditions(host, layout, directory, state, record):
    """Restore the exact snapshot; restart a rejecting greeter at most once.

    A successful systemd start does not imply a working DRM greeter. For
    example, amdgpu can reject KWin's chosen RA24 framebuffer format. Only
    a native SetConfig failure permits this recovery, never another mode,
    backend, benchmark environment or an occupied personal desktop.
    """
    first = restore_conditions(host, layout, state["conditions_snapshot"], lambda: logind_sessions(host))
    path = directory / "conditions-recovery.json"
    evidence = read_json(path) if path.exists() else {"schema": 1, "attempts": [], "greeter_restart": False,
                                                    "attempt_count": 0}
    def save_attempt(result):
        evidence["attempts"].append(result)
        evidence["attempt_count"] += 1
        # Keep the original failure/restart evidence and the latest recovery
        # attempts, including across explicit recover invocations.
        evidence["attempts"] = evidence["attempts"][:2] + evidence["attempts"][2:][-6:]
        atomic_json(path, evidence)
    save_attempt(first)
    if (first["status"] == "passed" or not first.get("output_restart_eligible")
            or not state["display_manager_was_active"] or state.get("output_greeter_restart")):
        return first
    try:
        require_no_graphical_sessions(logind_sessions(host), after_authorization=True)
        if host.pids(record["uid"]) or layout.path(f"run/user/{record['uid']}").exists():
            raise BenchError("Greeter restart withheld: reserved UID still has processes/runtime state")
        # Durable before stopping: interrupted recovery must not create a
        # repeated restart loop on subsequent recover invocations.
        update(directory, state, "restoring", output_greeter_restart=True)
        evidence["greeter_restart"] = True
        atomic_json(path, evidence)
        host.run(["systemctl", "stop", "display-manager.service"], timeout=45)
        require_no_graphical_sessions(logind_sessions(host), after_authorization=True)
        restore_display_manager(host, directory, state)
        # Resolve the new greeter's credentials/socket and all physical modes
        # afresh. Do not reuse the old process environment or backend mode IDs.
        second = restore_conditions(host, layout, state["conditions_snapshot"], lambda: logind_sessions(host))
        save_attempt(second)
        return second
    except (BenchError, OSError) as exc:
        evidence["restart_error"] = str(exc)
        first["errors"].append("Output greeter recovery: " + str(exc))
        return first
    finally:
        atomic_json(path, evidence)


def recover(host, layout=Layout()):
    record = load_setup(host, layout)
    with control_lock(layout):
        directory = state_directory(layout)
        state = read_json(directory / "state.json")
        if state.get("stage") in TERMINAL:
            return
        errors = []
        if os.environ.get("SERVICE_RESULT"):
            state["supervision"] = {key: os.environ.get(key) for key in ("SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS")}
        if state.get("result") == "pending":
            state.update(result="failed", error="Pilot interrupted before completing its declared work")
        if state.get("boot_id") and state["boot_id"] != Path("/proc/sys/kernel/random/boot_id").read_text().strip():
            # Runtime units/masks vanished at boot. Do not apply an old VT or
            # display-manager snapshot to the new login environment.
            state.update(restore_required=False, masked_units=[], result="failed", error="Host rebooted during pilot")
        update(directory, state, "restoring")
        stopped = False
        try:
            released = stop_user(host, record, layout)
            preserve_session_stop(directory, state, released)
            preserve_session_log(directory, state, record, layout)
            reset_profile(host, record, layout)
            stopped = True
        except (BenchError, OSError) as exc:
            errors.append(str(exc))
        if state.get("fault_uinput_sysfs"):
            sysname = state["fault_uinput_sysfs"]
            if not re.fullmatch(r"/sys/devices/virtual/input/input[0-9]+", sysname):
                state["fault_uinput_cleanup"] = "failed"
                errors.append("Invalid owned fault-input device identity")
            else:
                deadline = time.monotonic() + 2
                while Path(sysname).exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                state["fault_uinput_cleanup"] = "failed" if Path(sysname).exists() else "passed"
                if state["fault_uinput_cleanup"] != "passed":
                    errors.append("Fault recovery left its virtual input device registered")
        fixture = Path(state["fixture"])
        if fixture.parent != Path("/home") or fixture.name != ".vela-benchmark-" + state["run_id"]:
            errors.append("Invalid fixture cleanup path")
        else:
            try:
                (fixture / "sentinel").unlink(missing_ok=True)
                if fixture.exists():
                    fixture.rmdir()
            except OSError as exc:
                errors.append(str(exc))
        if state.get("restore_required"):
            if state["display_manager_was_active"]:
                if stopped:
                    try:
                        restore_display_manager(host, directory, state)
                    except BenchError as exc:
                        errors.append(str(exc))
                else:
                    errors.append("Display manager restart withheld: reserved UID was not fully stopped")
            if stopped and state.get("conditions_snapshot") and state.get("conditions_restore_required", True):
                state["conditions_restore"] = restore_hardware_conditions(host, layout, directory, state, record)
                errors.extend(state["conditions_restore"]["errors"])
            try:
                observed = host.run(["systemctl", "show", "display-manager.service", "--property=ActiveState", "--value"], timeout=5, check=False).stdout.strip()
                expected = "active" if state["display_manager_was_active"] else "inactive"
                if observed != expected and not (expected == "inactive" and observed == "failed"):
                    errors.append(f"Display manager restoration mismatch: {observed}")
                host.run(["chvt", str(state["origin_vt"])], timeout=5)
                if host.run(["fgconsole"], timeout=5).stdout.strip() != str(state["origin_vt"]):
                    errors.append("Originating TTY was not restored")
            except BenchError as exc:
                errors.append(str(exc))
        for unit in state.get("masked_units", []):
            if unit not in (f"getty@tty{state['target_vt']}.service", f"autovt@tty{state['target_vt']}.service"):
                errors.append("Invalid owned VT mask")
                continue
            try:
                host.run(["systemctl", "unmask", "--runtime", unit], timeout=5)
            except BenchError as exc:
                errors.append(str(exc))
        stage = "cleanup-failed" if errors else "done" if state.get("result") == "passed" else "failed"
        update(directory, state, stage, cleanup_errors=errors)
        if errors:
            raise BenchError("Pilot restoration failed: " + "; ".join(errors))


def desktop_environment(target, user, inherited):
    if os.getuid() != user.pw_uid or os.geteuid() != user.pw_uid or user.pw_name != ACCOUNT:
        raise BenchError("Desktop entry may only execute as the reserved unprivileged account")
    required = {"XDG_SESSION_ID", "XDG_SEAT", "XDG_VTNR"}
    if not required <= inherited.keys() or inherited["XDG_SEAT"] != "seat0":
        raise BenchError("Missing PAM/logind session environment")
    runtime = f"/run/user/{user.pw_uid}"
    environment = {"PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": user.pw_dir,
                   "USER": ACCOUNT, "LOGNAME": ACCOUNT, "SHELL": user.pw_shell,
                   "XDG_CONFIG_HOME": user.pw_dir + "/.config", "XDG_DATA_HOME": user.pw_dir + "/.local/share",
                   "XDG_STATE_HOME": user.pw_dir + "/.local/state", "XDG_CACHE_HOME": user.pw_dir + "/.cache",
                   "XDG_DATA_DIRS": "/usr/local/share:/usr/share", "XDG_CONFIG_DIRS": "/etc/xdg",
                   "XDG_RUNTIME_DIR": runtime, "DBUS_SESSION_BUS_ADDRESS": f"unix:path={runtime}/bus",
                   "XDG_SESSION_TYPE": "wayland", "XDG_SESSION_CLASS": "user", "XDG_SESSION_DESKTOP": target,
                   "XDG_CURRENT_DESKTOP": "Vela:KDE" if target == "vela" else "KDE"}
    environment.update({key: inherited[key] for key in required})
    return environment


def launcher_main(argv=None):
    parser = argparse.ArgumentParser(description="Fixed-operation Vela benchmark P0 launcher")
    sub = parser.add_subparsers(dest="command", required=True)
    live = sub.add_parser("session-test")
    live.add_argument("--target", action="append", choices=("vela", "plasma"))
    live.add_argument("--seconds", type=int, default=30)
    live.add_argument("--yes", action="store_true")
    live.add_argument("--probe", action="store_true")
    live.add_argument("--conditions", action="store_true")
    live.add_argument("--calibrate", action="store_true")
    for name in ("recover", "_pilot", "_recover", "results", "campaign-results"):
        sub.add_parser(name)
    child = sub.add_parser("_session")
    child.add_argument("target", choices=("vela", "plasma"))
    sub.add_parser("_resource-workload")
    capture = sub.add_parser("_capture")
    capture.add_argument("target", choices=("vela", "plasma"))
    capture.add_argument("primary")
    capture.add_argument("--one", action="store_true")
    campaign = sub.add_parser("campaign")
    campaign.add_argument("--case", choices=("calibrate", "all", "calibration", "crash", "hang", "observe"), default="calibrate")
    campaign.add_argument("--target", action="append", choices=("vela", "plasma"))
    campaign.add_argument("--resume")
    campaign.add_argument("--yes", action="store_true")
    campaign.add_argument("--fast", action="store_true")
    campaign.add_argument("--seed", type=int, default=0)
    campaign.add_argument("--exercise", action="store_true")
    from .model import MEASUREMENTS
    campaign.add_argument("--phase", action="append", choices=MEASUREMENTS)
    args = parser.parse_args(argv)
    host = Host()
    try:
        if args.command == "_resource-workload":
            user = pwd.getpwnam(ACCOUNT)
            if os.getuid()!=user.pw_uid or os.geteuid()!=user.pw_uid or user.pw_uid==0:
                raise BenchError("Local workload requires reserved unprivileged UID")
            check_profile_environment(os.environ,user.pw_dir,f"/run/user/{user.pw_uid}")
            from .resources import hash_client
            print(json.dumps(hash_client()),flush=True)
            return 0
        if args.command == "_capture":
            user = pwd.getpwnam(ACCOUNT)
            if os.getuid() != user.pw_uid or os.geteuid() != user.pw_uid or user.pw_uid == 0:
                raise BenchError("Capture may only run as the reserved unprivileged account")
            check_profile_environment(os.environ, user.pw_dir, f"/run/user/{user.pw_uid}")
            capture_worker(args.target, args.primary, Layout(), count=1 if args.one else 12)
            return 0
        if args.command == "_session":
            print("vela-benchmark-session: " + json.dumps({"stage": "entry", "pid": os.getpid(),
                  "uid": os.getuid(), "euid": os.geteuid(), "target": args.target}), file=sys.stderr, flush=True)
            user = pwd.getpwnam(ACCOUNT)
            env = desktop_environment(args.target, user, os.environ)
            print("vela-benchmark-session: environment validated", file=sys.stderr, flush=True)
            descriptor = resolve_targets([args.target])[args.target]
            os.chdir(user.pw_dir)
            print("vela-benchmark-session: exec " + descriptor["executables"][0]["path"], file=sys.stderr, flush=True)
            os.execve(descriptor["executables"][0]["path"], descriptor["argv"], env)
        require_root()
        if args.command in ("campaign", "campaign-results"):
            from .campaign import execute, latest
            if args.command == "campaign-results":
                load_setup(host)
                print(json.dumps(latest(), allow_nan=False))
                return 0
            return execute(host, args.target, case=args.case, resume=args.resume, assume_yes=args.yes,
                           fast=args.fast, seed=args.seed, phases=args.phase, exercise=args.exercise)
        if args.command == "results":
            load_setup(host)
            directory = state_directory()
            print(json.dumps(pilot_results(directory, read_json(directory / "state.json")), allow_nan=False))
            return 0
        if args.command == "session-test":
            return begin(host, args.target, args.seconds, args.yes, presentation_probe=args.probe,
                         controlled=args.conditions, calibrate=args.calibrate)
        if args.command == "_pilot":
            return worker(host) or 0
        if args.command == "recover":
            require_workers_stopped(host)
            require_no_graphical_sessions(logind_sessions(host))
        recover(host)
        if args.command == "recover":
            state = read_json(state_directory() / "state.json")
            print(f"Restoration complete. Pilot result: {state.get('result')}; stage: {state.get('stage')}.")
        return 0
    except (BenchError, OSError, ValueError, KeyError) as exc:
        print(f"vela-benchmark-launcher: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
