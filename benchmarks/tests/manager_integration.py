#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real start-limit recovery with harmless temporary user units.

Only the unit name and system/user-manager boundary are substituted. Never
addresses the display manager, PAM, a desktop, a VT or physical hardware.
Requires a running user manager and PyGObject. Run explicitly, unprivileged.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from gi.repository import Gio, GLib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench import BenchError
from vela_bench.archive import atomic_json, read_json
from vela_bench.host import HostOperationError, SAFE_ENV, SESSION_UNIT, release_session_unit
from vela_bench.pilot import properties, restore_display_manager


def main(output=None):
    if os.getuid() == 0:
        raise RuntimeError("run the user-manager integration unprivileged")
    address = f"unix:path=/run/user/{os.getuid()}/bus"
    env = dict(SAFE_ENV, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}", DBUS_SESSION_BUS_ADDRESS=address)
    connection = Gio.DBusConnection.new_for_address_sync(address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    units, refs = [], []

    def run(argv, *, check=True, timeout=8):
        result = subprocess.run(argv, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise HostOperationError(argv, result.returncode, result.stdout, result.stderr)
        return result

    def reference(method, unit):
        connection.call_sync("org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", method, GLib.Variant("(s)", (unit,)),
            None, Gio.DBusCallFlags.NONE, 5000, None)

    def wait(unit, wanted):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            info = properties(run(["systemctl", "--user", "show", unit, "--property=ActiveState",
                "--property=SubState", "--property=Result"]).stdout)
            if info.get("SubState") == wanted:
                return info
            time.sleep(.025)
        raise RuntimeError(f"unit did not reach {wanted}: {info}")

    def create(program, burst):
        unit = "vela-benchmark-limit-check-" + uuid.uuid4().hex + ".service"
        units.append(unit)
        run(["systemd-run", "--user", "--quiet", "--service-type=exec", "--unit=" + unit,
             "--property=RemainAfterExit=yes", "--property=StartLimitIntervalSec=30",
             "--property=StartLimitBurst=" + str(burst), program])
        reference("RefUnit", unit)
        refs.append(unit)
        return unit

    class MappedHost:
        def __init__(self, unit, expected="display-manager.service"):
            self.unit, self.expected, self.commands = unit, expected, []

        def run(self, command, **kwargs):
            assert command[0] == "systemctl" and command[1] in ("show", "start", "stop", "reset-failed")
            assert command[2] == self.expected
            self.commands.append(command)
            return run(["systemctl", "--user", command[1], self.unit, *command[3:]], **kwargs)

    try:
        with tempfile.TemporaryDirectory(prefix="vela-manager-recovery-") as temporary:
            base = Path(temporary)
            unit = create("/usr/bin/true", 2)
            wait(unit, "exited")
            run(["systemctl", "--user", "stop", unit])
            run(["systemctl", "--user", "start", unit])
            wait(unit, "exited")
            run(["systemctl", "--user", "stop", unit])
            # Keep a bus reference so systemd retains the same unit and its
            # counter after each clean stop, just like an installed service.
            host = MappedHost(unit)
            state = {"stage": "restoring", "run_id": uuid.uuid4().hex}
            atomic_json(base / "state.json", state)
            restore_display_manager(host, base, state)
            clean = read_json(base / "display-manager-recovery.json")
            assert clean["before"] == {"ActiveState": "inactive", "Result": "success"}, clean
            assert clean["blocked"]["Result"] == "start-limit-hit", clean
            assert clean["rate_limit_reset"] and clean["status"] == "passed", clean
            assert wait(unit, "exited")["ActiveState"] == "active"
            assert sum(c[1] == "reset-failed" for c in host.commands) == 1

            failed = create("/usr/bin/false", 1)
            wait(failed, "failed")
            run(["systemctl", "--user", "start", failed], check=False)
            assert wait(failed, "failed")["Result"] == "start-limit-hit"
            host = MappedHost(failed)
            try:
                restore_display_manager(host, base, {})
            except BenchError:
                pass
            else:
                raise RuntimeError("pre-existing service failure was incorrectly reset")
            assert not any(c[1] == "reset-failed" for c in host.commands)

            # Reproduce the session-name collision after timeout cleanup. This
            # process deliberately ignores TERM, then is frozen. Only its own
            # temporary unit is signaled, and its stop deadline is 250 ms.
            session = "vela-benchmark-session-check-" + uuid.uuid4().hex + ".service"
            units.append(session)
            ready = base / "ready.stderr"
            ready.touch(mode=0o600)
            prefix = ["systemd-run", "--user", "--quiet", "--service-type=exec", "--unit=" + session,
                "--uid=" + str(os.getuid()), "--property=Description=Vela benchmark P0 session release-fixture",
                "--property=RemainAfterExit=yes", "--property=TimeoutStopSec=250ms",
                "--property=StandardOutput=append:" + str(ready)]
            run([*prefix, sys.executable, "-I", "-c",
                "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('ready',flush=True); time.sleep(60)"])
            deadline = time.monotonic() + 5
            while "ready" not in ready.read_text():
                if time.monotonic() >= deadline:
                    raise RuntimeError("timeout fixture did not initialize")
                time.sleep(.025)
            run(["systemctl", "--user", "kill", "--signal=STOP", "--kill-whom=all", session])
            run(["systemctl", "--user", "stop", session])
            assert wait(session, "failed")["Result"] == "timeout"
            # Creating another transient service with this name must fail
            # before the new release operation, just as in the actual pilot.
            occupied = run([*prefix, "/usr/bin/true"], check=False)
            assert occupied.returncode and "already loaded" in occupied.stderr, occupied.stderr
            mapped = MappedHost(session, SESSION_UNIT)
            released = release_session_unit(mapped, {"uid": os.getuid(), "account_tag": "release-fixture"})
            assert released["before_release"]["Result"] == "timeout" and released["unloaded"], released
            run([*prefix, "/usr/bin/true"])
            assert wait(session, "exited")["Result"] == "success"
            run(["systemctl", "--user", "stop", session])
            release_session_unit(mapped, {"uid": os.getuid(), "account_tag": "release-fixture"})
            evidence = {"schema": 1, "scope": "real-user-manager-mapped-unit-no-pam-tty-desktop",
                "clean_stop_rate_limit_reproduced_and_recovered": True,
                "preexisting_failure_not_reset": True, "recovery": clean,
                "timeout_session_collision_reproduced": True, "same_session_name_recreated_after_release": True,
                "session_release": released}
            if output:
                path = Path(output)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(evidence, indent=2) + "\n")
            print(json.dumps(evidence, indent=2))
    finally:
        for unit in units:
            run(["systemctl", "--user", "stop", unit], check=False)
            run(["systemctl", "--user", "reset-failed", unit], check=False)
        for unit in refs:
            reference("UnrefUnit", unit)
        connection.close_sync(None)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
