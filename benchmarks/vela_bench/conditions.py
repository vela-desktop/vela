# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed display, brightness and power operations with durable restoration."""

import hashlib
import json
import math
from pathlib import Path
import pwd
import re
import shlex
import stat
import time

from . import BenchError
from .archive import decode_json
from .host import SAFE_ENV, Layout, HostOperationError
from .diagnostics import user_command

# Fixed timing/configuration across targets. The root worker intentionally
# has no operator HOME; ddcutil's default DSA cache otherwise fails at init.
DDC_COMMAND = ("/usr/bin/ddcutil", "--noconfig", "--disable-dynamic-sleep", "--disable-capabilities-cache")
DDC_READ_SECONDS, DDC_READ_ATTEMPTS = 8, 4


def connector(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,63}", value):
        raise BenchError("Invalid physical connector identity")
    return value


class OutputsUnavailable(BenchError):
    """A valid adapter has not announced any connected output yet."""


def normalized_outputs(raw, adapter):
    data = decode_json(raw)
    if adapter == "plasma":
        if not isinstance(data, dict) or data.get("backend") != "libkscreen-wayland-v1":
            raise BenchError("Unexpected Plasma output backend")
        rows = data.get("outputs")
    elif adapter == "vela":
        rows = data
    else:
        raise BenchError("No qualified display adapter for this target")
    if not isinstance(rows, list) or len(rows) > 16:
        shape = f"list with {len(rows)} entries" if isinstance(rows, list) else type(rows).__name__
        raise BenchError(f"Invalid {adapter} output inventory: expected at most 16 outputs, got {shape}")
    if not rows:
        raise OutputsUnavailable(f"{adapter} output inventory is empty: compositor inactive or outputs not announced")
    result = []
    for row in rows:
        name = connector(row.get("name"))
        if adapter == "vela":
            modes = [{"id": f"{mode['width']}x{mode['height']}@{mode['refresh']:.6f}",
                      "width": mode["width"], "height": mode["height"], "refresh_mhz": round(mode["refresh"] * 1000),
                      "preferred": mode.get("preferred") is True, "current": mode.get("current") is True}
                     for mode in row.get("modes", [])]
            current = next((mode["id"] for mode in modes if mode["current"]), None)
            rotation = {"normal": 1, "90": 2, "180": 4, "270": 8,
                        "flipped": 16, "flipped-90": 32, "flipped-180": 64, "flipped-270": 128}.get(row.get("transform"))
            adaptive = row.get("adaptive_sync")
            if adaptive is False:
                vrr = 0
            elif adaptive is True:
                vrr = 1
            elif row.get("enabled") is False:
                vrr = None
            else:
                raise BenchError("Cannot verify Vela adaptive-sync state")
            item = {"name": name, "enabled": row.get("enabled"), "scale": row.get("scale", 1),
                    "x": row.get("position", {}).get("x", 0), "y": row.get("position", {}).get("y", 0),
                    "rotation": rotation, "vrr": vrr, "priority": 0, "current_mode_id": current, "modes": modes}
        else:
            item = {key: row.get(key) for key in ("name", "enabled", "scale", "x", "y", "rotation", "vrr", "priority", "current_mode_id", "modes")}
        if type(item["enabled"]) is not bool or not isinstance(item["modes"], list):
            raise BenchError("Missing display state")
        for mode in item["modes"]:
            if (not isinstance(mode, dict) or not isinstance(mode.get("id"), str)
                    or any(type(mode.get(key)) is not int or mode[key] <= 0 for key in ("width", "height", "refresh_mhz"))
                    or mode["width"] > 16384 or mode["height"] > 16384 or mode["refresh_mhz"] > 1000000):
                raise BenchError("Invalid advertised output mode")
        if item["enabled"]:
            if (not isinstance(item["scale"], (int, float)) or isinstance(item["scale"], bool)
                    or not math.isfinite(item["scale"]) or not .5 <= item["scale"] <= 4
                    or item["rotation"] not in (1, 2, 4, 8, 16, 32, 64, 128) or item["vrr"] not in (0, 1, 2)
                    or any(type(item[key]) is not int for key in ("x", "y", "priority"))
                    or item["current_mode_id"] not in {mode["id"] for mode in item["modes"]}):
                raise BenchError("Unverifiable current output state")
        result.append(item)
    if len({row["name"] for row in result}) != len(result) or not any(row["enabled"] for row in result):
        raise BenchError("Ambiguous or entirely disabled output inventory")
    return result


