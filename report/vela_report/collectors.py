# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import datetime as dt
from collections import deque
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import time

from .core import LOG_LIMIT, Result, TEXT_LIMIT, read_regular, run_command, timestamp

CATEGORIES = ["Crash", "Freeze", "Rendering", "Performance", "Input", "Installation/update", "Other"]
SPECS = [
    ("version", "Vela version", "Package versions and executable build IDs identify regressions and test builds.",
     "Executable paths and package information.", 10),
    ("system", "System and runtime versions", "Distribution, kernel and library versions help compare machines.",
     "System details; user and host names are redacted where recognized.", 10),
    ("graphics", "Graphics hardware and driver", "GPU model and driver information help isolate graphics compatibility.",
     "GPU model and PCI addresses; recognized serial fields are redacted.", 10),
    ("logs", "Selected Vela session log", "The log shows startup, errors and recovery in the session you select.",
     "Application names and free-form messages may contain personal data.", 0),
    ("config", "Current Vela configuration", "Settings and relevant overrides help reproduce behavior.",
     "Current settings may differ from the failed session; paths may be present.", 0),
    ("runtime", "Live Vela state and outputs", "Read-only state shows output scaling, frame counters and compositor state.",
     "Window titles are omitted; application identifiers and geometry are included.", 2),
    ("session_journal", "Session journal", "Selected Vela process messages help correlate application and service failures.",
     "Free-form messages may contain personal information.", 10),
    ("kernel_journal", "Kernel journal", "Kernel messages can expose GPU resets, OOM events and device failures.",
     "Device details and free-form system messages may contain personal information.", 10),
    ("resources", "Live resource recording", "Descriptor types, limits, memory and CPU trends reveal resource exhaustion.",
     "Selected process metrics only; no keystrokes, file targets or full command lines.", 60),
    ("crashes", "Crash metadata", "Crash times, signals and executable identities help locate failures without exporting memory.",
     "Executable paths and identifiers; core memory is excluded.", 10),
    ("brightness", "Display brightness", "Each display's bus and one read of its brightness show why a slider is missing or wrong.",
     "Connector and monitor names, I2C bus numbers and the bytes of brightness replies. Only reads; nothing is sent to change the displays.", 15),
    ("vendor", "Generate NVIDIA diagnostic report", "NVIDIA's tool provides driver-specific evidence for NVIDIA hardware.",
     "Sensitive, unredacted vendor archive; may need permissions. No automatic elevation.", 120),
]
PROGRAMS = {"vela-compositor", "vela-shell", "vela-files", "vela-settings", "vela-lock",
            "vela-polkit-agent", "vela-polkit-prompt", "Xwayland", "firefox"}
OVERRIDES = [
    "VELA_SCALE", "VELA_OUTPUT_SIZE", "VELA_VRR", "VELA_TEARING", "VELA_SCANOUT",
    "VELA_READY_WAIT", "VELA_LATCH", "VELA_LATCH_MARGIN", "VELA_REALTIME", "VELA_SCREEN_OFF",
    "VELA_LOCK_ON_IDLE", "VELA_STATS", "VELA_VULKAN_VALIDATION", "VELA_DEBUG",
    "VELA_DEBUG_SYNC", "VELA_DEBUG_LINEAR", "VELA_DEBUG_DAMAGE", "VELA_DEBUG_SCANOUT",
    "VELA_SYNC_FILE", "WLR_RENDER_NO_EXPLICIT_SYNC", "__NV_DISABLE_EXPLICIT_SYNC",
    "QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "QSG_RENDER_LOOP",
    "QT_WAYLAND_SHELL_INTEGRATION", "QT_WAYLAND_RECONNECT",
    "XKB_DEFAULT_LAYOUT", "XKB_DEFAULT_VARIANT", "XKB_DEFAULT_OPTIONS",
]


def recommended(category):
    result = {"version", "system", "logs", "config"}
    if category in ("Crash", "Freeze", "Rendering", "Performance"):
        result |= {"graphics", "runtime"}
    if category in ("Crash", "Freeze"):
        result |= {"session_journal", "kernel_journal", "crashes"}
    if category in ("Freeze", "Performance"):
        result.add("resources")
    if category == "Input":
        result |= {"session_journal", "kernel_journal"}
    return result


