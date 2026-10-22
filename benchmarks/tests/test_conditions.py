# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import json
import os
from pathlib import Path
import socket
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.conditions import (brightness_snapshot, compare_outputs, desired_profile, display_command,
                                   greeter_environment, greeter_outputs, normalized_outputs, output_identities, restore_conditions, set_brightness, set_power)
from vela_bench.host import HostOperationError, Layout


def plasma():
    mode = {"id": "preferred", "width": 1920, "height": 1080, "refresh_mhz": 60000, "preferred": True}
    return [{"name": "DP-1", "enabled": True, "priority": 1, "scale": 1.25, "x": 100, "y": 0,
             "rotation": 1, "vrr": 2, "current_mode_id": "preferred", "modes": [mode]},
            {"name": "HDMI-A-1", "enabled": True, "priority": 2, "scale": 1.0, "x": 1636, "y": 0,
             "rotation": 1, "vrr": 0, "current_mode_id": "preferred", "modes": [copy.deepcopy(mode)]}]


def encoded(rows):
    return json.dumps({"backend": "libkscreen-wayland-v1", "outputs": rows})


class ControlledConditions(unittest.TestCase):
    def greeter_fixture(self, base, user_name="plasmalogin"):
        # CI containers can run as root, but greeter_environment intentionally
        # refuses UID 0. Keep fixtures unprivileged on every test runner.
        uid, gid = os.getuid(), os.getgid()
        if uid == 0:
            uid, gid = 65534, 65534
        user = SimpleNamespace(pw_uid=uid, pw_gid=gid, pw_name=user_name, pw_dir="/var/lib/" + user_name)
        session = {"Id": "c4", "User": str(user.pw_uid), "VTNr": "1", "Class": "greeter", "Type": "wayland",
                   "Remote": "no", "Seat": "seat0", "State": "online"}
        run, proc = base / "run", base / "proc"
        runtime = run / "user" / str(user.pw_uid)
        runtime.mkdir(parents=True, mode=0o700)
        proc.mkdir()
        for name in ("bus", "wayland-0"):
            endpoint = socket.socket(socket.AF_UNIX)
            self.addCleanup(endpoint.close)
            try:
                endpoint.bind(str(runtime / name))
            except PermissionError:
                self.skipTest("sandbox denies Unix socket fixtures; rerun outside the sandbox")
        # Bind as the test runner, then match the simulated greeter's UID/GID.
        # The production ownership checks must remain exercised in root CI.
        if os.geteuid() == 0:
            runtime.chown(uid, gid)
            for name in ("bus", "wayland-0"):
                (runtime / name).chown(uid, gid)
        return user, session, runtime, run, proc

    def test_greeter_fixture_is_unprivileged_and_owns_its_sockets(self):
        with tempfile.TemporaryDirectory() as temporary:
            user, _, runtime, _, _ = self.greeter_fixture(Path(temporary))
            self.assertNotEqual(user.pw_uid, 0)
            for path in (runtime, runtime / "bus", runtime / "wayland-0"):
                self.assertEqual(path.lstat().st_uid, user.pw_uid)
                self.assertEqual(path.lstat().st_gid, user.pw_gid)

    def test_root_greeter_is_rejected_before_querying_user_manager(self):
        from unittest.mock import Mock
        host = SimpleNamespace(run=Mock())
        session = {"Id": "c4", "User": "0", "VTNr": "1", "Class": "greeter", "Type": "wayland",
                   "Remote": "no", "Seat": "seat0", "State": "online"}
        root = SimpleNamespace(pw_uid=0, pw_gid=0, pw_name="plasmalogin", pw_dir="/var/lib/plasmalogin")
        with patch("vela_bench.conditions.pwd.getpwuid", return_value=root), self.assertRaisesRegex(BenchError, "only for Plasma"):
            greeter_environment([session], host=host)
        host.run.assert_not_called()

    def test_plasma_login_services_use_manager_environment_outside_session_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            user, session, runtime, run, proc = self.greeter_fixture(Path(temporary))
            # This reproduces the real layout: the only greeter process in
            # the session has no Wayland variable; services are in user@UID.
            process = proc / "10"
            process.mkdir()
            (process / "status").write_text("Uid:\t" + "\t".join([str(user.pw_uid)] * 4))
            (process / "cgroup").write_text("0::/user.slice/user-1000.slice/session-c4.scope\n")
            (process / "environ").write_bytes(b"HOME=/var/lib/plasmalogin\0")
            from unittest.mock import Mock
            host = SimpleNamespace(run=Mock(return_value=SimpleNamespace(returncode=0, stdout=
                f"WAYLAND_DISPLAY=wayland-0\nXDG_RUNTIME_DIR={runtime}\nDBUS_SESSION_BUS_ADDRESS='unix:path={runtime}/bus'\nSECRET=never-forwarded\n")))
            with patch("vela_bench.conditions.pwd.getpwuid", return_value=user):
                result = greeter_environment([session], proc, host=host, run=run)
            self.assertEqual(result["environment"]["WAYLAND_DISPLAY"], "wayland-0")
            self.assertEqual(result["environment"]["DBUS_SESSION_BUS_ADDRESS"], f"unix:path={runtime}/bus")
            self.assertNotIn("SECRET", result["environment"])
            argv = host.run.call_args.args[0]
            self.assertEqual(argv[:4], ["setpriv", f"--reuid={user.pw_uid}", f"--regid={user.pw_gid}", "--clear-groups"])
            self.assertEqual(argv[-3:], ["/usr/bin/systemctl", "--user", "show-environment"])

    def test_manager_environment_cannot_select_foreign_runtime_bus_or_missing_socket(self):
        for variable in ("XDG_RUNTIME_DIR=/run/user/999999", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/999999/bus",
                         "WAYLAND_DISPLAY=../../foreign", "WAYLAND_DISPLAY=wayland-missing"):
            with self.subTest(variable=variable), tempfile.TemporaryDirectory() as temporary:
                user, session, runtime, run, proc = self.greeter_fixture(Path(temporary))
                host = SimpleNamespace(run=lambda *args, **kwargs: SimpleNamespace(returncode=0,
                                       stdout="WAYLAND_DISPLAY=wayland-0\n" + variable + "\n"))
                with patch("vela_bench.conditions.pwd.getpwuid", return_value=user), self.assertRaisesRegex(BenchError, "Cannot locate greeter"):
                    greeter_environment([session], proc, host=host, run=run)

    def test_missing_manager_display_uses_fixed_unprivileged_peer_resolver(self):
        from unittest.mock import Mock
        for case in ("good", "foreign-uid", "missing-name", "boolean-pid", "wrong-method"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                user, session, runtime, run, proc = self.greeter_fixture(Path(temporary))
                result = {"schema": 1, "method": "kwin-socket-peer-v1", "uid": user.pw_uid, "pid": 777, "wayland_display": "wayland-0"}
                if case == "foreign-uid": result["uid"] += 1
                elif case == "missing-name": result["wayland_display"] = None
                elif case == "boolean-pid": result["pid"] = True
                elif case == "wrong-method": result["method"] = "unverified-guess"
                helper = Path("/fixed/vela-bench-desktop")
                host = SimpleNamespace(run=Mock(side_effect=[SimpleNamespace(returncode=0, stdout="", stderr=""),
                    SimpleNamespace(returncode=0, stdout=json.dumps(result), stderr="")]))
                with patch("vela_bench.conditions.pwd.getpwuid", return_value=user):
                    if case == "good":
                        resolved = greeter_environment([session], proc, host=host, run=run, desktop=helper)
                        self.assertEqual(resolved["environment"]["WAYLAND_DISPLAY"], "wayland-0")
                    else:
                        with self.assertRaises(BenchError): greeter_environment([session], proc, host=host, run=run, desktop=helper)
                argv = host.run.call_args.args[0]
                self.assertEqual(argv[:4], ["setpriv", f"--reuid={user.pw_uid}", f"--regid={user.pw_gid}", "--clear-groups"])
                self.assertEqual(argv[-2:], [str(helper), "--resolve-wayland"])

    def test_sddm_private_session_bus_still_works_and_scope_prefix_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            user, session, runtime, run, proc = self.greeter_fixture(Path(temporary), "sddm")
            process = proc / "10"
            process.mkdir()
            (process / "status").write_text("Uid:\t" + "\t".join([str(user.pw_uid)] * 4))
            (process / "cgroup").write_text("0::/user.slice/session-c4.scope\n")
            (process / "environ").write_bytes(f"XDG_RUNTIME_DIR={runtime}\0WAYLAND_DISPLAY=wayland-0\0DBUS_SESSION_BUS_ADDRESS=unix:abstract=/tmp/private-greeter-bus\0".encode())
            with patch("vela_bench.conditions.pwd.getpwuid", return_value=user):
                result = greeter_environment([session], proc, run=run)
                self.assertEqual(result["environment"]["DBUS_SESSION_BUS_ADDRESS"], "unix:abstract=/tmp/private-greeter-bus")
                (process / "cgroup").write_text("0::/user.slice/session-c4.scope-forged\n")
                with self.assertRaises(BenchError):
                    greeter_environment([session], proc, run=run)

    def test_non_greeter_account_is_rejected_before_querying_its_user_manager(self):
        from unittest.mock import Mock
        host = SimpleNamespace(run=Mock())
        with tempfile.TemporaryDirectory() as temporary:
            user, session, runtime, run, proc = self.greeter_fixture(Path(temporary), "personal")
            with patch("vela_bench.conditions.pwd.getpwuid", return_value=user), self.assertRaisesRegex(BenchError, "only for Plasma"):
                greeter_environment([session], proc, host=host, run=run)
        host.run.assert_not_called()

    def test_greeter_runtime_and_socket_types_are_verified(self):
        for case in ("runtime-permissions", "bus-file", "wayland-file", "wayland-symlink"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                user, session, runtime, run, proc = self.greeter_fixture(Path(temporary))
                if case == "runtime-permissions":
                    runtime.chmod(0o755)
                else:
                    endpoint = runtime / ("bus" if case == "bus-file" else "wayland-0")
                    endpoint.unlink()
                    if case == "wayland-symlink": endpoint.symlink_to(runtime / "bus")
                    else: endpoint.write_text("not a socket")
                host = SimpleNamespace(run=lambda *args, **kwargs: SimpleNamespace(returncode=0, stdout="WAYLAND_DISPLAY=wayland-0\n"))
                with patch("vela_bench.conditions.pwd.getpwuid", return_value=user), self.assertRaises(BenchError):
                    greeter_environment([session], proc, host=host, run=run)

    def test_primary_mode_scale_and_vrr_are_resolved_once_without_lower_mode_fallback(self):
        outputs = normalized_outputs(encoded(plasma()), "plasma")
        profile = desired_profile(outputs)
        self.assertEqual(profile["primary"], "DP-1")
        self.assertEqual([row["enabled"] for row in profile["outputs"]], [True, False])
        self.assertEqual(profile["outputs"][0]["scale"], 1)
        self.assertEqual(profile["outputs"][0]["vrr"], 0)
        with self.assertRaisesRegex(BenchError, "no alternate mode"):
            desired_profile(outputs, "DP-1", {"width": 1920, "height": 1080, "refresh_mhz": 144000})
        ambiguous = plasma()
        for row in ambiguous:
            row["priority"] = 0
        with self.assertRaisesRegex(BenchError, "primary"):
            desired_profile(ambiguous)

    def test_wlr_inventory_uses_observed_boolean_adaptive_sync_and_exact_refresh(self):
        rows = [{"name": "DP-1", "enabled": True, "modes": [{"width": 1920, "height": 1080, "refresh": 59.94,
                 "current": True, "preferred": True}], "scale": 1.0, "position": {"x": 0, "y": 0},
                 "transform": "normal", "adaptive_sync": False}]
        result = normalized_outputs(json.dumps(rows), "vela")
        self.assertEqual(result[0]["modes"][0]["refresh_mhz"], 59940)
        self.assertEqual(result[0]["vrr"], 0)
        del rows[0]["adaptive_sync"]
        with self.assertRaisesRegex(BenchError, "adaptive-sync"):
            normalized_outputs(json.dumps(rows), "vela")

    def test_readback_rejects_hotplug_wrong_scaling_mode_and_unrestored_primary(self):
        wanted = desired_profile(plasma())["outputs"]
        for field, value in (("scale", 1.5), ("vrr", 2), ("x", 10), ("enabled", False), ("priority", 2)):
            rows = copy.deepcopy(wanted)
            rows[0][field] = value
            with self.subTest(field=field), self.assertRaises(BenchError):
                compare_outputs(rows, wanted, priorities=True)
        with self.assertRaisesRegex(BenchError, "Connected"):
            compare_outputs(wanted[:1], wanted)
        rows = copy.deepcopy(wanted)
        rows[0]["modes"][0]["refresh_mhz"] = 75000
        with self.assertRaisesRegex(BenchError, "physical mode"):
            compare_outputs(rows, wanted)

    def test_invalid_connectors_cannot_become_command_options_or_paths(self):
        for name in ("--help", "../DP-1", "DP-1\n--off", "DP-1/other"):
            rows = plasma()
            rows[0]["name"] = name
            with self.subTest(name=name), self.assertRaises(BenchError):
                normalized_outputs(encoded(rows), "plasma")
        command = display_command(Layout(), "vela", desired_profile(plasma())["outputs"])
        self.assertEqual(command[0], "/usr/bin/wlr-randr")
        self.assertIn("1920x1080@60.000000Hz", command)
        self.assertIn("--off", command)

    def test_brightness_uses_drm_endpoint_and_edid_and_refuses_monitor_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            sysfs = Path(directory)
            drm = sysfs / "class/drm/card1-DP-1"
            drm.mkdir(parents=True)
            (drm / "status").write_text("connected\n")
            (drm / "edid").write_bytes(b"A" * 128)
            bus = sysfs / "devices/i2c-7"
            bus.mkdir(parents=True)
            (drm / "ddc").symlink_to(bus)
            class Host:
                def __init__(self):
                    self.value, self.calls = 25, []
                def run(self, command, **kwargs):
                    self.calls.append(command)
                    if "setvcp" in command:
                        self.value = int(command[-1])
                    return SimpleNamespace(stdout=f"VCP 10 C {self.value} 100\n")
            host = Host()
            snapshot = brightness_snapshot(host, "DP-1", sysfs)
            identities = output_identities([{"name": "DP-1"}], sysfs)
            self.assertEqual(identities["DP-1"]["edid_sha256"], snapshot["edid_sha256"])
            self.assertEqual(set_brightness(host, snapshot, 100, sysfs)["value"], 100)
            self.assertEqual(set_brightness(host, snapshot, snapshot["value"], sysfs)["value"], 25)
            written = list(host.calls)
            host.calls.clear()
            self.assertEqual(set_brightness(host, snapshot, 25, sysfs)["value"], 25)
            self.assertFalse(any("setvcp" in call for call in host.calls))
            self.assertTrue(any("getvcp" in call for call in host.calls))
            # Every read, write and recovery read must bypass per-user config
            # and caches; the actual root Host deliberately omits HOME.
            for call in written + host.calls:
                self.assertEqual(call[0], "/usr/bin/ddcutil")
                for option in ("--noconfig", "--disable-dynamic-sleep", "--disable-capabilities-cache"):
                    self.assertIn(option, call)
            (drm / "edid").write_bytes(b"B" * 128)
            host.calls.clear()
            with self.assertRaisesRegex(BenchError, "identity changed"):
                set_brightness(host, snapshot, 100, sysfs)
            self.assertFalse(any("setvcp" in call for call in host.calls))

    def test_power_profile_readback_is_mandatory_and_governor_is_not_a_substitute(self):
        class Host:
            def run(self, command, **kwargs):
                return SimpleNamespace(stdout="balanced\n")
        with self.assertRaisesRegex(BenchError, "read-back"):
            set_power(Host(), "performance")
        with self.assertRaisesRegex(BenchError, "Invalid"):
            set_power(Host(), "schedutil")

    def test_output_restore_refuses_replaced_monitor_before_writing_layout(self):
        host = SimpleNamespace(run=lambda argv, **kwargs: SimpleNamespace(stdout="1\n" if argv == ["fgconsole"] else encoded(plasma())))
        from unittest.mock import Mock
        host.run = Mock(side_effect=host.run)
        with patch("vela_bench.conditions.greeter_environment", return_value={"uid": 957, "gid": 957, "vt": 1, "environment": {}}), \
                patch("vela_bench.conditions.output_identities", return_value={"DP-1": "replacement"}), \
                patch("vela_bench.conditions.set_brightness"), patch("vela_bench.conditions.set_power"):
            result = restore_conditions(host, Layout(), {"outputs": plasma(), "output_identities": {"DP-1": "original"},
                                       "brightness": {"value": 25}, "power_profile": "balanced"}, lambda: [])
        self.assertEqual(result["status"], "failed")
        self.assertIn("replacement hardware", result["errors"][0])
        self.assertFalse(any("--apply" in call.args[0] for call in host.run.call_args_list))

    def test_inactive_greeter_is_activated_before_query_and_empty_announcements_are_retried(self):
        from unittest.mock import Mock
        # Real DRM greeters may publish no heads while another VT is active.
        calls, active, reads = [], 3, 0
        def run(argv, **kwargs):
            nonlocal active, reads
            calls.append(argv)
            if argv[0] == "chvt":
                active = int(argv[1])
                return SimpleNamespace(stdout="")
            if argv == ["fgconsole"]:
                return SimpleNamespace(stdout=str(active))
            self.assertEqual(active, 1)
            reads += 1
            return SimpleNamespace(stdout=encoded([] if reads == 1 else plasma()))
        greeter = {"uid": 957, "gid": 957, "vt": 1, "environment": {}}
        with patch("vela_bench.conditions.time.sleep"):
            actual = greeter_outputs(SimpleNamespace(run=run), Layout(), greeter, activate=True)
        self.assertEqual(calls[:2], [["chvt", "1"], ["fgconsole"]])
        self.assertEqual(reads, 2)
        self.assertEqual(actual[0]["name"], "DP-1")

    def test_only_native_setconfig_rejection_after_identity_validation_permits_restart(self):
        from unittest.mock import Mock
        marker = {"schema": 1, "event": "output-configuration-failed", "stage": "kscreen-set-config"}
        cases = ((marker, 1, None, True), ({}, 1, None, False),
                 (dict(marker, stage="get-config"), 1, None, False),
                 (marker, 2, None, False), (marker, None, 8, False))
        for failure, code, timeout, eligible in cases:
            with self.subTest(failure=failure, code=code, timeout=timeout):
                def run(argv, **kwargs):
                    if "--apply" in argv:
                        raise HostOperationError(argv, code, json.dumps(failure), "driver rejected", timeout=timeout)
                    return SimpleNamespace(stdout="1\n" if argv == ["fgconsole"] else encoded(plasma()))
                host = SimpleNamespace(run=Mock(side_effect=run))
                with patch("vela_bench.conditions.greeter_environment", return_value={"uid": 957, "gid": 957, "vt": 1, "environment": {}}), \
                        patch("vela_bench.conditions.output_identities", return_value={"DP-1": "original"}), \
                        patch("vela_bench.conditions.set_brightness"), patch("vela_bench.conditions.set_power"):
                    result = restore_conditions(host, Layout(), {"outputs": plasma(), "output_identities": {"DP-1": "original"},
                        "brightness": {"value": 25}, "power_profile": "balanced"}, lambda: [])
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["output_restart_eligible"], eligible)
                self.assertEqual(result["output_failed_operation"]["stderr"], "driver rejected")
                self.assertEqual(result["outputs_requested"], plasma())
                self.assertEqual(result["outputs_before"], plasma())

    def test_restoration_readback_checks_identity_and_exact_layout_without_a_restart(self):
        from unittest.mock import Mock
        for changed in (False, True):
            with self.subTest(changed=changed):
                host = SimpleNamespace(run=Mock(side_effect=lambda argv, **kwargs: SimpleNamespace(
                    stdout="1\n" if argv == ["fgconsole"] else encoded(plasma()))))
                with patch("vela_bench.conditions.greeter_environment", return_value={"uid": 957, "gid": 957, "vt": 1, "environment": {}}), \
                        patch("vela_bench.conditions.output_identities", side_effect=[{"DP-1": "original"},
                            {"DP-1": "replacement" if changed else "original"}]), \
                        patch("vela_bench.conditions.set_brightness"), patch("vela_bench.conditions.set_power"):
                    result = restore_conditions(host, Layout(), {"outputs": plasma(), "output_identities": {"DP-1": "original"},
                        "brightness": {"value": 25}, "power_profile": "balanced"}, lambda: [])
                self.assertEqual(result["status"], "failed" if changed else "passed")
                self.assertFalse(result.get("output_restart_eligible"))
                self.assertEqual(result["outputs_after"], plasma())

    def test_read_only_probe_refuses_inactive_greeter_without_switching_vt(self):
        from unittest.mock import Mock
        host = SimpleNamespace(run=Mock(return_value=SimpleNamespace(stdout="3\n")))
        with self.assertRaisesRegex(BenchError, "after START"):
            greeter_outputs(host, Layout(), {"vt": 1}, activate=False)
        host.run.assert_called_once_with(["fgconsole"], timeout=5)

    def test_empty_output_deadline_is_bounded_and_identifies_adapter_and_vt(self):
        host = SimpleNamespace(run=lambda argv, **kwargs: SimpleNamespace(stdout="1\n" if argv == ["fgconsole"] else encoded([])))
        with patch("vela_bench.conditions.time.monotonic", side_effect=[0, 9]), self.assertRaisesRegex(BenchError, "tty1: plasma.*empty.*deadline"):
            greeter_outputs(host, Layout(), {"vt": 1, "uid": 957, "gid": 957, "environment": {}}, activate=True)

    def test_restore_attempts_brightness_and_power_even_when_output_restore_fails(self):
        class Host:
            def run(self, command, **kwargs):
                raise BenchError("display unavailable")
        with patch("vela_bench.conditions.greeter_environment", return_value={"uid": 957, "gid": 957,
                    "vt": 1, "environment": {}}), patch("vela_bench.conditions.set_brightness") as brightness, \
                patch("vela_bench.conditions.set_power") as power:
            result = restore_conditions(Host(), Layout(), {"outputs": plasma(), "brightness": {"value": 25},
                                        "power_profile": "balanced"}, lambda: [])
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["components"], {"outputs": "failed", "brightness": "passed", "power_profile": "passed"})
            brightness.assert_called_once()
            power.assert_called_once()

    def test_personal_session_cannot_supply_the_greeter_environment(self):
        with self.assertRaisesRegex(BenchError, "greeter"):
            greeter_environment([{"Class": "user", "Type": "wayland", "Seat": "seat0", "Remote": "no"}])


if __name__ == "__main__":
    unittest.main()