def desired_profile(outputs, primary=None, fixed_mode=None):
    if primary is None:
        candidates = [row for row in outputs if row["enabled"] and row["priority"] == 1]
        if len(candidates) != 1:
            candidates = [row for row in outputs if row["enabled"]]
        if len(candidates) != 1:
            raise BenchError("Cannot resolve exactly one primary output before takeover")
        primary = candidates[0]["name"]
    primary = connector(primary)
    matches = [row for row in outputs if row["name"] == primary]
    if len(matches) != 1:
        raise BenchError("Resolved primary monitor is no longer connected")
    modes = matches[0]["modes"]
    if fixed_mode is None:
        preferred = [mode for mode in modes if mode.get("preferred")]
        if not preferred:
            raise BenchError("Primary output does not advertise a native preferred mode")
        native = max(preferred, key=lambda mode: mode["width"] * mode["height"])
        modes = [mode for mode in modes if (mode["width"], mode["height"]) == (native["width"], native["height"])]
        chosen = max(modes, key=lambda mode: mode["refresh_mhz"])
        fixed_mode = {key: chosen[key] for key in ("width", "height", "refresh_mhz")}
    available = [mode for mode in modes if all(mode[key] == fixed_mode[key] for key in fixed_mode)]
    if not available:
        raise BenchError("Requested physical mode/refresh is unavailable; no alternate mode was selected")
    desired = []
    for original in outputs:
        item = dict(original, enabled=original["name"] == primary)
        if not item["enabled"]:
            item["priority"] = 0
        if item["enabled"]:
            item.update(current_mode_id=available[0]["id"], scale=1.0, x=0, y=0, rotation=1, vrr=0, priority=1)
        desired.append(item)
    return {"primary": primary, "mode": fixed_mode, "outputs": desired}


def compare_outputs(observed, wanted, *, priorities=False):
    actual = {row["name"]: row for row in observed}
    if set(actual) != {row["name"] for row in wanted}:
        raise BenchError("Connected output set changed")
    for row in wanted:
        current = actual[row["name"]]
        keys = ("enabled", "scale", "x", "y", "rotation", "vrr") if row["enabled"] else ("enabled",)
        if any(current[key] != row[key] for key in keys):
            raise BenchError(f"Output condition read-back mismatch: {row['name']}")
        # Enabled priority 0 means the backend did not advertise an order.
        # Verify every known rank, and the disabled state independently.
        if priorities and row["priority"] > 0 and current["priority"] != row["priority"]:
            raise BenchError("Primary-output priority was not restored")
        if row["enabled"]:
            before = next(mode for mode in row["modes"] if mode["id"] == row["current_mode_id"])
            after = next(mode for mode in current["modes"] if mode["id"] == current["current_mode_id"])
            if any(before[key] != after[key] for key in ("width", "height", "refresh_mhz")):
                raise BenchError("Applied mode differs from the recorded physical mode")


def display_command(layout, adapter, outputs=None):
    if adapter == "plasma":
        return [str(layout.helper.parent / "vela-bench-desktop"), "--outputs"] if outputs is None else [
            str(layout.helper.parent / "vela-bench-desktop"), "--apply", json.dumps(outputs, allow_nan=False)]
    if adapter != "vela":
        raise BenchError("Unsupported display adapter")
    if outputs is None:
        return ["/usr/bin/wlr-randr", "--json"]
    command = ["/usr/bin/wlr-randr"]
    for row in outputs:
        command += ["--output", connector(row["name"]), "--on" if row["enabled"] else "--off"]
        if row["enabled"]:
            mode = next(mode for mode in row["modes"] if mode["id"] == row["current_mode_id"])
            transform = {1: "normal", 2: "90", 4: "180", 8: "270", 16: "flipped", 32: "flipped-90", 64: "flipped-180", 128: "flipped-270"}[row["rotation"]]
            command += ["--mode", f"{mode['width']}x{mode['height']}@{mode['refresh_mhz'] / 1000:.6f}Hz",
                        "--scale", str(row["scale"]), "--pos", f"{row['x']},{row['y']}", "--transform", transform,
                        "--adaptive-sync", "disabled" if row["vrr"] == 0 else "enabled"]
    return command


