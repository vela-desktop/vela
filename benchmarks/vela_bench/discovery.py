# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Read-only host discovery. Never execute a desktop entry or a target binary."""

import configparser
import hashlib
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess

from . import VERSION

SESSION_FILES = {"vela": "vela.desktop", "plasma": "plasma.desktop", "gnome": "gnome.desktop"}
COMPONENTS = {"vela": ("vela-compositor", "vela-shell"), "plasma": ("kwin_wayland", "plasmashell"),
              "gnome": ("gnome-shell",)}
TOOLS = ("systemctl", "systemd-run", "loginctl", "powerprofilesctl", "udevadm", "cc", "wayland-scanner")


def read(path):
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return None


def file_hash(path):
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError:
        return None


def executable(name):
    path = shutil.which(name)
    if path is None:
        return None
    real = str(Path(path).resolve())
    return {"path": real, "sha256": file_hash(real)}


def sessions(directory=Path("/usr/share/wayland-sessions")):
    found, rejected = {}, []
    for target, filename in SESSION_FILES.items():
        path = Path(directory) / filename
        if not path.is_file():
            continue
        try:
            parser = configparser.ConfigParser(interpolation=None, strict=True)
            parser.optionxform = str
            parser.read_string(path.read_text(encoding="utf-8"))
            entry = parser["Desktop Entry"]
            # Display-manager session entries (including Plasma's) commonly
            # omit Type. An explicit incompatible type is still rejected.
            if entry.get("Type", "Application") != "Application" or entry.get("Hidden", "false").lower() == "true":
                raise ValueError("not a visible Application entry")
            argv = shlex.split(entry["Exec"], posix=True)
            # No expansion, shell evaluation or execution here. Field codes
            # are deliberately unsupported until an adapter resolves them.
            if not argv or any("%" in x for x in argv):
                raise ValueError("unsupported Exec field codes")
            identities = []
            for name in dict.fromkeys([argv[0], entry.get("TryExec", argv[0])] + [x for x in argv[1:] if x.startswith("/")]):
                identity = executable(name)
                if identity is None:
                    raise ValueError(f"executable unavailable: {name}")
                identities.append(identity)
            found[target] = {"session_file": str(path), "session_sha256": file_hash(path), "argv": argv,
                             "executables": identities, "binary_source_revision": None,
                             "components": {name: executable(name) for name in COMPONENTS[target]},
                             "adapter_qualified": False}
        except (OSError, ValueError, KeyError, configparser.Error) as exc:
            rejected.append({"target": target, "reason": str(exc)})
    return found, rejected


def power_sources(sysroot):
    found = []
    for path in sorted((Path(sysroot) / "class/power_supply").glob("*")):
        kind, scope = read(path / "type"), read(path / "scope")
        eligible = kind == "Battery" and scope == "System"
        found.append({"name": path.name, "type": kind, "scope": scope, "status": read(path / "status"),
                      "whole_system_candidate": eligible,
                      "reason": "requires live discharge/counter validation" if eligible else "not a verified system battery"})
    return found


def package_inventory():
    """Bounded, fixed read-only queries; target package absence is explicit."""
    queries = (
        ("pacman", ["-Q"], ("vela", "plasma-workspace", "kwin", "gnome-shell", "mutter", "mesa", "vulkan-radeon", "nvidia-utils", "linux", "systemd", "wayland")),
        ("dpkg-query", ["-W", "-f=${binary:Package}\t${Version}\n"], ("vela", "plasma-workspace", "kwin-wayland", "gnome-shell", "mutter", "libgl1-mesa-dri", "mesa-vulkan-drivers", "systemd", "libwayland-client0")),
        ("rpm", ["-q", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\n"], ("vela", "plasma-workspace", "kwin", "gnome-shell", "mutter", "mesa-dri-drivers", "mesa-vulkan-drivers", "systemd", "wayland")),
    )
    for name, flags, packages in queries:
        command = shutil.which(name)
        if command is None:
            continue
        try:
            result = subprocess.run([command, *flags, *packages], capture_output=True, text=True, timeout=8,
                                    env=dict(os.environ, LC_ALL="C"), check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return {"manager": name, "status": "unavailable", "reason": str(exc)}
        versions = {}
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] in packages:
                versions[parts[0]] = parts[1]
        return {"manager": name, "status": "observed", "versions": versions,
                "missing": [x for x in packages if x not in versions]}
    return {"manager": None, "status": "unsupported"}


def discover(*, sysroot=Path("/sys"), procroot=Path("/proc"), session_dir=Path("/usr/share/wayland-sessions")):
    targets, rejected = sessions(session_dir)
    cpuinfo = read(Path(procroot) / "cpuinfo") or ""
    model = next((line.split(":", 1)[1].strip() for line in cpuinfo.splitlines() if line.startswith("model name")), None)
    gpu = []
    for path in sorted((Path(sysroot) / "class/drm").glob("card[0-9]*")):
        if not path.name[4:].isdigit():
            continue
        device = path / "device"
        gpu.append({"card": path.name, "vendor": read(device / "vendor"), "device": read(device / "device"),
                    "driver": (device / "driver").resolve().name if (device / "driver").exists() else None})
    connectors = []
    for path in sorted((Path(sysroot) / "class/drm").glob("card*-*")):
        if (path / "status").is_file():
            connectors.append({"name": path.name, "status": read(path / "status"), "edid_sha256": file_hash(path / "edid"),
                               "modes": (read(path / "modes") or "").splitlines()})
    # DRM sysfs modes do not establish the active mode, primary output, refresh
    # or scale. Those are intentionally unresolved before adapter launch.
    environment = {"kernel": platform.release(), "architecture": platform.machine(), "os": platform.freedesktop_os_release(),
                   "cpu": {"model": model, "logical_cpus": os.cpu_count()}, "gpu": gpu,
                   "connectors": connectors, "packages": package_inventory()}
    module_dir = Path(__file__).parent
    suite_sources = {p.name: file_hash(p) for p in sorted(module_dir.glob("*.py"))}
    return {"environment": environment, "targets": targets, "rejected_sessions": rejected,
            "power_sources": power_sources(sysroot), "tools": {name: shutil.which(name) for name in TOOLS},
            "operator": {"session_type": os.environ.get("XDG_SESSION_TYPE"), "stdin_is_tty": os.isatty(0)},
            "display_verification": "pending real-session adapter", "suite": {"version": VERSION, "sources": suite_sources},
            "live_execution_available": False,
            "calibration_execution_available": True,
            "campaign_exercise_available": True,
            "neutral_client_measurements_available": ["responsiveness", "motion", "slow-apps"],
            "platform_measurements_available": ["scaling", "overhead"],
            "limitations": ["Historical P0/P1 and P2/P3 fast acceptance covers the initial Vela/Plasma setup. Current P4 pattern/resources need physical acceptance; full family coverage remains incomplete.",
                            "No active display, brightness or power-profile changes are made by discovery.",
                            "Planning budgets are provisional; they are not measured durations."]}
