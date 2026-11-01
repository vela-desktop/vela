# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed privileged host operations for the P0 session pilot.

No sudoers/polkit rules or command RPC are installed. Every mutating entry
point requires an explicit root invocation. Workloads always use another UID.
"""

from dataclasses import dataclass
import fcntl
import grp
import hashlib
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import subprocess
import tempfile
import time
import uuid

from . import BenchError
from .archive import atomic_json, read_json
from .discovery import file_hash

ACCOUNT = "vela-benchmark"
PILOT_UNIT = "vela-benchmark-pilot.service"
FAULT_UNIT = "vela-benchmark-qualification.service"
SESSION_UNIT = "vela-benchmark-session.service"
PILOT_SECONDS = 600
RECOVERY_SECONDS = 300
SAFE_ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C", "LANG": "C"}
BASE_MODULES = ("__init__.py", "archive.py", "model.py", "discovery.py", "host.py", "pilot.py")
MODULES = BASE_MODULES + ("diagnostics.py", "conditions.py", "calibration.py", "qualification.py", "campaign.py", "measurements.py", "desktop.py", "workloads.py", "platform_workloads.py", "pixels.py", "resources.py")


@dataclass(frozen=True)
class Layout:
    root: Path = Path("/")

    def path(self, relative):
        return self.root / relative

    @property
    def base(self):
        return self.path("var/lib/vela-benchmark")

    @property
    def home(self):
        return self.base / "home"

    @property
    def helper(self):
        return self.path("usr/local/libexec/vela-benchmark/launcher.py")

    @property
    def record(self):
        return self.base / "setup.json"

    @property
    def probe(self):
        return self.helper.parent / "vela-bench-probe"


class HostOperationError(BenchError):
    """Retain bounded stdout as well as stderr; many DDC errors use stdout."""

    def __init__(self, argv, returncode=None, stdout="", stderr="", *, timeout=None):
        def bounded(value):
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            return (value or "")[-8192:]
        self.operation = {"argv": list(argv), "returncode": returncode, "timeout_seconds": timeout,
                          "stdout": bounded(stdout), "stderr": bounded(stderr)}
        status = f"timeout after {timeout}s" if timeout is not None else f"exit {returncode}"
        details = "\n".join(f"{name}: {self.operation[name].strip()}" for name in ("stdout", "stderr") if self.operation[name].strip())
        super().__init__(f"Host operation failed ({status}): {' '.join(argv[:3])}: " + (details or "no diagnostic output"))


class Host:
    """Bounded shell-free operations; replace this interface in CPU tests."""

    def run(self, argv, *, check=True, timeout=20):
        try:
            result = subprocess.run(argv, env=SAFE_ENV, stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as exc:
            raise HostOperationError(argv, stdout=exc.stdout, stderr=exc.stderr, timeout=timeout) from exc
        except OSError as exc:
            raise HostOperationError(argv, stderr=str(exc)) from exc
        if check and result.returncode:
            raise HostOperationError(argv, result.returncode, result.stdout, result.stderr)
        return result

    def spawn(self, argv, stderr):
        return subprocess.Popen(argv, env=SAFE_ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=stderr, bufsize=0)

    def user(self):
        try:
            return pwd.getpwnam(ACCOUNT)
        except KeyError:
            return None

    def group(self):
        try:
            return grp.getgrnam(ACCOUNT)
        except KeyError:
            return None

    def groups(self, user):
        return os.getgrouplist(user.pw_name, user.pw_gid)

    def locked(self):
        # Python's spwd was removed in 3.13. Never return shadow contents.
        for line in Path("/etc/shadow").read_text().splitlines():
            fields = line.split(":")
            if fields[0] == ACCOUNT:
                return fields[1].startswith(("!", "*"))
        return False

    def pids(self, uid):
        found = []
        for directory in Path("/proc").glob("[0-9]*"):
            try:
                for line in (directory / "status").read_text().splitlines():
                    if line.startswith("Uid:") and uid in map(int, line.split()[1:]):
                        found.append(int(directory.name))
            except FileNotFoundError:
                pass
        return found


def require_root():
    if os.geteuid() != 0:
        raise BenchError("This operation requires an explicit root invocation; preview with --dry-run first")


def trusted(path, *, directory=False):
    """Installed code/state and every ancestor must resist unprivileged edits."""
    path = Path(path)
    for item in (path, *path.parents):
        info = item.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise BenchError(f"Untrusted privileged path: {item}")
    info = path.stat()
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise BenchError(f"Unexpected privileged path type: {path}")


def unit_quote(value):
    value = str(value)
    if any(ord(char) < 32 for char in value):
        raise BenchError("Control characters are not allowed in systemd values")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'


def pilot_unit(layout, *, fault=False):
    return ("[Unit]\nDescription=Vela benchmark P0 session pilot\nAfter=systemd-logind.service\n"
            "[Service]\nType=exec\nExecStart=/usr/bin/python3 -I " + unit_quote(layout.helper) + " _pilot\n"
            "ExecStopPost=/usr/bin/python3 -I " + unit_quote(layout.helper) + " _recover\n"
            f"RuntimeMaxSec={90 if fault else PILOT_SECONDS}\nTimeoutStopSec={RECOVERY_SECONDS}\nKillMode=control-group\nRestart=no\nUMask=0077\n")


def require_workers_stopped(host):
    for unit in (PILOT_UNIT, FAULT_UNIT):
        active = host.run(["systemctl", "show", unit, "--property=ActiveState", "--value"], check=False).stdout.strip()
        if active not in ("inactive", "failed"):
            raise BenchError("Stop and restore the active pilot/qualification worker first")


def isolation_properties(uid, layout, private_homes=()):
    """The same filesystem view for session children and user-manager services."""
    base = layout.base
    paths = ["/home", "/root"]
    for value in private_homes:
        path = Path(value)
        protected = (base, layout.helper, Path("/usr/bin"), Path("/etc"), Path("/run"), Path("/tmp"), Path("/var/tmp"))
        if not path.is_absolute() or any(path == item or path in item.parents for item in protected):
            raise BenchError(f"Cannot isolate this nonstandard home layout: {path}")
        if str(path) not in paths and not str(path).startswith(("/home/", "/root/")):
            paths.append(str(path))
    return {"ProtectHome": "tmpfs", "BindPaths": f"/run/user/{uid} {unit_quote(base / 'tmp')}:/tmp {unit_quote(base / 'var-tmp')}:/var/tmp",
            "InaccessiblePaths": " ".join("-" + unit_quote(path) for path in paths),
            "NoNewPrivileges": "yes"}


def pam_policy(pam_dir=Path("/etc/pam.d")):
    # Start with the installed Arch-style login stack. Other distributions
    # fail explicitly until their session policy is reviewed and tested.
    stack = Path(pam_dir) / "system-login"
    if not stack.is_file() or "pam_systemd.so" not in stack.read_text():
        raise BenchError("P0 currently requires a system-login PAM stack containing pam_systemd.so")
    return ("#%PAM-1.0\n# Root/systemd opens sessions; this service never authenticates a password.\n"
            "auth requisite pam_deny.so\naccount include system-login\npassword requisite pam_deny.so\nsession include system-login\n")


def setup_preview(layout=Layout()):
    return {"account": ACCOUNT, "home": str(layout.home), "password": "locked", "supplementary_groups": [],
            "launcher": str(layout.helper), "pam_service": "vela-benchmark", "linger": False,
            "persistent_resources": [str(layout.base), str(layout.helper.parent), "/etc/pam.d/vela-benchmark",
                                     "/etc/systemd/system/vela-benchmark-pilot.service",
                                     "/etc/systemd/system/user@UID.service.d/70-vela-benchmark.conf"],
            "effects": ["Create one normal local account once", "Install or refresh owned fixed-operation launcher code",
                        "Apply home/runtime isolation to this UID's user manager only"],
            "does_not": ["start a desktop", "change the display manager", "install packages", "grant sudo access"],
            "qualification": "P0 prototype; not a supported performance adapter"}


def personal_homes():
    limits = {"UID_MIN": 1000, "UID_MAX": 60000}
    for line in Path("/etc/login.defs").read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in limits:
            limits[fields[0]] = int(fields[1])
    return sorted({x.pw_dir for x in pwd.getpwall() if x.pw_name != ACCOUNT
                   and (x.pw_uid == 0 or limits["UID_MIN"] <= x.pw_uid <= limits["UID_MAX"])})


def check_account(record, host, layout=Layout()):
    user = host.user()
    group = host.group()
    if (not user or not group or user.pw_uid == 0 or user.pw_uid != record.get("uid")
            or user.pw_gid != record.get("gid") or group.gr_gid != record["gid"]
            or user.pw_dir != str(layout.home) or user.pw_shell != "/bin/sh"
            or user.pw_gecos != record.get("account_tag") or set(host.groups(user)) != {user.pw_gid}
            or not host.locked()):
        raise BenchError("Reserved account identity, groups or password lock changed; refusing UID operations")
    return user


def load_setup(host, layout=Layout()):
    trusted(layout.record)
    record = read_json(layout.record)
    if not isinstance(record, dict) or record.get("schema") != 1 or record.get("status") != "ready":
        raise BenchError("Incomplete/unsupported benchmark setup; rerun setup to inspect it")
    check_account(record, host, layout)
    for path, digest in record["files"].items():
        trusted(path)
        if file_hash(path) != digest:
            raise BenchError(f"Installed benchmark resource changed: {path}")
    return record


def pending_account_creation(record, host, layout):
    """Only retry the known empty bootstrap, never adopt a partial account."""
    expected = {"schema", "status", "account_tag", "account", "uid", "gid", "files", "home", "private_homes"}
    if (not isinstance(record, dict) or set(record) != expected or type(record.get("schema")) is not int
            or record["schema"] != 1 or record.get("status") != "provisioning" or record.get("account") != ACCOUNT
            or record.get("uid") is not None or record.get("gid") is not None or record.get("files") != {}
            or record.get("home") != str(layout.home) or not isinstance(record.get("account_tag"), str)
            or not re.fullmatch(r"Vela benchmark [0-9a-f]{32}", record["account_tag"])
            or not isinstance(record.get("private_homes"), list)
            or any(not isinstance(path, str) for path in record["private_homes"])
            or host.user() is not None or host.group() is not None):
        return False
    trusted(layout.base, directory=True)
    names = {path.name for path in layout.base.iterdir()}
    if names - {"setup.json", "mail"}:
        return False
    mail = layout.base / "mail"
    if mail.exists() or mail.is_symlink():
        if mail.is_symlink() or not mail.is_dir():
            return False
        trusted(mail, directory=True)
        if any(mail.iterdir()):
            return False
    return True


def useradd_command(layout, account_tag):
    # --key overrides login.defs, not /etc/default/useradd. CREATE_MAIL_SPOOL
    # is not a login.defs key. Keep any optional mailbox in the owned area
    # using the documented MAIL_DIR key, without changing host defaults.
    return ["useradd", "--user-group", "--no-create-home", "--no-log-init", "--home-dir", str(layout.home),
            "--shell", "/bin/sh", "--comment", account_tag, "--key", f"MAIL_DIR={layout.base / 'mail'}", ACCOUNT]


def install(host, layout=Layout(), source=None, pam_dir=Path("/etc/pam.d"), private_homes=(), probe=None, tools_dir=None):
    """Provision once and refresh owned code. Never adopt unowned resources."""
    source = Path(source or Path(__file__).parent)
    policy = pam_policy(pam_dir)
    for command in ("useradd", "usermod", "userdel", "groupdel", "loginctl", "systemctl", "systemd-run", "chvt", "fgconsole", "setpriv", "busctl"):
        if shutil.which(command, path=SAFE_ENV["PATH"]) is None:
            raise BenchError(f"Missing setup prerequisite: {command}")
    previous = None
    retry_account = False
    if layout.record.exists():
        trusted(layout.record)
        record = read_json(layout.record)
        if not isinstance(record, dict):
            raise BenchError("Partial setup record is malformed; no resources were overwritten")
        if record.get("status") in ("ready", "updating"):
            return update_launcher(host, layout, source=source, probe=probe, tools_dir=tools_dir)
        if record.get("status") == "uninstalled":
            previous = record
        elif pending_account_creation(record, host, layout):
            previous = record
            retry_account = True
        # Other partial setup is not adopted: account creation can fail
        # between NSS changes and journaling. Keep it for inspection.
        else:
            raise BenchError("Partial setup exists; inspect setup.json before repairing it; no resources were overwritten")
    if host.user() or host.group():
        raise BenchError("vela-benchmark account/group already exists without suite ownership")
    paths = (layout.base, layout.helper.parent, layout.path("etc/pam.d/vela-benchmark"),
             layout.path("etc/systemd/system") / PILOT_UNIT, layout.path("etc/systemd/system") / FAULT_UNIT)
    for path in paths:
        if path == layout.base and previous is not None:
            trusted(path, directory=True)
            continue
        if path.exists() or path.is_symlink():
            raise BenchError(f"Unowned setup path already exists: {path}")
        ancestor = next(item for item in path.parents if item.exists())
        trusted(ancestor, directory=True)
    # Validate policy/layout and all input files before creating any account.
    isolation_properties(1000, layout, private_homes)
    contents = {name: (source / name).read_bytes() for name in MODULES}
    wrapper = (source.parent / "launcher.py").read_bytes()
    native = probe_bytes(probe) if probe is not None else None
    optional_tools = instrument_payload(layout, tools_dir)
    host.run(["/usr/bin/python3", "-I", "-c", "import sys; raise SystemExit(sys.version_info < (3, 11))"])
    for name in (PILOT_UNIT, FAULT_UNIT, SESSION_UNIT):
        if host.run(["systemctl", "show", name, "--property=LoadState", "--value"], check=False).stdout.strip() != "not-found":
            raise BenchError(f"Unowned systemd unit already exists: {name}")
    layout.base.mkdir(mode=0o755, exist_ok=previous is not None)
    layout.base.chmod(0o755)
    record = {"schema": 1, "status": "provisioning", "account_tag": previous["account_tag"] if retry_account else "Vela benchmark " + uuid.uuid4().hex,
              "account": ACCOUNT, "uid": None, "gid": None, "files": {}, "home": str(layout.home),
              "private_homes": list(private_homes)}
    atomic_json(layout.record, record)
    mail = layout.base / "mail"
    mail.mkdir(mode=0o700, exist_ok=retry_account)
    mail.chmod(0o700)
    host.run(useradd_command(layout, record["account_tag"]))
    user = host.user()
    if user is None:
        raise BenchError("useradd succeeded but the account cannot be resolved")
    record.update(uid=user.pw_uid, gid=user.pw_gid)
    record["mail_directory"] = str(mail)
    atomic_json(layout.record, record)
    host.run(["usermod", "--lock", ACCOUNT])
    check_account(record, host, layout)
    host.run(["loginctl", "disable-linger", ACCOUNT])
    for name in ("home", "tmp", "var-tmp"):
        path = layout.base / name
        path.mkdir(mode=0o700)
        os.chown(path, user.pw_uid, user.pw_gid)
    prepare_socket_directories(layout)
    (layout.base / "pilots").mkdir(mode=0o700, exist_ok=previous is not None)
    package = layout.helper.parent / "vela_bench"
    missing_parents = []
    for directory in package.parents:
        if directory.exists():
            break
        missing_parents.append(directory)
    record["created_code_parents"] = [str(path) for path in missing_parents if path != layout.helper.parent]
    package.mkdir(parents=True, mode=0o755)
    for directory in (package, *missing_parents):
        directory.chmod(0o755)
    for name, value in contents.items():
        path = package / name
        path.write_bytes(value)
        path.chmod(0o644)
        record["files"][str(path)] = file_hash(path)
    layout.helper.write_bytes(wrapper)
    layout.helper.chmod(0o755)
    record["files"][str(layout.helper)] = file_hash(layout.helper)
    if native is not None:
        layout.probe.write_bytes(native)
        layout.probe.chmod(0o755)
        record["files"][str(layout.probe)] = file_hash(layout.probe)
    for name, data in optional_tools.items():
        Path(name).write_bytes(data)
        Path(name).chmod(0o755)
        record["files"][name] = file_hash(name)
    dropin = layout.path(f"etc/systemd/system/user@{user.pw_uid}.service.d/70-vela-benchmark.conf")
    if dropin.exists() or dropin.is_symlink():
        raise BenchError(f"Unowned user-manager drop-in: {dropin}")
    properties = isolation_properties(user.pw_uid, layout, private_homes)
    record["created_dropin_directory"] = not dropin.parent.exists()
    dropin.parent.mkdir(parents=True, exist_ok=True)
    unit = "[Service]\n" + "".join(f"{key}={value}\n" for key, value in properties.items())
    pilot = pilot_unit(layout)
    for path, text in ((dropin, unit), (layout.path("etc/pam.d/vela-benchmark"), policy),
                       (layout.path("etc/systemd/system") / PILOT_UNIT, pilot),
                       (layout.path("etc/systemd/system") / FAULT_UNIT, pilot_unit(layout, fault=True))):
        path.write_text(text)
        path.chmod(0o644)
        record["files"][str(path)] = file_hash(path)
        atomic_json(layout.record, record)
    host.run(["systemctl", "daemon-reload"])
    record["status"] = "ready"
    atomic_json(layout.record, record)
    return record


def session_unit_info(host):
    info = host.run(["systemctl", "show", SESSION_UNIT, "--property=LoadState", "--property=User",
                     "--property=Transient", "--property=Description", "--property=ActiveState",
                     "--property=Result", "--property=ExecMainCode", "--property=ExecMainStatus"], timeout=5, check=False).stdout
    return dict(line.split("=", 1) for line in info.splitlines() if "=" in line)


def check_session_unit(unit, record):
    if (unit.get("User") not in (ACCOUNT, str(record["uid"])) or unit.get("Transient") != "yes"
            or unit.get("Description") != "Vela benchmark P0 session " + record["account_tag"]):
        raise BenchError("Session unit ownership mismatch; refusing to change an unrelated service")


def release_session_unit(host, record):
    """After UID teardown, clear only our stopped unit and await collection.

    A stop that times out can leave a failed transient unit loaded forever.
    Capture its outcome before reset; a loaded unit must not count as a
    complete teardown or block the next StartTransientUnit with the same name.
    """
    initial = session_unit_info(host)
    unit, reset = initial, False
    deadline = time.monotonic() + 5
    while unit.get("LoadState") != "not-found":
        if unit.get("LoadState") != "loaded":
            raise BenchError("Cannot establish session unit ownership during release")
        check_session_unit(unit, record)
        if unit.get("ActiveState") not in ("inactive", "failed"):
            raise BenchError("Session unit is not stopped; refusing release")
        if unit["ActiveState"] == "failed" and not reset:
            host.run(["systemctl", "reset-failed", SESSION_UNIT], timeout=5)
            reset = True
        if time.monotonic() >= deadline:
            raise BenchError("Stopped session unit was not unloaded; next session refused")
        time.sleep(.05)
        unit = session_unit_info(host)
    return {"schema": 1, "before_release": initial, "reset_failed": reset, "unloaded": True}


def stop_user(host, record, layout=Layout()):
    user = check_account(record, host, layout)
    unit = session_unit_info(host)
    if unit.get("LoadState") == "loaded":
        check_session_unit(unit, record)
        host.run(["systemctl", "stop", SESSION_UNIT], timeout=15)
    elif unit.get("LoadState") != "not-found":
        raise BenchError("Cannot establish session unit ownership")
    known = host.run(["loginctl", "show-user", str(user.pw_uid)], check=False, timeout=5)
    if known.returncode == 0:
        host.run(["loginctl", "terminate-user", str(user.pw_uid)], timeout=15)
    host.run(["systemctl", "stop", f"user@{user.pw_uid}.service", f"user-runtime-dir@{user.pw_uid}.service"], timeout=115)
    deadline = time.monotonic() + 10
    while host.pids(user.pw_uid) and time.monotonic() < deadline:
        time.sleep(.1)
    if host.pids(user.pw_uid) or layout.path(f"run/user/{user.pw_uid}").exists():
        raise BenchError("Reserved UID still has processes/runtime state; profile reset and next session refused")
    return release_session_unit(host, record)


def prepare_socket_directories(layout):
    # Both process trees bind this private /tmp. Xwayland and ICE require
    # these conventional root-owned sticky directories, even on Wayland.
    for name in (".X11-unix", ".ICE-unix"):
        path = layout.base / "tmp" / name
        path.mkdir(mode=0o1777)
        path.chmod(0o1777)


def reset_profile(host, record, layout=Layout()):
    user = check_account(record, host, layout)
    if host.pids(user.pw_uid) or layout.path(f"run/user/{user.pw_uid}").exists():
        raise BenchError("Cannot reset a live benchmark profile")
    for name in ("home", "tmp", "var-tmp"):
        path = layout.base / name
        if path.is_symlink() or (path.exists() and not path.is_dir()) or path.is_mount():
            raise BenchError(f"Unsafe profile root: {path}")
    for name in ("home", "tmp", "var-tmp"):
        path = layout.base / name
        if path.exists():
            # Python's fd-based rmtree does not follow nested symlinks. Refuse
            # bind mounts too: no deletion may cross a managed profile boundary.
            for item in path.rglob("*"):
                if not item.is_symlink() and item.is_mount():
                    raise BenchError(f"Mounted profile contents: {item}")
            shutil.rmtree(path)
        path.mkdir(mode=0o700)
        os.chown(path, user.pw_uid, user.pw_gid)
    prepare_socket_directories(layout)


def control_lock(layout=Layout(), *, wait=False):
    descriptor = os.open(layout.base / "control.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB))
    except OSError:
        os.close(descriptor)
        raise BenchError("Another privileged benchmark operation is active")
    return os.fdopen(descriptor, "w")


def probe_bytes(path):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > 8 * 1024 * 1024:
        raise BenchError("Probe must be an ELF executable of at most 8 MiB")
    with path.open("rb") as stream:
        data = stream.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024 or not data.startswith(b"\x7fELF"):
        raise BenchError("Probe must be an ELF executable; build the vela-bench-probe target first")
    return data


def instrument_payload(layout, directory):
    if directory is None:
        return {}
    payload = {str(layout.helper.parent / name): probe_bytes(Path(directory) / name)
            for name in ("vela-bench-probe", "vela-bench-desktop", "vela-screen-dump", "vela-bench-input")}
    if (Path(directory) / "vela-slowgpu").exists():
        payload[str(layout.helper.parent / "vela-slowgpu")] = probe_bytes(Path(directory) / "vela-slowgpu")
    if (Path(directory) / "vela-pattern").exists():
        payload[str(layout.helper.parent / "vela-pattern")] = probe_bytes(Path(directory) / "vela-pattern")
    if (Path(directory) / "vela-bench-scene").exists():
        payload[str(layout.helper.parent / "vela-bench-scene")] = probe_bytes(Path(directory) / "vela-bench-scene")
    return payload


def launcher_payload(layout=Layout(), source=None, probe=None, tools_dir=None):
    """Fixed installed paths and source bytes; no caller-supplied file list."""
    source = Path(source or Path(__file__).parent)
    payload = {str(layout.helper.parent / "vela_bench" / name): (source / name).read_bytes() for name in MODULES}
    payload[str(layout.helper)] = (source.parent / "launcher.py").read_bytes()
    payload[str(layout.path("etc/systemd/system") / PILOT_UNIT)] = pilot_unit(layout).encode()
    payload[str(layout.path("etc/systemd/system") / FAULT_UNIT)] = pilot_unit(layout, fault=True).encode()
    if probe is not None:
        payload[str(layout.probe)] = probe_bytes(probe)
    payload.update(instrument_payload(layout, tools_dir))
    return payload


def require_current_launcher(layout=Layout(), source=None):
    for name, data in launcher_payload(layout, source).items():
        if file_hash(name) != hashlib.sha256(data).hexdigest():
            raise BenchError("Installed launcher code differs from this checkout. "
                             "Run sudo ./scripts/vela-bench setup before running benchmarks.")
        trusted(name)


def update_launcher(host, layout=Layout(), source=None, probe=None, tools_dir=None):
    """Refresh only owned code, retaining the account, policies and evidence."""
    trusted(layout.record)
    payload = launcher_payload(layout, source, probe, tools_dir)
    after = {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()}
    with control_lock(layout):
        record = read_json(layout.record)
        if not isinstance(record, dict) or record.get("schema") != 1 or record.get("status") not in ("ready", "updating"):
            raise BenchError("Code update requires a completed owned setup")
        check_account(record, host, layout)
        require_workers_stopped(host)
        if host.pids(record["uid"]) or layout.path(f"run/user/{record['uid']}").exists():
            raise BenchError("Stop and restore the pilot before updating installed code")
        if (layout.base / "active.json").exists():
            from .pilot import state_directory, TERMINAL
            # Recovery fixes must be installable after cleanup failed. Workers,
            # reserved processes and runtime are already proved absent above;
            # retain the pending state and evidence for the upgraded recover.
            if read_json(state_directory(layout) / "state.json").get("stage") not in (*TERMINAL, "cleanup-failed"):
                raise BenchError("Recover the unfinished pilot before updating installed code")
        # Derive optional module additions from the fixed installed payload.
        # A separate migration list missed desktop.py in the P2 → P3 upgrade.
        # Original core resources must still be present in the ownership record;
        # existing unowned paths remain rejected by the checks below.
        additions = {str(layout.helper.parent / "vela_bench" / name) for name in MODULES if name not in BASE_MODULES}
        additions |= {str(layout.helper.parent / name) for name in ("vela-bench-probe", "vela-bench-desktop", "vela-screen-dump", "vela-bench-input", "vela-slowgpu", "vela-bench-scene", "vela-pattern")}
        additions.add(str(layout.path("etc/systemd/system") / FAULT_UNIT))
        if not isinstance(record.get("files"), dict) or payload.keys() - record["files"].keys() - additions:
            raise BenchError("Installed code layout differs; code-only update refused")
        update = record.get("code_update")
        if record["status"] == "updating":
            if (not isinstance(update, dict) or update.get("after") != after
                    or not isinstance(update.get("before"), dict) or update["before"].keys() != payload.keys()
                    or any(update["before"][name] != record["files"].get(name) for name in payload)):
                raise BenchError("Interrupted update source/record changed; refusing to adopt uncertain files")
        else:
            update = {"before": {name: record["files"].get(name) for name in payload}, "after": after}
        for name in payload.keys() - record["files"].keys():
            path = Path(name)
            trusted(path.parent, directory=True)
            if path.exists() or path.is_symlink():
                if record["status"] != "updating" or file_hash(path) != after[name]:
                    raise BenchError(f"Unowned installed code path: {name}")
                trusted(path)
        for name, digest in record["files"].items():
            trusted(name)
            permitted = {digest}
            if record["status"] == "updating" and name in payload:
                permitted.add(after[name])
            if file_hash(name) not in permitted:
                raise BenchError(f"Installed benchmark resource changed: {name}")
        if record["status"] == "ready" and update["before"] == after:
            return record
        record.update(status="updating", code_update=update)
        atomic_json(layout.record, record)
        for name, data in payload.items():
            path = Path(name)
            descriptor, temporary = tempfile.mkstemp(prefix=".code-update-", dir=path.parent)
            try:
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(data)
                    stream.flush()
                    os.fchmod(stream.fileno(), 0o755 if path.parent == layout.helper.parent else 0o644)
                    os.fsync(stream.fileno())
                os.replace(temporary, path)
                parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(parent)
                finally:
                    os.close(parent)
            finally:
                Path(temporary).unlink(missing_ok=True)
        record["files"].update(after)
        if any(update["before"][str(layout.path("etc/systemd/system") / service)] != after[str(layout.path("etc/systemd/system") / service)]
               for service in (PILOT_UNIT, FAULT_UNIT)):
            host.run(["systemctl", "daemon-reload"])
        record.pop("code_update")
        record["status"] = "ready"
        atomic_json(layout.record, record)
        return record


def uninstall(host, layout=Layout()):
    record = load_setup(host, layout)
    with control_lock(layout):
        require_workers_stopped(host)
        if (layout.base / "active.json").exists():
            from .pilot import state_directory, TERMINAL
            if read_json(state_directory(layout) / "state.json").get("stage") not in TERMINAL:
                raise BenchError("An unrestored pilot remains; recover before uninstalling")
        stop_user(host, record, layout)
        reset_profile(host, record, layout)
        # Validate every directory before removing the account. Untracked
        # files in the code installation must not leave an avoidable partial
        # removal after the NSS identity has already gone.
        code_root = layout.helper.parent
        expected = {Path(name) for name in record["files"] if Path(name).is_relative_to(code_root)}
        actual = {path for path in code_root.rglob("*") if path.is_file() or path.is_symlink()}
        if actual != expected:
            raise BenchError("Untracked installed launcher files; inspect before uninstalling")
        mail = layout.base / "mail"
        if record.get("mail_directory"):
            if record["mail_directory"] != str(mail):
                raise BenchError("Unexpected mailbox directory")
            trusted(mail, directory=True)
            for path in mail.iterdir():
                info = path.lstat()
                if path.name != ACCOUNT or not stat.S_ISREG(info.st_mode) or info.st_uid != record["uid"]:
                    raise BenchError("Untracked mailbox contents; refusing removal")
        host.run(["userdel", ACCOUNT])
        group = host.group()
        if group is not None:
            if group.gr_gid != record["gid"] or group.gr_mem:
                raise BenchError("Owned primary group changed; refusing to delete it")
            host.run(["groupdel", ACCOUNT])
        for name in (".X11-unix", ".ICE-unix"):
            (layout.base / "tmp" / name).rmdir()
        for name in ("home", "tmp", "var-tmp"):
            (layout.base / name).rmdir()
        if record.get("mail_directory"):
            (mail / ACCOUNT).unlink(missing_ok=True)
            mail.rmdir()
        for name in record["files"]:
            Path(name).unlink()
        dropin_dir = layout.path(f"etc/systemd/system/user@{record['uid']}.service.d")
        if record.get("created_dropin_directory") and dropin_dir.exists() and not any(dropin_dir.iterdir()):
            dropin_dir.rmdir()
        (layout.helper.parent / "vela_bench").rmdir()
        layout.helper.parent.rmdir()
        for name in record.get("created_code_parents", []):
            path = Path(name)
            if path.is_dir() and not any(path.iterdir()):
                path.rmdir()
        host.run(["systemctl", "daemon-reload"])
        record["status"] = "uninstalled"
        atomic_json(layout.record, record)
    return {"status": "uninstalled", "preserved_pilot_evidence": str(layout.base / "pilots"),
            "other_result_archives": "preserved"}