def greeter_environment(sessions, proc=Path("/proc"), *, host=None, run=Path("/run"), desktop=None):
    greeters = [row for row in sessions if row.get("Class") == "greeter" and row.get("Type") == "wayland"
                and row.get("Remote") == "no" and row.get("Seat") == "seat0" and row.get("State") != "closing"]
    if len(greeters) != 1:
        raise BenchError("Controlled P0 needs exactly one Wayland greeter to snapshot/restore the original outputs")
    session = greeters[0]
    identity = session.get("Id", "")
    vt = session.get("VTNr", "")
    if (not re.fullmatch(r"[A-Za-z0-9_-]+", identity) or not session.get("User", "").isdigit()
            or not vt.isdigit() or not 1 <= int(vt) <= 63):
        raise BenchError("Invalid greeter session identity")
    user = pwd.getpwuid(int(session["User"]))
    if user.pw_name not in ("plasmalogin", "sddm") or user.pw_uid == 0:
        raise BenchError("Greeter output adapter is currently qualified only for Plasma Login/SDDM")
    runtime = str(run / "user" / str(user.pw_uid))
    try:
        info = Path(runtime).lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != user.pw_uid or stat.S_IMODE(info.st_mode) != 0o700:
            raise BenchError("Greeter runtime has an unexpected owner, type or permissions")
    except OSError as exc:
        raise BenchError("Greeter runtime is unavailable") from exc

    def environment(env):
        wayland = env.get("WAYLAND_DISPLAY", "")
        if (env.get("XDG_RUNTIME_DIR") != runtime or not re.fullmatch(r"wayland-[A-Za-z0-9_-]{1,64}", wayland)
                or not env.get("DBUS_SESSION_BUS_ADDRESS", "").startswith("unix:")):
            return None
        try:
            endpoint = (Path(runtime) / wayland).lstat()
            if not stat.S_ISSOCK(endpoint.st_mode) or endpoint.st_uid != user.pw_uid:
                return None
        except OSError:
            return None
        return {"uid": user.pw_uid, "gid": user.pw_gid, "vt": int(vt), "environment": {**SAFE_ENV, "HOME": user.pw_dir,
                "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": wayland,
                "DBUS_SESSION_BUS_ADDRESS": env["DBUS_SESSION_BUS_ADDRESS"], "QT_QPA_PLATFORM": "wayland"}}

    # Plasma Login's compositor/greeter live under user@UID.service, not the
    # logind session scope. WAYLAND_DISPLAY is imported into the manager
    # after its start, so /proc/PID/environ is not an authoritative source.
    manager_error = "no native user bus"
    if host is not None:
        try:
            info = (Path(runtime) / "bus").lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != user.pw_uid:
                raise BenchError("user bus is not an owned socket")
            bus = "unix:path=" + runtime + "/bus"
            manager = {"uid": user.pw_uid, "gid": user.pw_gid, "environment": {**SAFE_ENV, "HOME": user.pw_dir,
                       "XDG_RUNTIME_DIR": runtime, "DBUS_SESSION_BUS_ADDRESS": bus}}
            result = host.run(greeter_command(manager, ["/usr/bin/systemctl", "--user", "show-environment"]), check=False, timeout=5)
            if result.returncode:
                raise BenchError("user manager environment query failed")
            env = {"XDG_RUNTIME_DIR": runtime, "DBUS_SESSION_BUS_ADDRESS": bus}
            for line in result.stdout.splitlines():
                key, separator, value = line.partition("=")
                if separator and key in ("WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"):
                    parsed = shlex.split(value)
                    if len(parsed) != 1:
                        raise BenchError("user manager returned a malformed display variable")
                    env[key] = parsed[0]
            if env["DBUS_SESSION_BUS_ADDRESS"] != bus:
                raise BenchError("user manager returned a foreign bus address")
            if not env.get("WAYLAND_DISPLAY"):
                # Direct kwin_wayland (as shipped by Plasma Login) need not
                # export WAYLAND_DISPLAY. Resolve its actual listening socket
                # by matching SO_PEERCRED with the org.kde.KWin bus owner.
                helper = desktop or Layout().helper.parent / "vela-bench-desktop"
                result = host.run(greeter_command(manager, [str(helper), "--resolve-wayland"]), check=False, timeout=8)
                if result.returncode:
                    raise BenchError("KWin socket validation failed: " + result.stderr.strip()[-1000:])
                endpoint = decode_json(result.stdout)
                if (not isinstance(endpoint, dict) or type(endpoint.get("schema")) is not int or endpoint["schema"] != 1
                        or endpoint.get("method") != "kwin-socket-peer-v1" or not isinstance(endpoint.get("wayland_display"), str)
                        or type(endpoint.get("uid")) is not int or endpoint["uid"] != user.pw_uid
                        or type(endpoint.get("pid")) is not int or endpoint["pid"] <= 0):
                    raise BenchError("KWin socket resolver returned an invalid owner")
                env["WAYLAND_DISPLAY"] = endpoint.get("wayland_display")
            resolved = environment(env)
            if resolved:
                return resolved
            manager_error = "user manager has no live owned Wayland endpoint"
        except (BenchError, OSError, ValueError) as exc:
            manager_error = str(exc)

    # Older SDDM configurations launch a private bus inside the greeter's
    # own session. Preserve this explicit route without reading other UIDs.
    for directory in proc.glob("[0-9]*"):
        try:
            status = (directory / "status").read_text()
            uid_line = next(line for line in status.splitlines() if line.startswith("Uid:"))
            if set(map(int, uid_line.split()[1:])) != {user.pw_uid}:
                continue
            if not any(f"session-{identity}.scope" in line.split(":", 2)[-1].split("/")
                       for line in (directory / "cgroup").read_text().splitlines()):
                continue
            env = dict(item.decode().split("=", 1) for item in (directory / "environ").read_bytes().split(b"\0") if b"=" in item)
            resolved = environment(env)
            if resolved:
                return resolved
        except (OSError, UnicodeError, ValueError, StopIteration):
            continue
    raise BenchError(f"Cannot locate greeter {user.pw_name} (UID {user.pw_uid}, session {identity}): "
                     f"{manager_error}; no valid session-scoped environment")