def brightness_helper():
    """vela-brightness next to the installed reporter (<libexec>/report/vela_report),
    or in the build tree (build/report/vela_report, build/brightness)."""
    base = Path(__file__).resolve().parents[2]
    for path in (base / "vela-brightness", base / "brightness" / "vela-brightness"):
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None


def process_identity(directory):
    fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
    executable = Path(os.readlink(directory / "exe")).name.removesuffix(" (deleted)")
    role = executable
    if executable == "vela-compositor":
        # Read only to distinguish the supervisor; never export the command line.
        if b"--supervise" in (directory / "cmdline").read_bytes().split(b"\0"):
            role = "vela-supervisor"
    return {"pid": int(directory.name), "start_ticks": int(fields[19]),
            "role": role, "executable": executable}


def discover(environment, state_dir=None):
    processes = []
    try:
        for path in environment.proc.iterdir():
            if not path.name.isdigit():
                continue
            try:
                if path.stat().st_uid != environment.uid:
                    continue
                identity = process_identity(path)
                if identity["executable"] in PROGRAMS:
                    processes.append(identity)
            except (OSError, ValueError, IndexError):
                continue
    except OSError:
        pass
    logs = []
    for name, label in (("vela.log", "Current/last retained session"), ("vela.log.old", "Previous retained session")):
        path = Path(state_dir or environment.state_dir()) / name
        try:
            info = path.lstat()
            if stat.S_ISREG(info.st_mode):
                logs.append({"path": str(path), "label": label, "bytes": info.st_size,
                             "modified": dt.datetime.fromtimestamp(info.st_mtime).astimezone().isoformat()})
        except OSError:
            pass
    sockets = []
    runtime = environment.env.get("XDG_RUNTIME_DIR")
    if runtime:
        for path in sorted(Path(runtime).glob("vela-*.sock")):
            try:
                if path.lstat().st_uid == environment.uid and stat.S_ISSOCK(path.lstat().st_mode):
                    sockets.append(str(path))
            except OSError:
                pass
    return {"processes": sorted(processes, key=lambda p: p["pid"]), "logs": logs, "sockets": sockets,
            "collections": [{"id": k, "title": title, "help": help_text, "privacy": privacy,
                             "seconds": seconds, "sensitive": k == "vendor"} for k, title, help_text, privacy, seconds in SPECS],
            "categories": CATEGORIES, "default_state_dir": str(environment.state_dir()),
            "recommendations": {category: sorted(recommended(category)) for category in CATEGORIES},
            "vendor_available": bool(shutil.which("nvidia-bug-report.sh"))}