def greeter_command(greeter, command):
    return ["setpriv", f"--reuid={greeter['uid']}", f"--regid={greeter['gid']}", "--clear-groups", "/usr/bin/env", "-i",
            *(f"{key}={value}" for key, value in greeter["environment"].items()), *command]


def output_identities(outputs, sysfs=Path("/sys")):
    result = {}
    for row in outputs:
        matches = [path for path in (sysfs / "class/drm").glob("card*-" + connector(row["name"]))
                   if (path / "status").read_text().strip() == "connected"]
        if len(matches) != 1:
            raise BenchError("Cannot identify every connected DRM output uniquely")
        edid = (matches[0] / "edid").read_bytes()
        if len(edid) < 128:
            raise BenchError("Connected output has no usable monitor identity")
        result[row["name"]] = {"connector": matches[0].name, "edid_sha256": hashlib.sha256(edid).hexdigest()}
    return result


def read_ddc_brightness(host, bus):
    """Bound DDC readiness after a VT/link transition; never retry writes.

    Code 1 is ddcutil's device/communication failure, not proof the monitor
    is unsupported immediately after a compositor resumes. Syntax errors
    and signaled processes are not transport readiness retries.
    """
    command = [*DDC_COMMAND, "--bus", str(bus), "--terse", "getvcp", "10"]
    deadline, attempts, last_error = time.monotonic() + DDC_READ_SECONDS, [], None
    for attempt in range(DDC_READ_ATTEMPTS):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            result = host.run(command, timeout=min(5, remaining))
            attempts.append({"argv": command, "returncode": 0, "stdout": result.stdout[-8192:],
                             "stderr": getattr(result, "stderr", "")[-8192:]})
            return result.stdout, attempts
        except HostOperationError as exc:
            last_error = exc
            attempts.append(dict(exc.operation))
            transient = exc.operation["returncode"] == 1 or exc.operation["timeout_seconds"] is not None
            remaining = deadline - time.monotonic()
            if not transient or attempt + 1 == DDC_READ_ATTEMPTS or remaining <= .3:
                exc.operation["attempts"] = attempts
                raise
            time.sleep(.3)
    if last_error is not None:
        last_error.operation["attempts"] = attempts
        raise last_error
    raise BenchError(f"DDC readiness deadline expired on bus {bus}")


def brightness_snapshot(host, primary, sysfs=Path("/sys")):
    matches = [path for path in (sysfs / "class/drm").glob("card*-" + connector(primary)) if (path / "status").read_text().strip() == "connected"]
    if len(matches) != 1:
        raise BenchError("Cannot identify the primary DRM connector uniquely")
    physical = matches[0]
    raw_edid = (physical / "edid").read_bytes()
    if len(raw_edid) < 128:
        raise BenchError("Primary monitor has no usable EDID identity for reversible brightness control")
    edid = hashlib.sha256(raw_edid).hexdigest()
    ddc = physical / "ddc"
    if ddc.exists() and re.fullmatch(r"i2c-[0-9]+", ddc.resolve().name):
        bus = int(ddc.resolve().name.removeprefix("i2c-"))
        try:
            raw, attempts = read_ddc_brightness(host, bus)
        except BenchError as exc:
            raise BenchError(f"DDC brightness read failed on bus {bus}: {exc}") from exc
        match = re.fullmatch(r"VCP\s+10\s+C\s+(\d+)\s+(\d+)\s*", raw.strip())
        if not match or not 0 <= int(match[1]) <= int(match[2]) <= 65535 or int(match[2]) == 0:
            raise BenchError("Primary monitor has no verified DDC brightness range")
        if (physical / "edid").read_bytes() != raw_edid or (physical / "status").read_text().strip() != "connected":
            raise BenchError("Primary monitor changed during DDC readiness checks")
        return {"backend": "ddcutil-vcp10", "bus": bus, "value": int(match[1]), "maximum": int(match[2]),
                "connector": physical.name, "edid_sha256": edid, "read_attempts": attempts}
    backlights = list((sysfs / "class/backlight").iterdir())
    if primary.startswith("eDP-") and len(backlights) == 1:
        path = backlights[0]
        value, maximum = int((path / "brightness").read_text()), int((path / "max_brightness").read_text())
        if not re.fullmatch(r"[A-Za-z0-9_-]+", path.name) or not 0 <= value <= maximum or maximum <= 0:
            raise BenchError("Invalid backlight brightness range")
        return {"backend": "sysfs-backlight", "device": path.name, "value": value, "maximum": maximum,
                "connector": physical.name, "edid_sha256": edid}
    raise BenchError("No verified primary brightness control; no software dimming substitute selected")


def set_brightness(host, snapshot, value, sysfs=Path("/sys")):
    if type(value) is not int or not 0 <= value <= snapshot["maximum"]:
        raise BenchError("Brightness outside recorded hardware range")
    # Rediscover the endpoint and EDID before every write, including recovery.
    current = brightness_snapshot(host, snapshot["connector"].split("-", 1)[1], sysfs)
    for key in ("backend", "connector", "edid_sha256", "maximum", "bus", "device"):
        if current.get(key) != snapshot.get(key):
            raise BenchError("Brightness endpoint identity changed; refusing hardware write")
    # A verified read is sufficient when the panel is already at its target.
    # In particular, failed startup before configuration needs no DDC write.
    if current["value"] == value:
        return current
    if snapshot["backend"] == "ddcutil-vcp10":
        try:
            host.run([*DDC_COMMAND, "--bus", str(snapshot["bus"]), "setvcp", "10", str(value)], timeout=5)
        except BenchError as exc:
            raise BenchError(f"DDC brightness write failed on bus {snapshot['bus']}: {exc}") from exc
    else:
        path = sysfs / "class/backlight" / snapshot["device"] / "brightness"
        path.write_text(str(value))
    for _ in range(3):
        current = brightness_snapshot(host, snapshot["connector"].split("-", 1)[1], sysfs)
        if current["value"] == value:
            return current
        time.sleep(.1)
    raise BenchError("Hardware brightness did not reach the requested value")