def query(path, uid, line):
    """One question on the selected command socket, its JSON answer."""
    info = Path(path).lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != uid:
        raise ValueError("Select a command socket owned by the reporting user.")
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(2)
        connection.connect(str(path))
        connection.sendall(line.encode() + b"\n")
        data = bytearray()
        while b"\n" not in data:
            chunk = connection.recv(min(65536, TEXT_LIMIT + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > TEXT_LIMIT:
                raise ValueError("Runtime state exceeds the size limit.")
    return json.loads(data.split(b"\n", 1)[0])


def query_state(path, uid):
    state = query(path, uid, "state")
    # Never export window titles, including when the socket supplies unexpected fields.
    fields = {"locked", "workspace", "workspaces", "held"}
    result = {key: value for key, value in state.items() if key in fields}
    result["outputs"] = [{key: value for key, value in output.items()
                          if key in {"name", "x", "y", "w", "h", "scale", "enabled", "powered", "frames", "missed"}}
                         for output in state.get("outputs", [])]
    result["windows"] = [{key: value for key, value in window.items()
                           if key in {"app", "x", "y", "w", "h", "output", "workspace", "sticky",
                                      "minimized", "snap", "snapGroup", "mapped", "fullscreen", "maximized"}}
                          for window in state.get("windows", [])]
    return result


def query_brightness(path, uid):
    state = query(path, uid, "brightness")
    fields = {"name", "description", "mechanism", "state", "value", "pending", "refused", "max", "seq"}
    return {"outputs": [{key: value for key, value in output.items() if key in fields}
                        for output in state.get("outputs", [])]}


def resource_sample(environment, identity):
    directory = environment.proc / str(identity["pid"])
    sample = dict(identity, captured_at=timestamp(), monotonic_seconds=time.monotonic())
    try:
        if directory.stat().st_uid != environment.uid:
            return dict(sample, status="permission_denied")
        current = process_identity(directory)
        if current["start_ticks"] != identity["start_ticks"] or current["executable"] != identity["executable"]:
            return dict(sample, status="pid_reused")
        fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
        sample.update(cpu_user_ticks=int(fields[11]), cpu_system_ticks=int(fields[12]),
                      clock_ticks_per_second=os.sysconf("SC_CLK_TCK"))
        metrics = {}
        for line in (directory / "status").read_text().splitlines():
            key, _, value = line.partition(":")
            if key in {"VmRSS", "RssAnon", "RssFile", "RssShmem", "VmSize", "Threads"}:
                metrics[key] = value.strip()
        sample["memory"] = metrics
        for line in (directory / "limits").read_text().splitlines():
            if line.startswith("Max open files"):
                columns = line.split()
                sample["nofile"] = {"soft": columns[3], "hard": columns[4]}
        descriptors = list((directory / "fd").iterdir())
        sample["fd_count"] = len(descriptors)
        sample["highest_fd"] = max((int(p.name) for p in descriptors), default=-1)
        categories = {}
        for descriptor in descriptors:
            try:
                target = os.readlink(descriptor)
            except OSError:
                category = "unavailable"
            else:
                category = "other_file"
                if target.startswith("anon_inode:"):
                    category = "other_anon_inode"
                    for marker, label in (("sync_file", "sync_file"), ("syncobj", "syncobj"),
                                          ("dmabuf", "dmabuf"), ("dma_buf", "dmabuf"),
                                          ("eventfd", "eventfd"), ("eventpoll", "eventpoll"),
                                          ("timerfd", "timerfd")):
                        if marker in target:
                            category = label
                            break
                elif target.startswith("socket:"):
                    category = "socket"
                elif target.startswith("pipe:"):
                    category = "pipe"
                elif target in {"/dmabuf", "/dmabuf (deleted)"}:
                    category = "dmabuf"
                elif target.endswith(" (deleted)"):
                    category = "unlinked_file"
            categories[category] = categories.get(category, 0) + 1
        sample["fd_types"] = categories
        # Do not return metrics taken across a PID replacement.
        if process_identity(directory)["start_ticks"] != identity["start_ticks"]:
            return dict(identity, captured_at=timestamp(), status="pid_reused")
        sample["status"] = "collected"
    except FileNotFoundError:
        sample["status"] = "exited"
    except PermissionError:
        sample["status"] = "permission_denied"
    except (OSError, IndexError, ValueError):
        sample["status"] = "unavailable"
    return sample


class IncidentBootUnavailable(ValueError):
    pass


class Collector:
    def __init__(self, report, runner=run_command):
        self.report = report
        self.environment = report.environment
        self.run = runner
        self._boot_listing = None

    def boot_listing(self):
        if self._boot_listing is None:
            self._boot_listing = self.run(["journalctl", "--list-boots", "--no-pager", "--no-legend", "--utc"], timeout=10)
        return self._boot_listing

    def command_file(self, collection, name, args, timeout=10):
        result = self.run(args, timeout=timeout)
        if result.output:
            self.report.add_text(collection, name, result.output, source="command: " + " ".join(args),
                                 metadata={"captured_at": timestamp(), "timeout_seconds": timeout,
                                           "byte_limit": TEXT_LIMIT, "returncode": result.returncode,
                                           "status": result.status, "reason": result.reason})
        return result

    def file(self, collection, path, name):
        try:
            data, metadata = read_regular(path)
            self.report.add_text(collection, name, data, str(path), metadata)
            if metadata["truncated"] or metadata["changed_during_copy"]:
                return Result("partial", reason="Source was truncated or changed during copying.")
            return Result("collected")
        except FileNotFoundError:
            return Result("unavailable", reason="Source is not available.")
        except PermissionError:
            return Result("permission_denied", reason="Cannot read the selected source.")
        except (OSError, ValueError):
            return Result("failed", reason="Source is not a readable regular file; symlinks are not followed.")

    def version(self):
        results = []
        packages = ["vela-git", "wlroots0.20", "qt6-base", "qt6-wayland", "wayland", "mesa",
                    "libdrm", "vulkan-icd-loader", "egl-wayland", "layer-shell-qt"]
        if shutil.which("pacman"):
            args = ["pacman", "-Q", *packages]
        elif shutil.which("dpkg-query"):
            args = ["dpkg-query", "-W", "vela", "libwlroots*", "libqt6core*", "libwayland*", "libdrm*"]
        elif shutil.which("rpm"):
            args = ["rpm", "-q", "vela", "wlroots", "qt6-qtbase", "wayland", "mesa-libEGL", "libdrm"]
        else:
            args = None
        if args:
            results.append(self.command_file("version", "system/packages.txt", args))
        else:
            results.append(Result("unavailable", reason="No supported package database found."))
        for program in ("vela-compositor", "vela-shell"):
            executable = shutil.which(program)
            if executable:
                results.append(self.command_file("version", f"system/{program}-build-id.txt",
                                                 ["readelf", "-n", executable]))
        self.report.add_json("version", "system/reporter-version.json",
                             {"reporter_build": self.report.build, "full_vela_commit": "unknown",
                              "note": "Reporter build and installed compositor identity are separate evidence."})
        return combine(results, "Some package or executable identities are unavailable.")

    def system(self):
        uname = os.uname()
        self.report.add_json("system", "system/runtime.json",
                             {"kernel": uname.release, "architecture": uname.machine,
                              "reporting_desktop": self.environment.env.get("XDG_CURRENT_DESKTOP", "unknown"),
                              "reporting_session_type": self.environment.env.get("XDG_SESSION_TYPE", "unknown"),
                              "python": os.sys.version.split()[0], "captured_at": timestamp()})
        # os-release is commonly a symlink to /usr/lib/os-release. Only this
        # fixed system source is resolved; selected files never follow symlinks.
        source = (self.environment.etc / "os-release").resolve()
        result = self.file("system", source, "system/os-release.txt")
        return result if result.status == "collected" else Result("partial", reason=result.reason)

    def graphics(self):
        pci = self.run(["lspci", "-nnk"], timeout=10)
        blocks = re.split(r"(?m)(?=^\S)", pci.output)
        graphics = "".join(block for block in blocks if re.search(
            r"VGA compatible controller|3D controller|Display controller|\[(?:0300|0302|0380)\]", block))
        if graphics:
            self.report.add_text("graphics", "system/pci-graphics.txt", graphics,
                                 source="graphics-only blocks from lspci -nnk",
                                 metadata={"captured_at": timestamp(), "returncode": pci.returncode})
        results = [pci if pci.status != "collected" or graphics else
                   Result("unavailable", reason="No PCI graphics device was reported.")]
        if shutil.which("nvidia-smi"):
            results.append(self.command_file("graphics", "system/nvidia-smi.txt",
                           ["nvidia-smi", "--query-gpu=name,driver_version,pci.bus_id,memory.total,"
                            "memory.used,temperature.gpu", "--format=csv"]))
        driver = self.environment.proc / "driver/nvidia/version"
        if driver.exists():
            results.append(self.file("graphics", driver, "system/nvidia-driver.txt"))
        return combine(results, "Some graphics tools or driver information are unavailable.")

    def logs(self, expected=None):
        path = self.report.context.get("log_path")
        if not path:
            return Result("unavailable", reason="No incident log was selected.")
        data, metadata = read_regular(Path(path), LOG_LIMIT, bookends=True)
        if expected is not None and (metadata["device"], metadata["inode"]) != expected:
            return Result("partial", reason="Selected log rotated during collection; original snapshot retained.")
        self.report.add_text("logs", "logs/vela.log", data, path, metadata)
        session = {"last_log_write": dt.datetime.fromtimestamp(
            metadata["mtime_ns"] / 1e9, dt.timezone.utc).isoformat()}
        header = re.search(r"^vela-session: started_at=(\S+) boot_id=([0-9a-fA-F-]{36})$", data, re.M)
        if header:
            # Log contents are untrusted: accept only a valid timestamp and UUID.
            try:
                started = dt.datetime.fromisoformat(header[1])
                if started.tzinfo is not None and re.fullmatch(
                        r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", header[2]):
                    session.update(started_at=started.isoformat(), boot_id=header[2].replace("-", ""))
            except ValueError:
                pass
        self.report.context["log_session"] = session
        for derived in ("logs/error-summary.json", "logs/resource-history.jsonl"):
            if derived in self.report.artifacts:
                self.report.remove(derived)
        count, first, last = 0, None, None
        shell_restarts, layer_errors = 0, 0
        last_shell_restart, last_layer_error = None, None
        samples = deque(maxlen=10000)
        sample_count = 0
        keys = {"pid", "elapsed_s", "fds", "sync_file", "dmabuf", "eventfd", "sockets", "nofile", "rss_kib"}
        for line in io.StringIO(data):
            if re.search(r'\bShell "[^"\n]+" exited \((?:code [0-9]+|signal [^)]*)\): restarting it', line):
                shell_restarts += 1
                last_shell_restart = line.strip()[:4096]
            if "layer_surface has never been configured" in line:
                layer_errors += 1
                last_layer_error = line.strip()[:4096]
            if "Too many open files" in line:
                count += 1
                last = line.strip()[:4096]
                if first is None:
                    first = last
            # wlroots writes log prefixes separately: a supervisor record
            # can land between a prefix and its message during an error flood.
            _, separator, record = line.partition("vela-supervise: resources ")
            if separator:
                values = {key: int(value) for key, value in re.findall(r"([a-z_]+)=([0-9]{1,20})\b", record)
                          if key in keys}
                if {"pid", "fds", "nofile", "sync_file"} <= values.keys():
                    samples.append(values)
                    sample_count += 1
        self.report.observations = [o for o in self.report.observations if not o.startswith("Session log:")]
        if count or shell_restarts or layer_errors:
            self.report.add_json("logs", "logs/error-summary.json",
                                 {"file_descriptor_exhaustion_messages": count,
                                  "first": first, "last": last,
                                  "shell_restarts": shell_restarts,
                                  "last_shell_restart": last_shell_restart,
                                  "layer_surface_unconfigured_errors": layer_errors,
                                  "last_layer_surface_error": last_layer_error,
                                  "scope": "retained log bytes; these messages do not establish the underlying cause"})
        if count:
            self.report.observations.append("Session log: file descriptor exhaustion (Too many open files); "
                                            "see logs/error-summary.json and retained resource samples.")
        if shell_restarts:
            self.report.observations.append(f"Session log: shell restarted {shell_restarts} times after an error; "
                                            "this can reset the wallpaper and desktop panels.")
        if layer_errors:
            self.report.observations.append(f"Session log: {layer_errors} layer-shell buffer commits before configure; "
                                            "Wayland disconnects the offending client.")
        if samples:
            self.report.add_text("logs", "logs/resource-history.jsonl",
                                 "".join(json.dumps(s) + "\n" for s in samples),
                                 source="supervisor samples from the selected incident log",
                                 metadata={"scope": "historical compositor samples, not the reporting desktop",
                                           "sample_count": sample_count, "retained_samples": len(samples)})
        reasons = []
        if metadata["truncated"]:
            reasons.append(f"Log exceeds {LOG_LIMIT / (1024 * 1024):g} MiB; startup and latest messages retained, with an explicit omission marker.")
        if metadata["changed_during_copy"]:
            reasons.append("Log changed during copying.")
        return Result("partial" if reasons else "collected", reason=" ".join(reasons))

    def config(self):
        results = [self.file("config", self.environment.config_dir() / name, "config/" + name)
                   for name in ("vela.conf", "outputs.conf", "schermi.conf", "desktop.conf")]
        overrides = {key: self.environment.env[key] for key in OVERRIDES if key in self.environment.env}
        self.report.add_json("config", "config/current-overrides.json",
                             {"scope": "reporting environment, not historical incident configuration",
                              "overrides": overrides})
        return combine(results, "Some optional configuration files are absent.")

    def runtime(self):
        path = self.report.context.get("socket")
        if not path:
            return Result("unavailable", reason="No live Vela command socket was selected.")
        state = query_state(path, self.environment.uid)
        self.report.add_json("runtime", "session/runtime-state.json",
                             {"scope": "live selected Vela instance at collection time",
                              "socket": path, "captured_at": timestamp(), "state": state})
        return Result("collected")

    def brightness(self):
        # --probe only reads, and takes the same lock as the compositor's
        # helpers: safe while a session is driving the same displays.
        helper = brightness_helper()
        if helper:
            results = [self.command_file("brightness", "system/brightness-probe.txt",
                                         [str(helper), "--probe"], timeout=30)]
        else:
            results = [Result("unavailable", reason="vela-brightness is not installed.")]
        path = self.report.context.get("socket")
        if path:
            try:
                state = query_brightness(path, self.environment.uid)
                self.report.add_json("brightness", "session/brightness-state.json",
                                     {"scope": "live selected Vela instance at collection time",
                                      "socket": path, "captured_at": timestamp(), "state": state})
                results.append(Result("collected"))
            except (OSError, ValueError) as error:
                results.append(Result("failed", reason=self.report.redactor.text(str(error))))
        return combine(results, "The displays' probe or the compositor's brightness state is unavailable.")

    def journal_args(self):
        context = self.report.context
        boot = str(context.get("boot", "auto"))
        if not re.fullmatch(r"(?:auto|0|-[1-9][0-9]*|[0-9a-fA-F]{32})", boot):
            raise ValueError("Boot must be auto, 0, a negative index, or a 32-character boot ID.")
        incident = context.get("incident_time", "")
        session = context.get("log_session", {})
        source = "explicit"
        if incident:
            when = dt.datetime.fromisoformat(incident)
            if when.tzinfo is None:
                raise ValueError("Incident time must include a timezone offset.")
            time_source = "incident_time"
        elif session.get("last_log_write"):
            when = dt.datetime.fromisoformat(session["last_log_write"])
            time_source = "log_last_write"
        else:
            when = None
            time_source = "whole_boot"
        if boot == "auto":
            if session.get("boot_id"):
                boot = session["boot_id"]
                source = "session_log_header"
            elif when is not None:
                listing = self.boot_listing()
                if listing.output:
                    self.report.add_text("kernel_journal" if self.report.plan.get("kernel_journal") else "session_journal",
                                         "logs/journal-boots.txt", listing.output,
                                         source="journalctl --list-boots --utc")
                boots = []
                for line in listing.output.splitlines():
                    match = re.match(r"\s*(-?\d+)\s+([0-9a-fA-F]{32})\b", line)
                    dates = re.findall(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", line)
                    if match and len(dates) == 2:
                        start, end = [dt.datetime.fromisoformat(d).replace(tzinfo=dt.timezone.utc) for d in dates]
                        boots.append((start, end, match[2]))
                boots.sort()
                for index, (start, end, identifier) in enumerate(boots):
                    upper = boots[index + 1][0] if index + 1 < len(boots) else dt.datetime.now(dt.timezone.utc)
                    if start <= when < upper:
                        boot, source = identifier, time_source
                        break
                if boot == "auto":
                    raise IncidentBootUnavailable("Cannot match the incident to a retained journal boot; current-boot logs were not substituted.")
            elif str(context.get("live_during_incident", "")).lower() == "yes":
                boot, source = "0", "live_session"
            else:
                raise IncidentBootUnavailable("Incident boot is unknown. Select its boot; current-boot logs were not substituted.")
        if session.get("boot_id") and source == "explicit":
            selected_id = self.boot_id().replace("-", "") if boot == "0" else boot
            if selected_id.startswith("-"):
                listing = self.boot_listing()
                selected_id = next((match[2] for line in listing.output.splitlines()
                                    if (match := re.match(r"\s*(-?\d+)\s+([0-9a-fA-F]{32})\b", line))
                                    and match[1] == boot), "")
                if not selected_id:
                    raise ValueError("Selected boot index cannot be resolved against the incident log.")
                boot = selected_id
            if not re.fullmatch(r"[0-9a-fA-F]{32}", selected_id):
                raise IncidentBootUnavailable("Selected journal boot cannot be verified against the incident log.")
            if selected_id.lower() != session["boot_id"].lower():
                raise ValueError("Selected journal boot conflicts with the selected Vela session log.")
        before = int(context.get("minutes_before", 10))
        after = int(context.get("minutes_after", 5))
        if not (0 <= before <= 120 and 0 <= after <= 120):
            raise ValueError("Journal windows must be between 0 and 120 minutes.")
        context["journal_scope"] = {"boot": boot, "boot_source": source, "time_source": time_source,
                                    "journal_order": "newest_first"}
        args = ["--boot", boot]
        if when is not None:
            start = when - dt.timedelta(minutes=before)
            stop = min(when + dt.timedelta(minutes=after), dt.datetime.now().astimezone())
            args += ["--since", start.isoformat(), "--until", stop.isoformat()]
        return args

    def session_journal(self):
        return self.command_file("session_journal", "logs/session-journal.txt",
                    ["journalctl", "--user", "--no-pager", "--reverse", "--output=short-iso-precise",
                     *self.journal_args(),
                     *["_COMM=" + p for p in ("vela-compositor", "vela-shell", "vela-polkit-age",
                                               "vela-polkit-pro", "vela-files", "vela-settings", "vela-lock")]])

    def kernel_journal(self):
        return self.command_file("kernel_journal", "logs/kernel-journal.txt",
                                 ["journalctl", "-k", "--no-pager", "--reverse", "--output=short-iso-precise",
                                  *self.journal_args()])

    def crashes(self):
        args = self.journal_args()
        # coredumpctl has no --boot option: restrict matches by the selected boot when resolvable.
        boot = args[1]
        try:
            if boot == "0":
                boot = (self.environment.proc / "sys/kernel/random/boot_id").read_text().strip().replace("-", "")
        except OSError:
            boot = ""
        if re.fullmatch(r"-[1-9][0-9]*", boot or ""):
            index = boot
            listing = self.boot_listing()
            boot = next((match.group(2) for line in listing.output.splitlines()
                         if (match := re.match(r"\s*(-?\d+)\s+([0-9a-fA-F]{32})\b", line))
                         and match.group(1) == index), "")
        if not re.fullmatch(r"[0-9a-fA-F]{32}", boot or ""):
            return Result("unavailable", reason="The selected boot could not be resolved. Select its explicit boot ID.")
        result = self.command_file("crashes", "crashes/metadata.txt",
                    ["coredumpctl", "--no-pager", *args[2:], "list", "_BOOT_ID=" + boot,
                     "COREDUMP_UID=" + str(self.environment.uid),
                     *["COREDUMP_COMM=" + p for p in ("vela-compositor", "vela-shell", "vela-files",
                                                       "vela-settings", "vela-lock")]])
        if result.returncode == 1 and result.output.strip() == "No coredumps found.":
            return Result("collected", result.output, "No coredumps in the selected incident boot/window.", 1)
        return result

    def resources(self, progress):
        identities = self.report.context.get("processes", [])
        if not identities:
            return Result("unavailable", reason="No live processes were selected.")
        duration = int(self.report.context.get("duration", 60))
        if not 1 <= duration <= 300:
            raise ValueError("Recording duration must be between 1 and 300 seconds.")
        samples, interrupted, truncated, size = [], False, False, 0
        lines = []
        started = time.monotonic()
        try:
            for index in range(duration + 1):
                progress(f"Recording resources: {max(0, duration - index)} seconds remaining.")
                for identity in identities:
                    sample = resource_sample(self.environment, identity)
                    line = (json.dumps(self.report.redactor.value(sample)) + "\n").encode()
                    if size + len(line) > TEXT_LIMIT:
                        truncated = True
                        break
                    samples.append(sample)
                    lines.append(line)
                    size += len(line)
                if truncated:
                    break
                if index < duration:
                    time.sleep(max(0, started + index + 1 - time.monotonic()))
        except KeyboardInterrupt:
            interrupted = True
        self.report.add_bytes("resources", "samples/resources.jsonl", b"".join(lines), "text", False,
                              source="selected processes in /proc",
                              metadata={"duration_requested": duration, "duration_actual": time.monotonic() - started,
                                        "interval_seconds": 1, "stopped_early": interrupted, "truncated": truncated})
        for identity in identities:
            matches = [s for s in samples if s["pid"] == identity["pid"] and s["status"] == "collected"]
            if len(matches) >= 2:
                self.report.observations.append(
                    f"{identity['role']} (PID {identity['pid']}): descriptors changed from "
                    f"{matches[0]['fd_count']} to {matches[-1]['fd_count']}; this does not identify the cause.")
        missing = any(s["status"] != "collected" for s in samples)
        return Result("partial" if interrupted or missing or truncated else "collected",
                      reason="Recording stopped early, a target became unavailable, or output was capped."
                      if interrupted or missing or truncated else "")

    def vendor(self):
        vendor_dir = self.report.directory / "vendor-work"
        vendor_dir.mkdir(mode=0o700)
        result = self.run(["nvidia-bug-report.sh"], timeout=120, cwd=vendor_dir)
        if result.output:
            self.report.add_text("vendor", "vendor/tool-output.txt", result.output)
        attached = False
        for filename in ("nvidia-bug-report.log.gz", "nvidia-bug-report.log", "nvidia-bug-report.log.tgz"):
            output = vendor_dir / filename
            if not output.exists():
                continue
            name = self.report.attach(output, "vendor")
            item = self.report.artifacts.pop(name)
            destination = "vendor/" + filename
            (self.report.directory / "vendor").mkdir(exist_ok=True, mode=0o700)
            (self.report.directory / name).rename(self.report.directory / destination)
            item.update(collection="vendor", source="generated NVIDIA report")
            self.report.artifacts[destination] = item
            if not any(a["collection"] == "attachments" for a in self.report.artifacts.values()):
                self.report.results.pop("attachments", None)
                self.report.plan.pop("attachments", None)
            attached = True
        if result.status == "collected" and not attached:
            return Result("failed", reason="The NVIDIA tool produced no report file.")
        return result

    def collect(self, selected, context, description, progress=lambda _: None):
        known = {s[0] for s in SPECS}
        if set(selected) - known:
            raise ValueError("Unknown collection.")
        self.report.context = context
        self.report.description = description
        self.report.plan = {key: key in selected for key in known}
        # Snapshot retained logs before potentially slow probes or live sampling.
        order = ["logs", "resources", *[s[0] for s in SPECS if s[0] not in {"logs", "resources"}]]
        for key in order:
            if key not in selected:
                self.report.results[key] = {"status": "excluded", "reason": "Not selected."}
                continue
            progress(f"Collecting {next(s[1] for s in SPECS if s[0] == key)}…")
            started = timestamp()
            try:
                result = self.resources(progress) if key == "resources" else getattr(self, key)()
            except KeyboardInterrupt:
                result = Result("partial", reason="Collection stopped by the user.")
            except PermissionError:
                result = Result("permission_denied", reason="This collection is not accessible to the current user.")
            except FileNotFoundError:
                result = Result("unavailable", reason="This source is unavailable.")
            except IncidentBootUnavailable as error:
                result = Result("unavailable", reason=str(error))
            except (OSError, ValueError, KeyError, TypeError) as error:
                result = Result("failed", reason=self.report.redactor.text(str(error)))
            self.report.results[key] = {"status": result.status, "reason": result.reason,
                                        "started_at": started, "finished_at": timestamp()}
            if key == "resources" and "logs" in selected and self.report.artifacts.get("logs/vela.log"):
                # Include the reproduction itself, not only the log before sampling.
                original = self.report.artifacts["logs/vela.log"]["metadata"]
                try:
                    refreshed = self.logs(expected=(original["device"], original["inode"]))
                    self.report.results["logs"].update(status=refreshed.status, reason=refreshed.reason,
                                                       finished_at=timestamp())
                except (OSError, ValueError):
                    self.report.results["logs"].update(status="partial", reason="Selected log became unavailable; original snapshot retained.")
        self.report.context["reporting_boot_id"] = self.boot_id()
        self.report.context["reporting_session_id"] = self.environment.env.get("XDG_SESSION_ID", "unknown")
        return self.report.summary()

    def boot_id(self):
        try:
            return (self.environment.proc / "sys/kernel/random/boot_id").read_text().strip()
        except OSError:
            return "unknown"


def combine(results, reason):
    if all(r.status == "collected" for r in results):
        return Result("collected")
    if any(r.output or r.status == "collected" for r in results):
        return Result("partial", reason=reason)
    return results[0] if results else Result("unavailable", reason=reason)