def power_snapshot(host):
    profile = host.run(["/usr/bin/powerprofilesctl", "get"], timeout=5).stdout.strip()
    if profile not in ("performance", "balanced", "power-saver"):
        raise BenchError("Unsupported platform power-profile state")
    return profile


def set_power(host, profile):
    if profile not in ("performance", "balanced", "power-saver"):
        raise BenchError("Invalid fixed platform profile")
    host.run(["/usr/bin/powerprofilesctl", "set", profile], timeout=5)
    if power_snapshot(host) != profile:
        raise BenchError("Platform power-profile read-back differs from requested state")


def power_sources(sysfs=Path("/sys")):
    result = {}
    for path in (sysfs / "class/power_supply").glob("*"):
        try:
            if (path / "type").read_text().strip() in ("Mains", "USB", "USB_C", "USB_PD"):
                result[path.name] = (path / "online").read_text().strip()
        except FileNotFoundError:
            continue
    return result


def greeter_outputs(host, layout, greeter, *, activate=False):
    if activate:
        host.run(["chvt", str(greeter["vt"])], timeout=5)
    if host.run(["fgconsole"], timeout=5).stdout.strip() != str(greeter["vt"]):
        raise BenchError("Greeter output query requires its active VT; snapshot belongs after START in the supervised worker")
    deadline = time.monotonic() + 8
    while True:
        try:
            raw = host.run(greeter_command(greeter, display_command(layout, "plasma")), timeout=8).stdout
            return normalized_outputs(raw, "plasma")
        except OutputsUnavailable as exc:
            if time.monotonic() >= deadline:
                raise BenchError(f"Greeter snapshot on tty{greeter['vt']}: {exc}; output announcement deadline expired") from exc
            time.sleep(.2)


def snapshot_conditions(host, layout, sessions, *, activate=False):
    greeter = greeter_environment(sessions, host=host, desktop=layout.helper.parent / "vela-bench-desktop")
    outputs = greeter_outputs(host, layout, greeter, activate=activate)
    profile = desired_profile(outputs)
    return {"schema": 1, "source": "declared-wayland-greeter", "outputs": outputs, "profile": profile,
            "output_identities": output_identities(outputs),
            "brightness": brightness_snapshot(host, profile["primary"]), "power_profile": power_snapshot(host),
            "power_sources": power_sources()}


def apply_conditions(host, record, layout, target, snapshot):
    query = user_command(record, layout, "display", display_command(layout, target))
    actual = normalized_outputs(host.run(query, timeout=32).stdout, target)
    if output_identities(actual) != snapshot["output_identities"]:
        raise BenchError("Connected monitor identities changed before applying conditions")
    profile = desired_profile(actual, snapshot["profile"]["primary"], snapshot["profile"]["mode"])
    host.run(user_command(record, layout, "display", display_command(layout, target, profile["outputs"])), timeout=32)
    set_power(host, "performance")
    set_brightness(host, snapshot["brightness"], snapshot["brightness"]["maximum"])
    return verify_conditions(host, record, layout, target, snapshot)


def verify_conditions(host, record, layout, target, snapshot):
    actual = normalized_outputs(host.run(user_command(record, layout, "display", display_command(layout, target)), timeout=32).stdout, target)
    if output_identities(actual) != snapshot["output_identities"]:
        raise BenchError("Connected monitor identities changed during the block")
    profile = desired_profile(actual, snapshot["profile"]["primary"], snapshot["profile"]["mode"])
    compare_outputs(actual, profile["outputs"], priorities=target == "plasma")
    brightness = brightness_snapshot(host, profile["primary"])
    if any(brightness.get(key) != snapshot["brightness"].get(key) for key in
           ("backend", "connector", "edid_sha256", "maximum", "bus", "device")) or brightness["value"] != brightness["maximum"]:
        raise BenchError("Hardware brightness/identity changed during the block")
    if power_snapshot(host) != "performance" or power_sources() != snapshot["power_sources"]:
        raise BenchError("Power profile or power source changed during the block")
    return {"status": "passed", "outputs": actual, "brightness": brightness, "power_profile": "performance",
            "power_sources": snapshot["power_sources"]}


def restore_conditions(host, layout, snapshot, get_sessions):
    result = {"status": "failed", "components": {}, "errors": []}
    output_stage = "greeter-query"
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                greeter = greeter_environment(get_sessions(), host=host, desktop=layout.helper.parent / "vela-bench-desktop")
                break
            except BenchError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.25)
        fresh = greeter_outputs(host, layout, greeter, activate=True)
        result["outputs_before"] = fresh
        output_stage = "identity-and-mode-validation"
        if output_identities(fresh) != snapshot["output_identities"]:
            raise BenchError("Connected monitors changed; refusing output restoration to replacement hardware")
        wanted = []
        for saved in snapshot["outputs"]:
            found = next((row for row in fresh if row["name"] == saved["name"]), None)
            if found is None:
                raise BenchError("Original monitor is no longer connected during restoration")
            restored = dict(saved, modes=found["modes"])
            if saved["enabled"]:
                mode = next(mode for mode in saved["modes"] if mode["id"] == saved["current_mode_id"])
                modes = [row for row in found["modes"] if all(row[key] == mode[key] for key in ("width", "height", "refresh_mhz"))]
                if not modes:
                    raise BenchError("Original output mode is unavailable during restoration")
                restored["current_mode_id"] = modes[0]["id"]
            wanted.append(restored)
        result["outputs_requested"] = wanted
        output_stage = "apply"
        host.run(greeter_command(greeter, display_command(layout, "plasma", wanted)), timeout=8)
        output_stage = "read-back"
        actual = normalized_outputs(host.run(greeter_command(greeter, display_command(layout, "plasma")), timeout=8).stdout, "plasma")
        result["outputs_after"] = actual
        if output_identities(actual) != snapshot["output_identities"]:
            raise BenchError("Connected monitors changed during output restoration")
        compare_outputs(actual, wanted, priorities=True)
        result["components"]["outputs"] = "passed"
    except (BenchError, OSError, StopIteration) as exc:
        result["components"]["outputs"] = "failed"
        result["errors"].append("Output restoration: " + str(exc))
        result["output_failure_stage"] = output_stage
        if isinstance(exc, HostOperationError):
            result["output_failed_operation"] = exc.operation
            try:
                failure = decode_json(exc.operation["stdout"])
            except (ValueError, BenchError):
                failure = None
            # Only a native SetConfig rejection after exact identity/mode
            # validation can request one supervised greeter restart. A lost
            # socket, timeout, hotplug or read-back mismatch remains a failure.
            result["output_restart_eligible"] = (output_stage == "apply"
                and exc.operation["returncode"] == 1 and exc.operation["timeout_seconds"] is None
                and isinstance(failure, dict) and failure.get("schema") == 1
                and failure.get("event") == "output-configuration-failed"
                and failure.get("stage") == "kscreen-set-config")
    for name, operation in (("brightness", lambda: set_brightness(host, snapshot["brightness"], snapshot["brightness"]["value"])),
                            ("power_profile", lambda: set_power(host, snapshot["power_profile"]))):
        try:
            operation()
            result["components"][name] = "passed"
        except (BenchError, OSError) as exc:
            result["components"][name] = "failed"
            result["errors"].append(f"{name} restoration: {exc}")
    if not result["errors"]:
        result["status"] = "passed"
    return result
