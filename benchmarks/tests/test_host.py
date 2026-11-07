# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Host lifecycle contracts with synthetic NSS, commands and temporary files.

These tests NEVER create accounts, contact systemd/logind or switch a VT.
They are not evidence that PAM/logind works on a real seat.
"""

import contextlib
import io
import os
import shutil
import subprocess
from pathlib import Path
import stat
import signal
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json, read_json
from vela_bench.cli import main
from vela_bench.host import (ACCOUNT, Layout, PILOT_UNIT, SESSION_UNIT, check_account, control_lock,
                            install, isolation_properties, pam_policy, reset_profile, stop_user, trusted,
                            unit_quote, uninstall, useradd_command, update_launcher, require_current_launcher, MODULES, HostOperationError,
                            release_session_unit)
from vela_bench.pilot import (check_profile_environment, desktop_environment, recover, reserve_vt,
                             select_login_session, session_command, tty_preconditions, worker,
                             preserve_session_log, wait_session, restore_display_manager, restore_hardware_conditions)


class FakeHost:
    def __init__(self):
        self.account = None
        self.primary = None
        self.calls = []
        self.running = []
        self.password_locked = True
        self.extra_groups = []
        self.manager_active = False
        self.fail_start = False
        self.restore_vt = "2\n"
        self.fail_useradd = False

    def run(self, argv, **kwargs):
        self.calls.append(argv)
        stdout, code = "", 0
        if argv[0] == "useradd":
            if self.fail_useradd:
                raise BenchError("synthetic useradd configuration error")
            if "CREATE_MAIL_SPOOL=no" in argv:
                raise AssertionError("CREATE_MAIL_SPOOL is not a login.defs --key")
            self.account = SimpleNamespace(pw_name=ACCOUNT, pw_uid=1240, pw_gid=1240,
                pw_dir=argv[argv.index("--home-dir") + 1], pw_shell="/bin/sh",
                pw_gecos=argv[argv.index("--comment") + 1])
            self.primary = SimpleNamespace(gr_gid=1240, gr_mem=[])
        elif argv[:2] == ["userdel", ACCOUNT]:
            self.account = None
        elif argv[:2] == ["groupdel", ACCOUNT]:
            self.primary = None
        elif argv[:2] == ["loginctl", "show-user"]:
            code = 1
        elif argv[:3] == ["systemctl", "start", "display-manager.service"]:
            if self.fail_start:
                raise BenchError("synthetic display-manager start failure")
            self.manager_active = True
        elif argv[:2] == ["systemctl", "show"]:
            if "--property=LoadState" in argv:
                stdout = "not-found\n" if "--value" in argv else "LoadState=not-found\n"
            else:
                stdout = "active\n" if self.manager_active and argv[2] == "display-manager.service" else "inactive\n"
        elif argv == ["fgconsole"]:
            stdout = self.restore_vt
        return SimpleNamespace(returncode=code, stdout=stdout, stderr="")

    def user(self):
        return self.account

    def group(self):
        return self.primary

    def groups(self, user):
        return [user.pw_gid, *self.extra_groups]

    def locked(self):
        return self.password_locked

    def pids(self, uid):
        return self.running


class HostOperations(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.layout = Layout(Path(self.temp.name))
        self.host = FakeHost()
        self.pam = self.layout.path("etc/pam.d")
        self.pam.mkdir(parents=True)
        (self.pam / "system-login").write_text("session required pam_systemd.so\n")
        self.layout.path("etc/systemd/system").mkdir(parents=True)
        self.layout.path("var/lib").mkdir(parents=True)
        # Root ownership/chown is modelled, not granted to the test runner.
        self.addCleanup(patch.stopall)
        patch("vela_bench.host.trusted").start()
        patch("vela_bench.pilot.trusted").start()
        patch("vela_bench.host.os.chown").start()

    def setup_account(self):
        return install(self.host, self.layout, pam_dir=self.pam, private_homes=["/home/operator", "/srv/person"])

    def pilot_state(self, result="passed", active_manager=True):
        run_id = "a" * 32
        directory = self.layout.base / "pilots" / run_id
        directory.mkdir()
        # Point to a deliberately invalid fixture path so this CPU test never
        # writes outside its temporary root. Patch fixture methods separately.
        state = {"schema": 1, "run_id": run_id, "stage": "restoring", "result": result,
                 "restore_required": True, "origin_vt": 2, "target_vt": 8,
                 "display_manager_was_active": active_manager, "fixture": f"/home/.vela-benchmark-{run_id}"}
        atomic_json(directory / "state.json", state)
        atomic_json(self.layout.base / "active.json", {"run_id": run_id})
        return directory

    def recover_without_fixture_io(self):
        # Only fixture cleanup is mocked; profile deletion, state and command
        # ordering below use the real implementation and temporary files.
        original_unlink, original_exists = Path.unlink, Path.exists
        def unlink(path, **kwargs):
            return None if str(path).startswith("/home/.vela-benchmark-") else original_unlink(path, **kwargs)
        def exists(path):
            return False if str(path).startswith("/home/.vela-benchmark-") else original_exists(path)
        with patch.object(Path, "unlink", unlink), patch.object(Path, "exists", exists):
            return recover(self.host, self.layout)

    def test_setup_provisions_one_normal_locked_account_and_fixed_resources(self):
        record = self.setup_account()
        self.assertEqual(record["status"], "ready")
        self.assertEqual(record["uid"], 1240)
        create = next(x for x in self.host.calls if x[0] == "useradd")
        self.assertNotIn("--system", create)
        self.assertNotIn("--groups", create)
        self.assertIn(["usermod", "--lock", ACCOUNT], self.host.calls)
        self.assertIn(["loginctl", "disable-linger", ACCOUNT], self.host.calls)
        self.assertEqual(self.host.account.pw_dir, str(self.layout.home))
        dropin = self.layout.path("etc/systemd/system/user@1240.service.d/70-vela-benchmark.conf").read_text()
        self.assertIn("ProtectHome=tmpfs", dropin)
        self.assertIn("/run/user/1240", dropin)
        self.assertIn("/srv/person", dropin)
        pilot = self.layout.path("etc/systemd/system") / PILOT_UNIT
        self.assertIn("ExecStopPost=", pilot.read_text())
        self.assertIn("RuntimeMaxSec=600", pilot.read_text())
        self.assertIn("TimeoutStopSec=300", pilot.read_text())
        self.assertEqual(self.layout.record.stat().st_mode & 0o777, 0o600)

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd-analyze unavailable")
    def test_generated_systemd_units_pass_static_validation(self):
        self.setup_account()
        session = self.layout.path("session-policy.service")
        policy = isolation_properties(1240, self.layout, ["/srv/person with spaces"])
        session.write_text("[Service]\nType=simple\nExecStart=/usr/bin/true\nUser=1240\nPAMName=vela-benchmark\n"
                           + "".join(f"{key}={value}\n" for key, value in policy.items()))
        result = subprocess.run(["systemd-analyze", "verify", "--man=no", str(session),
                                 str(self.layout.path("etc/systemd/system") / PILOT_UNIT),
                                 str(self.layout.path("etc/systemd/system/vela-benchmark-qualification.service"))],
                                capture_output=True, text=True, timeout=15)
        if result.returncode and result.stderr.strip() and all("Operation not permitted" in line
                for line in result.stderr.strip().splitlines()):
            self.skipTest("sandbox denies systemd-analyze socket options; rerun this check outside the sandbox")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_setup_is_idempotent_and_does_not_recreate_the_account(self):
        first = self.setup_account()
        before = list(self.host.calls)
        self.assertEqual(self.setup_account(), first)
        self.assertEqual(self.host.calls[len(before):],
                         [["systemctl", "show", unit, "--property=ActiveState", "--value"]
                          for unit in (PILOT_UNIT, "vela-benchmark-qualification.service")])

    def changed_source(self):
        original = Path(__file__).resolve().parents[1]
        source = self.layout.path("replacement/vela_bench")
        source.mkdir(parents=True)
        for name in MODULES:
            shutil.copyfile(original / "vela_bench" / name, source / name)
        shutil.copyfile(original / "launcher.py", source.parent / "launcher.py")
        with (source / "pilot.py").open("ab") as stream:
            stream.write(b"\n# Changed pilot fixture\n")
        return source

    def test_installed_code_update_keeps_account_policies_and_evidence(self):
        old = self.setup_account()
        source = self.changed_source()
        account = self.host.user()
        self.host.calls.clear()
        with self.assertRaisesRegex(BenchError, "sudo ./scripts/vela-bench setup"):
            require_current_launcher(self.layout, source=source)
        record = install(self.host, self.layout, source=source, pam_dir=self.pam)
        require_current_launcher(self.layout, source=source)
        self.assertIs(self.host.user(), account)
        self.assertEqual(record["uid"], old["uid"])
        self.assertEqual(record["account_tag"], old["account_tag"])
        path = self.layout.helper.parent / "vela_bench/pilot.py"
        self.assertEqual(path.read_bytes(), (source / "pilot.py").read_bytes())
        self.assertNotEqual(record["files"][str(path)], old["files"][str(path)])
        for name, digest in old["files"].items():
            if not Path(name).is_relative_to(self.layout.helper.parent):
                self.assertEqual(record["files"][name], digest)
        self.assertFalse(any(call[0] in ("useradd", "userdel", "usermod") for call in self.host.calls))
        self.assertEqual(record["status"], "ready")

    def test_code_update_is_blocked_while_reserved_uid_is_running(self):
        old = self.setup_account()
        source = self.changed_source()
        self.host.running = [123]
        with self.assertRaisesRegex(BenchError, "Stop and restore"):
            update_launcher(self.host, self.layout, source=source)
        self.assertEqual(read_json(self.layout.record), old)

    def test_recovery_fix_can_update_stopped_cleanup_failed_pilot_without_losing_state(self):
        self.setup_account()
        directory = self.pilot_state(result="failed")
        state = read_json(directory / "state.json")
        state.update(stage="cleanup-failed", cleanup_errors=["old native mode ID"])
        atomic_json(directory / "state.json", state)
        source = self.changed_source()
        update_launcher(self.host, self.layout, source=source)
        self.assertEqual(read_json(directory / "state.json"), state)
        state["stage"] = "launch"
        atomic_json(directory / "state.json", state)
        with self.assertRaisesRegex(BenchError, "Recover the unfinished"):
            update_launcher(self.host, self.layout, source=source)

    def legacy_code_layout(self):
        record = self.setup_account()
        path = self.layout.helper.parent / "vela_bench/diagnostics.py"
        path.unlink()
        del record["files"][str(path)]
        atomic_json(self.layout.record, record)
        return record

    def test_setup_migrates_previous_launcher_and_adds_probe_without_recreating_account(self):
        old = self.legacy_code_layout()
        probe = self.layout.path("built-probe")
        probe.write_bytes(b"\x7fELFsynthetic fixture; never executed")
        self.host.calls.clear()
        current = install(self.host, self.layout, pam_dir=self.pam, probe=probe)
        self.assertEqual(current["uid"], old["uid"])
        self.assertEqual(current["account_tag"], old["account_tag"])
        self.assertEqual(self.layout.probe.read_bytes(), probe.read_bytes())
        self.assertEqual(self.layout.probe.stat().st_mode & 0o777, 0o755)
        self.assertTrue((self.layout.helper.parent / "vela_bench/diagnostics.py").is_file())
        self.assertIn(str(self.layout.probe), current["files"])
        self.assertFalse(any(command[0] in ("useradd", "userdel", "usermod") for command in self.host.calls))

    def p2_code_layout(self):
        from vela_bench.discovery import file_hash
        record = self.setup_account()
        desktop = self.layout.helper.parent / "vela_bench/desktop.py"
        desktop.unlink()
        del record["files"][str(desktop)]
        # Ownership hashes describe the older installed code, rather than
        # merely removing a module from an otherwise current installation.
        installed = self.layout.helper.parent / "vela_bench/host.py"
        installed.write_text("# Synthetic owned P2 launcher implementation\n")
        record["files"][str(installed)] = file_hash(installed)
        atomic_json(self.layout.record, record)
        self.host.calls.clear()
        return record, desktop

    def test_setup_migrates_p2_to_p3_with_scene_tool_and_preserves_account_policies_and_evidence(self):
        from vela_bench.discovery import file_hash
        old, desktop = self.p2_code_layout()
        account = self.host.user()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        state["stage"] = "done"
        atomic_json(directory / "state.json", state)
        evidence = directory / "sample.jsonl"
        evidence.write_bytes(b'{"event":"prior-P2-evidence"}\n')
        evidence_before = {path.name: path.read_bytes() for path in directory.iterdir()}
        tools = self.layout.path("built-tools")
        tools.mkdir()
        for name in ("vela-bench-probe", "vela-bench-desktop", "vela-screen-dump", "vela-bench-input", "vela-slowgpu", "vela-bench-scene"):
            (tools / name).write_bytes(b"\x7fELFsynthetic unexecuted fixture " + name.encode())
        current = install(self.host, self.layout, pam_dir=self.pam, tools_dir=tools)
        require_current_launcher(self.layout)
        self.assertEqual(current["status"], "ready")
        self.assertIs(self.host.user(), account)
        for name in ("uid", "gid", "account_tag", "home", "private_homes"):
            self.assertEqual(current[name], old[name])
        self.assertEqual(current["files"][str(desktop)], file_hash(desktop))
        self.assertEqual(desktop.stat().st_mode & 0o777, 0o644)
        scene = self.layout.helper.parent / "vela-bench-scene"
        self.assertEqual(scene.read_bytes(), (tools / scene.name).read_bytes())
        self.assertEqual(scene.stat().st_mode & 0o777, 0o755)
        self.assertEqual(current["files"][str(scene)], file_hash(scene))
        for name, digest in old["files"].items():
            if not Path(name).is_relative_to(self.layout.helper.parent):
                self.assertEqual(file_hash(name), digest)
                self.assertEqual(current["files"][name], digest)
        self.assertEqual({path.name: path.read_bytes() for path in directory.iterdir()}, evidence_before)
        self.assertFalse(any(command[0] in ("useradd", "userdel", "usermod", "chvt") for command in self.host.calls))
        self.assertEqual(install(self.host, self.layout, pam_dir=self.pam, tools_dir=tools), current)

    def test_p2_module_migration_also_works_without_tools_and_does_not_adopt_an_unowned_desktop_module(self):
        old, desktop = self.p2_code_layout()
        desktop.write_text("# Unowned file: do not overwrite\n")
        with self.assertRaisesRegex(BenchError, "Unowned installed code path"):
            install(self.host, self.layout, pam_dir=self.pam)
        self.assertEqual(desktop.read_text(), "# Unowned file: do not overwrite\n")
        self.assertEqual(read_json(self.layout.record), old)
        desktop.unlink()
        current = install(self.host, self.layout, pam_dir=self.pam)
        self.assertIn(str(desktop), current["files"])
        require_current_launcher(self.layout)

    def test_migration_does_not_adopt_a_missing_original_core_module(self):
        old = self.setup_account()
        path = self.layout.helper.parent / "vela_bench/model.py"
        path.unlink()
        del old["files"][str(path)]
        atomic_json(self.layout.record, old)
        with self.assertRaisesRegex(BenchError, "layout differs"):
            install(self.host, self.layout, pam_dir=self.pam)
        self.assertEqual(read_json(self.layout.record), old)
        self.assertFalse(path.exists())

    def test_previous_installation_upgrades_owned_supervision_and_new_modules_in_place(self):
        from vela_bench.discovery import file_hash
        record = self.setup_account()
        for name in ("conditions.py", "calibration.py", "qualification.py", "campaign.py"):
            path = self.layout.helper.parent / "vela_bench" / name
            path.unlink()
            del record["files"][str(path)]
        fault = self.layout.path("etc/systemd/system/vela-benchmark-qualification.service")
        fault.unlink()
        del record["files"][str(fault)]
        pilot = self.layout.path("etc/systemd/system") / PILOT_UNIT
        pilot.write_text(pilot.read_text().replace("TimeoutStopSec=300", "TimeoutStopSec=150"))
        record["files"][str(pilot)] = file_hash(pilot)
        atomic_json(self.layout.record, record)
        self.host.calls.clear()
        result = install(self.host, self.layout, pam_dir=self.pam)
        self.assertEqual(result["account_tag"], record["account_tag"])
        self.assertIn("TimeoutStopSec=300", pilot.read_text())
        self.assertIn("RuntimeMaxSec=90", fault.read_text())
        self.assertIn(["systemctl", "daemon-reload"], self.host.calls)
        self.assertFalse(any(command[0] in ("useradd", "userdel") for command in self.host.calls))

    def test_new_code_paths_are_not_adopted_and_interrupted_addition_can_resume(self):
        old = self.legacy_code_layout()
        path = self.layout.helper.parent / "vela_bench/diagnostics.py"
        path.write_text("# unowned file")
        with self.assertRaisesRegex(BenchError, "Unowned installed code path"):
            install(self.host, self.layout, pam_dir=self.pam)
        self.assertEqual(read_json(self.layout.record), old)
        path.unlink()
        replace = os.replace
        def interrupt(temporary, destination):
            result = replace(temporary, destination)
            if Path(destination) == path:
                raise OSError("interruption after new code publication")
            return result
        with patch("vela_bench.host.os.replace", side_effect=interrupt):
            with self.assertRaisesRegex(OSError, "interruption after"):
                install(self.host, self.layout, pam_dir=self.pam)
        record = install(self.host, self.layout, pam_dir=self.pam)
        self.assertEqual(record["status"], "ready")
        self.assertIn(str(path), record["files"])

    def test_probe_source_must_be_bounded_elf_and_is_never_executed_as_root(self):
        self.setup_account()
        path = self.layout.path("not-a-native-probe")
        path.write_text("#!/bin/sh\nexit 0\n")
        self.host.calls.clear()
        with self.assertRaisesRegex(BenchError, "ELF"):
            install(self.host, self.layout, pam_dir=self.pam, probe=path)
        self.assertEqual(self.host.calls, [])

    def test_code_update_rejects_changed_installed_files(self):
        self.setup_account()
        source = self.changed_source()
        (self.layout.helper.parent / "vela_bench/pilot.py").write_text("# unrecorded edit\n")
        with self.assertRaisesRegex(BenchError, "resource changed"):
            update_launcher(self.host, self.layout, source=source)

    def test_interrupted_code_update_can_resume_with_identical_source(self):
        self.setup_account()
        source = self.changed_source()
        replace = os.replace
        def interrupt(temporary, destination):
            if Path(destination).name == "pilot.py":
                raise OSError("synthetic interrupted replacement")
            return replace(temporary, destination)
        with patch("vela_bench.host.os.replace", side_effect=interrupt):
            with self.assertRaisesRegex(OSError, "interrupted"):
                update_launcher(self.host, self.layout, source=source)
        self.assertEqual(read_json(self.layout.record)["status"], "updating")
        record = install(self.host, self.layout, source=source, pam_dir=self.pam)
        self.assertEqual(record["status"], "ready")
        self.assertNotIn("code_update", record)

    def test_setup_prints_current_code_confirmation_and_supports_full_json(self):
        record = self.setup_account()
        with patch("vela_bench.cli.require_root"), patch("vela_bench.cli.personal_homes", return_value=[]), \
                patch("vela_bench.cli.install", return_value=record) as provision:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(main(["setup"]), 0)
            provision.assert_called_once()
            self.assertIn("Launcher code: up to date with this checkout", output.getvalue())
            self.assertIn("UID 1240", output.getvalue())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(main(["setup", "--json"]), 0)
            import json
            self.assertEqual(json.loads(output.getvalue()), record)

    def test_session_test_refuses_stale_launcher_before_execution_but_recover_remains_available(self):
        with patch("vela_bench.cli.require_root"), patch("vela_bench.cli.trusted"), \
                patch("vela_bench.cli.require_current_launcher", side_effect=BenchError("Run setup")) as current, \
                patch("vela_bench.cli.subprocess.call", return_value=0) as execute:
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                self.assertEqual(main(["session-test", "--target", "vela"]), 2)
            self.assertIn("Run setup", errors.getvalue())
            execute.assert_not_called()
            current.assert_called_once()
            current.reset_mock()
            self.assertEqual(main(["recover"]), 0)
            current.assert_not_called()
            execute.assert_called_once()

    def test_cli_forwards_probe_flag_and_exports_latest_diagnostic_report(self):
        from vela_bench.diagnostics import pilot_results
        import json
        data = pilot_results(self.layout.base, {"run_id": "a" * 32, "stage": "done", "result": "passed",
                                              "plan": {"targets": {"vela": {}}}})
        with patch("vela_bench.cli.require_root"), patch("vela_bench.cli.trusted"), \
                patch("vela_bench.cli.require_current_launcher"), \
                patch("vela_bench.cli.subprocess.call", return_value=0) as execute, \
                patch("vela_bench.cli.subprocess.run", return_value=SimpleNamespace(
                    stdout=json.dumps(data), stderr="", returncode=0)) as inspect, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["session-test", "--target", "vela", "--probe", "--output",
                                   str(self.layout.path("automatic-export"))]), 0)
            self.assertIn("--probe", execute.call_args.args[0])
            self.assertEqual(main(["pilot-report", "--output", str(self.layout.path("export"))]), 0)
            self.assertEqual(inspect.call_args.args[0][-1], "results")
            self.assertEqual(inspect.call_count, 2)
        path = self.layout.path("export/pilot-" + "a" * 32)
        self.assertTrue((path / "report.html").is_file())
        self.assertFalse(read_json(path / "summary.json")["diagnostics_complete"])
        self.assertTrue((self.layout.path("automatic-export/pilot-" + "a" * 32) / "report.html").is_file())

    def test_automatic_report_preserves_failure_and_never_exports_an_old_run_after_preflight_rejection(self):
        for status, probe in ((status, probe) for status in (0, 1, 2, 130) for probe in (False, True)):
            with self.subTest(status=status, probe=probe), patch("vela_bench.cli.require_root"), \
                    patch("vela_bench.cli.trusted"), patch("vela_bench.cli.require_current_launcher"), \
                    patch("vela_bench.cli.subprocess.call", return_value=status), \
                    patch("vela_bench.cli.export_latest_pilot") as export:
                self.assertEqual(main(["session-test"] + (["--probe"] if probe else [])), status)
                self.assertEqual(export.call_count, 1 if status in (0, 1) else 0)

    def test_strict_operator_umask_does_not_hide_code_or_nss_home_from_reserved_uid(self):
        previous = os.umask(0o077)
        try:
            self.setup_account()
        finally:
            os.umask(previous)
        for path in (self.layout.base, self.layout.helper.parent, self.layout.helper.parent / "vela_bench"):
            self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.layout.home.stat().st_mode & 0o777, 0o700)

    def test_existing_unowned_user_or_path_is_never_adopted(self):
        self.layout.base.mkdir()
        with self.assertRaisesRegex(BenchError, "Unowned setup path"):
            self.setup_account()
        self.assertEqual(self.host.calls, [])

    def test_partial_setup_stays_inspectable_and_is_not_overwritten(self):
        self.layout.base.mkdir()
        atomic_json(self.layout.record, {"status": "provisioning"})
        with self.assertRaisesRegex(BenchError, "Partial setup"):
            self.setup_account()
        self.assertEqual(self.host.calls, [])

    def test_failed_useradd_before_nss_changes_can_be_retried(self):
        self.host.fail_useradd = True
        with self.assertRaisesRegex(BenchError, "useradd configuration"):
            self.setup_account()
        pending = read_json(self.layout.record)
        self.assertIsNone(pending["uid"])
        self.assertIsNone(self.host.user())
        self.host.fail_useradd = False
        record = self.setup_account()
        self.assertEqual(record["status"], "ready")
        self.assertEqual(record["account_tag"], pending["account_tag"])

    def legacy_failed_bootstrap(self):
        self.layout.base.mkdir()
        record = {"schema": 1, "status": "provisioning", "account_tag": "Vela benchmark " + "b" * 32,
                  "account": ACCOUNT, "uid": None, "gid": None, "files": {}, "home": str(self.layout.home),
                  "private_homes": ["/home/operator"]}
        atomic_json(self.layout.record, record)
        return record

    def test_exact_legacy_bootstrap_from_unsupported_mail_key_is_resumed(self):
        old = self.legacy_failed_bootstrap()
        record = self.setup_account()
        self.assertEqual(record["status"], "ready")
        self.assertEqual(record["account_tag"], old["account_tag"])
        create = next(call for call in self.host.calls if call[0] == "useradd")
        self.assertIn(f"MAIL_DIR={self.layout.base / 'mail'}", create)
        self.assertNotIn("CREATE_MAIL_SPOOL=no", create)

    def test_partial_bootstrap_with_extra_artifacts_is_not_adopted(self):
        self.legacy_failed_bootstrap()
        (self.layout.base / "unexpected").touch()
        with self.assertRaisesRegex(BenchError, "Partial setup"):
            self.setup_account()
        self.assertEqual(self.host.calls, [])

    def test_partial_bootstrap_with_existing_group_is_not_adopted(self):
        self.legacy_failed_bootstrap()
        self.host.primary = SimpleNamespace(gr_gid=1240, gr_mem=[])
        with self.assertRaisesRegex(BenchError, "Partial setup"):
            self.setup_account()
        self.assertEqual(self.host.calls, [])

    def test_recorded_uid_prevents_automatic_account_recreation(self):
        old = self.legacy_failed_bootstrap()
        old["uid"] = 1240
        atomic_json(self.layout.record, old)
        with self.assertRaisesRegex(BenchError, "Partial setup"):
            self.setup_account()
        self.assertEqual(self.host.calls, [])

    def test_partial_bootstrap_with_symlink_mail_directory_is_rejected(self):
        self.legacy_failed_bootstrap()
        outside = self.layout.path("outside")
        outside.mkdir()
        (outside / "keep").touch()
        (self.layout.base / "mail").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(BenchError):
            self.setup_account()
        self.assertTrue((outside / "keep").exists())

    @unittest.skipUnless(shutil.which("useradd"), "shadow useradd unavailable")
    def test_real_useradd_accepts_options_before_rejecting_deliberately_invalid_name(self):
        # Prefix isolates the account databases as a second guard. The name
        # contains '/', which useradd rejects before opening account files.
        # This exercises the installed parser without creating any account.
        account_root = self.layout.path("parser-only")
        etc = account_root / "etc"
        etc.mkdir(parents=True)
        (etc / "login.defs").write_text("MAIL_DIR /var/mail\nUID_MIN 1000\nGID_MIN 1000\n")
        argv = useradd_command(Layout(), "Vela benchmark " + "c" * 32)
        argv[-1] = "vela-benchmark/invalid"
        result = subprocess.run([argv[0], "--prefix", str(account_root), *argv[1:]],
                                env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"},
                                text=True, capture_output=True, timeout=10)
        if "Cannot open audit interface" in result.stderr:
            self.skipTest("sandbox denies shadow audit socket; rerun parser validation outside sandbox")
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertIn("invalid user name", result.stderr.lower())
        self.assertFalse((etc / "passwd").exists())
        self.assertFalse((etc / "group").exists())

    def test_changed_account_identity_stops_uid_operations(self):
        record = self.setup_account()
        self.host.account.pw_uid = 0
        before = len(self.host.calls)
        with self.assertRaisesRegex(BenchError, "identity"):
            stop_user(self.host, record, self.layout)
        self.assertEqual(len(self.host.calls), before)

    def test_extra_group_or_unlocked_password_is_rejected(self):
        record = self.setup_account()
        self.host.extra_groups = [10]
        with self.assertRaises(BenchError):
            check_account(record, self.host, self.layout)
        self.host.extra_groups = []
        self.host.password_locked = False
        with self.assertRaises(BenchError):
            check_account(record, self.host, self.layout)

    def test_reset_recreates_nss_home_and_removes_prior_state(self):
        record = self.setup_account()
        (self.layout.home / "old-settings").write_text("must disappear")
        reset_profile(self.host, record, self.layout)
        self.assertEqual(list(self.layout.home.iterdir()), [])
        self.assertEqual(self.host.account.pw_dir, str(self.layout.home))
        self.assertEqual(self.layout.home.stat().st_mode & 0o777, 0o700)
        for name in (".X11-unix", ".ICE-unix"):
            path = self.layout.base / "tmp" / name
            self.assertEqual(path.stat().st_mode & 0o7777, 0o1777)
            self.assertEqual(list(path.iterdir()), [])

    def test_live_uid_or_runtime_prevents_profile_deletion(self):
        record = self.setup_account()
        marker = self.layout.home / "preserved"
        marker.touch()
        self.host.running = [123]
        with self.assertRaisesRegex(BenchError, "live benchmark"):
            reset_profile(self.host, record, self.layout)
        self.host.running = []
        self.layout.path("run/user/1240").mkdir(parents=True)
        with self.assertRaisesRegex(BenchError, "live benchmark"):
            reset_profile(self.host, record, self.layout)
        self.assertTrue(marker.exists())

    def test_profile_root_symlink_is_rejected_without_touching_destination(self):
        record = self.setup_account()
        self.layout.home.rmdir()
        destination = self.layout.path("personal")
        destination.mkdir()
        sentinel = destination / "keep"
        sentinel.touch()
        self.layout.home.symlink_to(destination, target_is_directory=True)
        with self.assertRaisesRegex(BenchError, "Unsafe profile root"):
            reset_profile(self.host, record, self.layout)
        self.assertTrue(sentinel.exists())

    def test_nested_symlinks_are_removed_without_traversal(self):
        record = self.setup_account()
        destination = self.layout.path("personal")
        destination.mkdir()
        (destination / "keep").touch()
        (self.layout.home / "link").symlink_to(destination, target_is_directory=True)
        reset_profile(self.host, record, self.layout)
        self.assertTrue((destination / "keep").exists())

    def test_uid_teardown_ends_manager_and_native_runtime(self):
        record = self.setup_account()
        stop_user(self.host, record, self.layout)
        self.assertIn(["systemctl", "stop", "user@1240.service", "user-runtime-dir@1240.service"], self.host.calls)

    def test_session_retirement_cannot_precede_process_and_runtime_teardown(self):
        record = self.setup_account()
        for live_process in (True, False):
            self.host.running = [123] if live_process else []
            if not live_process:
                self.layout.path("run/user/1240").mkdir(parents=True)
            with self.subTest(live_process=live_process), \
                    patch("vela_bench.host.time.monotonic", side_effect=[0, 11]), \
                    patch("vela_bench.host.release_session_unit") as release, \
                    self.assertRaisesRegex(BenchError, "still has processes/runtime"):
                stop_user(self.host, record, self.layout)
            release.assert_not_called()

    def test_unrelated_unit_cannot_be_stopped_by_its_name(self):
        record = self.setup_account()
        with patch.object(self.host, "run", return_value=SimpleNamespace(stdout="LoadState=loaded\nUser=root\nTransient=no\n")) as run:
            with self.assertRaisesRegex(BenchError, "ownership mismatch"):
                stop_user(self.host, record, self.layout)
        self.assertEqual(run.call_count, 1)

    def test_owned_transient_session_unit_is_stopped(self):
        record = self.setup_account()
        real = self.host.run
        def run(argv, **kwargs):
            if argv[:3] == ["systemctl", "show", SESSION_UNIT]:
                if ["systemctl", "stop", SESSION_UNIT] in self.host.calls:
                    return SimpleNamespace(stdout="LoadState=not-found\n")
                return SimpleNamespace(stdout="LoadState=loaded\nUser=vela-benchmark\nTransient=yes\nDescription=Vela benchmark P0 session "
                                       + record["account_tag"] + "\n")
            return real(argv, **kwargs)
        with patch.object(self.host, "run", side_effect=run):
            stop_user(self.host, record, self.layout)
        self.assertIn(["systemctl", "stop", SESSION_UNIT], self.host.calls)

    def test_failed_owned_session_is_reset_then_unloaded_before_next_start(self):
        record = self.setup_account()
        owned = ("LoadState=loaded\nUser=vela-benchmark\nTransient=yes\nDescription=Vela benchmark P0 session "
                 + record["account_tag"] + "\n")
        reply = lambda raw: SimpleNamespace(stdout=raw)
        with patch.object(self.host, "run", side_effect=[reply(owned + "ActiveState=failed\nResult=timeout\n"),
                reply(""), reply(owned + "ActiveState=inactive\nResult=success\n"), reply("LoadState=not-found\n")]) as run, \
                patch("vela_bench.host.time.sleep"):
            evidence = release_session_unit(self.host, record)
        self.assertTrue(evidence["unloaded"])
        self.assertTrue(evidence["reset_failed"])
        self.assertEqual(evidence["before_release"]["Result"], "timeout")
        self.assertEqual(sum(call.args[0] == ["systemctl", "reset-failed", SESSION_UNIT] for call in run.call_args_list), 1)

    def test_session_release_refuses_running_foreign_or_persistently_loaded_units(self):
        record = self.setup_account()
        owned = ("LoadState=loaded\nUser=vela-benchmark\nTransient=yes\nDescription=Vela benchmark P0 session "
                 + record["account_tag"] + "\n")
        for raw, error in ((owned + "ActiveState=active\n", "not stopped"),
                           ("LoadState=loaded\nUser=root\nTransient=no\nActiveState=failed\n", "ownership mismatch")):
            with patch.object(self.host, "run", return_value=SimpleNamespace(stdout=raw)) as run, \
                    self.assertRaisesRegex(BenchError, error):
                release_session_unit(self.host, record)
            self.assertEqual(run.call_count, 1)
        with patch.object(self.host, "run", return_value=SimpleNamespace(stdout=owned + "ActiveState=inactive\n")) as run, \
                patch("vela_bench.host.time.monotonic", side_effect=[0, 6]), \
                self.assertRaisesRegex(BenchError, "was not unloaded"):
            release_session_unit(self.host, record)
        self.assertEqual(run.call_count, 1)

    def test_session_command_has_pam_vt_and_the_same_manager_isolation(self):
        record = self.setup_account()
        argv = session_command("plasma", 8, record, self.layout)
        self.assertIn("--uid=vela-benchmark", argv)
        self.assertIn("--property=PAMName=vela-benchmark", argv)
        self.assertIn("--property=TTYPath=/dev/tty8", argv)
        self.assertIn("--property=StandardInput=tty-fail", argv)
        self.assertIn("--property=RemainAfterExit=yes", argv)
        self.assertNotIn("--collect", argv)
        self.assertEqual(argv[-2:], ["_session", "plasma"])
        self.assertNotIn("sh", argv)
        for key, value in isolation_properties(1240, self.layout, record["private_homes"]).items():
            self.assertIn(f"--property={key}={value}", argv)
        directory = self.pilot_state()
        logged = session_command("vela", 8, record, self.layout, directory=directory)
        self.assertIn(f"--property=StandardError=append:{directory}/vela-launch.stderr", logged)
        with self.assertRaisesRegex(BenchError, "evidence directory"):
            session_command("vela", 8, record, self.layout, directory=self.layout.home)

    def test_control_lock_rejects_concurrent_operation(self):
        self.setup_account()
        with control_lock(self.layout):
            with self.assertRaisesRegex(BenchError, "Another privileged"):
                control_lock(self.layout)

    def test_recovery_restores_manager_then_returns_to_original_tty(self):
        self.setup_account()
        directory = self.pilot_state()
        self.recover_without_fixture_io()
        self.assertEqual(read_json(directory / "state.json")["stage"], "done")
        start = self.host.calls.index(["systemctl", "start", "display-manager.service"])
        switch = self.host.calls.index(["chvt", "2"])
        self.assertLess(start, switch)
        before = list(self.host.calls)
        self.recover_without_fixture_io()
        self.assertEqual(self.host.calls, before)

    def test_reserved_session_log_is_preserved_before_profile_reset(self):
        record = self.setup_account()
        directory = self.pilot_state(result="failed")
        state = read_json(directory / "state.json")
        state["target"] = "vela"
        atomic_json(directory / "state.json", state)
        log = self.layout.home / ".local/state/vela/vela.log"
        log.parent.mkdir(parents=True)
        log.write_text("Vela startup diagnostic\n")
        # File ownership uses the real test UID; account/PAM remains a fixture.
        def collect(directory, state, record, layout):
            return preserve_session_log(directory, state, dict(record, uid=os.getuid()), layout)
        with patch("vela_bench.pilot.preserve_session_log", side_effect=collect):
            self.recover_without_fixture_io()
        saved = read_json(directory / "vela-session-log.json")
        self.assertEqual(saved["text"], "Vela startup diagnostic\n")
        self.assertFalse(log.exists())

    def test_reserved_log_cannot_follow_parent_links_or_read_special_or_hardlinked_files(self):
        record = dict(self.setup_account(), uid=os.getuid())
        directory = self.pilot_state(result="failed")
        artifact = directory / "vela-session-log.json"
        outside = self.layout.base / "outside"
        (outside / "state/vela").mkdir(parents=True)
        (outside / "state/vela/vela.log").write_text("must not be read\n")
        (self.layout.home / ".local").symlink_to(outside, target_is_directory=True)
        preserve_session_log(directory, {"target": "vela"}, record, self.layout)
        self.assertNotIn("text", read_json(artifact))
        (self.layout.home / ".local").unlink()
        parent = self.layout.home / ".local/state/vela"
        parent.mkdir(parents=True)
        for kind in ("hardlink", "fifo"):
            artifact.unlink()
            log = parent / "vela.log"
            if kind == "hardlink":
                os.link(outside / "state/vela/vela.log", log)
            else:
                os.mkfifo(log)
            preserve_session_log(directory, {"target": "vela"}, record, self.layout)
            self.assertNotIn("text", read_json(artifact))
            log.unlink()

    def test_readiness_stops_waiting_when_desktop_has_already_exited(self):
        self.setup_account()
        directory = self.pilot_state()
        with patch("vela_bench.pilot.verify_session", side_effect=BenchError("no login session")), \
                patch.object(self.host, "run", return_value=SimpleNamespace(stdout="LoadState=not-found\n")), \
                patch("vela_bench.pilot.time.sleep") as sleep:
            with self.assertRaisesRegex(BenchError, "exited before readiness"):
                wait_session(self.host, {}, {"target": "vela"}, directory, self.layout)
        sleep.assert_not_called()
        self.assertEqual(read_json(directory / "vela-session-exit.json")["readiness_error"], "no login session")
        self.assertFalse(read_json(directory / "vela-session-exit.json")["exit_status_available"])

    def test_retained_clean_signal_exit_is_detected_even_when_unit_is_active(self):
        self.setup_account()
        directory = self.pilot_state()
        with patch("vela_bench.pilot.verify_session", side_effect=BenchError("no login session")), \
                patch.object(self.host, "run", return_value=SimpleNamespace(stdout=
                    "LoadState=loaded\nActiveState=active\nSubState=exited\nExecMainCode=2\nExecMainStatus=1\nResult=success\n")), \
                patch("vela_bench.pilot.time.sleep") as sleep:
            with self.assertRaisesRegex(BenchError, "code=2, status=1"):
                wait_session(self.host, {}, {"target": "vela"}, directory, self.layout)
        sleep.assert_not_called()
        self.assertTrue(read_json(directory / "vela-session-exit.json")["exit_status_available"])

    def test_initially_stopped_manager_is_not_started(self):
        self.setup_account()
        directory = self.pilot_state(active_manager=False)
        self.recover_without_fixture_io()
        self.assertEqual(read_json(directory / "state.json")["stage"], "done")
        self.assertNotIn(["systemctl", "start", "display-manager.service"], self.host.calls)

    def test_failed_restoration_never_publishes_success(self):
        self.setup_account()
        directory = self.pilot_state()
        self.host.fail_start = True
        with self.assertRaisesRegex(BenchError, "restoration failed"):
            self.recover_without_fixture_io()
        state = read_json(directory / "state.json")
        self.assertEqual(state["stage"], "cleanup-failed")
        self.assertTrue(state["cleanup_errors"])

    def test_recovery_resets_only_the_counter_of_a_clean_stopped_rate_limited_manager_once(self):
        self.setup_account()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        reply = lambda raw="": SimpleNamespace(stdout=raw, returncode=0, stderr="")
        responses = [reply("ActiveState=inactive\nResult=success\n"), BenchError("start rejected"),
                     reply("ActiveState=failed\nResult=start-limit-hit\n"), reply(), reply()]
        with patch.object(self.host, "run", side_effect=responses) as run:
            restore_display_manager(self.host, directory, state)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands.count(["systemctl", "start", "display-manager.service"]), 2)
        self.assertEqual(commands.count(["systemctl", "reset-failed", "display-manager.service"]), 1)
        self.assertTrue(read_json(directory / "state.json")["display_manager_rate_limit_reset"])
        self.assertEqual(read_json(directory / "display-manager-recovery.json")["status"], "passed")

    def test_rejecting_greeter_is_restarted_once_and_both_restore_attempts_are_preserved(self):
        record = self.setup_account()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        state["conditions_snapshot"] = {"fixture": "exact-original-layout"}
        rejected = {"status": "failed", "components": {"outputs": "failed"},
                    "errors": ["native SetConfig rejected"], "output_restart_eligible": True}
        passed = {"status": "passed", "components": {"outputs": "passed"}, "errors": []}
        with patch("vela_bench.pilot.restore_conditions", side_effect=[rejected, passed]) as restore, \
                patch("vela_bench.pilot.logind_sessions", return_value=[]), \
                patch("vela_bench.pilot.restore_display_manager") as manager:
            result = restore_hardware_conditions(self.host, self.layout, directory, state, record)
        self.assertEqual(result, passed)
        self.assertEqual(restore.call_count, 2)
        manager.assert_called_once()
        self.assertEqual(self.host.calls.count(["systemctl", "stop", "display-manager.service"]), 1)
        self.assertTrue(read_json(directory / "state.json")["output_greeter_restart"])
        evidence = read_json(directory / "conditions-recovery.json")
        self.assertEqual(evidence["attempts"], [rejected, passed])
        # The same rejection after a process interruption cannot loop.
        self.host.calls.clear()
        with patch("vela_bench.pilot.restore_conditions", return_value=rejected) as restore:
            result = restore_hardware_conditions(self.host, self.layout, directory, state, record)
        restore.assert_called_once()
        self.assertEqual(result["status"], "failed")
        self.assertNotIn(["systemctl", "stop", "display-manager.service"], self.host.calls)
        evidence = read_json(directory / "conditions-recovery.json")
        self.assertTrue(evidence["greeter_restart"])
        self.assertEqual(evidence["attempts"][:2], [rejected, passed])
        self.assertEqual(evidence["attempt_count"], 3)

    def test_greeter_restart_refuses_personal_sessions_live_reserved_uid_and_other_failures(self):
        record = self.setup_account()
        directory = self.pilot_state()
        baseline = dict(read_json(directory / "state.json"), conditions_snapshot={})
        for case in ("personal", "live-benchmark", "hotplug", "second-rejection", "restart-error"):
            with self.subTest(case=case):
                state = dict(baseline)
                self.host.calls.clear()
                self.host.running = [77] if case == "live-benchmark" else []
                sessions_ = [{"Class": "user", "Type": "wayland", "State": "online"}] if case == "personal" else []
                rejected = {"status": "failed", "errors": ["refused"], "output_restart_eligible": case != "hotplug"}
                with patch("vela_bench.pilot.restore_conditions", return_value=rejected) as restore, \
                        patch("vela_bench.pilot.logind_sessions", return_value=sessions_), \
                        patch("vela_bench.pilot.restore_display_manager", side_effect=BenchError("start failed")
                              if case == "restart-error" else None):
                    result = restore_hardware_conditions(self.host, self.layout, directory, state, record)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(restore.call_count, 2 if case == "second-rejection" else 1)
                self.assertEqual(["systemctl", "stop", "display-manager.service"] in self.host.calls,
                                 case in ("second-rejection", "restart-error"))
        self.host.running = []

    def test_recovery_does_not_reset_existing_failures_or_unrelated_errors_or_loop_on_failure(self):
        self.setup_account()
        directory = self.pilot_state()
        for before, result, already_reset in (("failed", "start-limit-hit", False),
                                              ("inactive", "exit-code", False),
                                              ("inactive", "start-limit-hit", True)):
            state = read_json(directory / "state.json")
            state["display_manager_rate_limit_reset"] = already_reset
            reply = lambda raw: SimpleNamespace(stdout=raw, returncode=0, stderr="")
            responses = [reply(f"ActiveState={before}\nResult=success\n"), BenchError("start failed"),
                         reply(f"ActiveState=failed\nResult={result}\n")]
            with self.subTest(before=before, result=result, already_reset=already_reset), \
                    patch.object(self.host, "run", side_effect=responses) as run, \
                    self.assertRaisesRegex(BenchError, "start failed"):
                restore_display_manager(self.host, directory, state)
            self.assertFalse(any(call.args[0][:2] == ["systemctl", "reset-failed"] for call in run.call_args_list))
        state["display_manager_rate_limit_reset"] = False
        responses = [reply("ActiveState=inactive\nResult=success\n"), BenchError("limit"),
                     reply("ActiveState=failed\nResult=start-limit-hit\n"), reply(""), BenchError("still broken")]
        with patch.object(self.host, "run", side_effect=responses) as run, self.assertRaisesRegex(BenchError, "still broken"):
            restore_display_manager(self.host, directory, state)
        self.assertEqual(sum(call.args[0][:2] == ["systemctl", "reset-failed"] for call in run.call_args_list), 1)
        self.assertEqual(read_json(directory / "display-manager-recovery.json")["status"], "failed")

    def test_killed_worker_pending_result_still_requires_cleanup(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        self.recover_without_fixture_io()
        self.assertEqual(read_json(directory / "state.json")["stage"], "failed")

    def test_uninstall_keeps_evidence_and_removes_only_recorded_files(self):
        record = self.setup_account()
        evidence = self.layout.base / "pilots" / "keep.json"
        evidence.write_text("{}")
        uninstall(self.host, self.layout)
        self.assertTrue(evidence.exists())
        self.assertIsNone(self.host.user())
        self.assertFalse(self.layout.helper.exists())
        self.assertEqual(read_json(self.layout.record)["status"], "uninstalled")
        self.assertTrue(all(not Path(path).exists() for path in record["files"]))

    def test_untracked_launcher_file_prevents_account_removal(self):
        self.setup_account()
        (self.layout.helper.parent / "untracked").touch()
        with self.assertRaisesRegex(BenchError, "Untracked installed"):
            uninstall(self.host, self.layout)
        self.assertIsNotNone(self.host.user())

    def test_worker_prepares_and_tears_down_each_target_independently(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", plan={"targets": {"vela": {"identity": 1}, "plasma": {"identity": 2}},
                     "target_order": ["vela", "plasma"], "seconds_per_target": 0, "presentation_probe": True})
        atomic_json(directory / "state.json", state)
        calls = []
        probes = []
        def probe(host, record, directory, target, scenario, layout):
            probes.append((target, scenario))
            return {"status": "complete" if target == "vela" else "failed", "error": "unsupported protocol"}
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
             patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", return_value=[
                 {"Id": "2", "Type": "wayland", "Class": "user", "State": "closing", "Active": "no"},
                 {"Id": "c1", "Type": "wayland", "Class": "greeter", "State": "online", "Active": "no"}]), \
             patch("vela_bench.pilot.reserve_vt"), \
             patch("vela_bench.pilot.resolve_targets", side_effect=lambda names: {names[0]: state["plan"]["targets"][names[0]]}), \
             patch("vela_bench.pilot.stop_user", side_effect=lambda *args: calls.append("stop")), \
             patch("vela_bench.pilot.reset_profile", side_effect=lambda *args: calls.append("reset")), \
             patch("vela_bench.pilot.run_probe", side_effect=probe), \
             patch("vela_bench.pilot.verify_session", side_effect=lambda *args: calls.append("verify") or {"verified": True}):
            worker(self.host, self.layout)
        self.assertEqual(probes, [("vela", "baseline"), ("vela", "client-delay"),
                                  ("plasma", "baseline"), ("plasma", "client-delay")])
        self.assertEqual([item for item in calls if item != "verify"], ["stop", "reset", "stop", "stop", "reset", "stop"])
        final = read_json(directory / "state.json")
        self.assertEqual(final["stage"], "restoring")
        self.assertEqual(final["result"], "failed")
        self.assertEqual(final["lifecycle_result"], "passed")
        self.assertIn("unsupported protocol", final["error"])
        self.assertTrue((directory / "vela-session.json").exists())
        self.assertTrue((directory / "plasma-session.json").exists())

    def test_worker_launch_failure_is_journaled_for_stop_post_recovery(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", plan={"targets": {"vela": {"identity": 1}}, "seconds_per_target": 0})
        atomic_json(directory / "state.json", state)
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
             patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", return_value=[]), \
             patch("vela_bench.pilot.reserve_vt"), \
             patch("vela_bench.pilot.resolve_targets", return_value={"vela": {"identity": 1}}), \
             patch("vela_bench.pilot.stop_user"), \
             patch("vela_bench.pilot.reset_profile", side_effect=BenchError("synthetic failure")):
            with self.assertRaisesRegex(BenchError, "synthetic failure"):
                worker(self.host, self.layout)
        final = read_json(directory / "state.json")
        self.assertEqual(final["stage"], "restoring")
        self.assertEqual(final["result"], "failed")
        self.assertIn("synthetic failure", final["error"])

    def test_controlled_snapshot_is_supervised_and_saved_before_stopping_the_greeter(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", plan={"targets": {"vela": {"identity": 1}}, "controlled_conditions": True})
        atomic_json(directory / "state.json", state)
        snapshot = {"fixture": "original active greeter hardware"}
        def acquire(*args, **kwargs):
            saved = read_json(directory / "state.json")
            self.assertTrue(saved["restore_required"])
            self.assertEqual(saved["stage"], "snapshot")
            self.assertTrue(kwargs["activate"])
            self.assertNotIn(["systemctl", "stop", "display-manager.service"], self.host.calls)
            return snapshot
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
                patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", return_value=[]), \
                patch("vela_bench.pilot.snapshot_conditions", side_effect=acquire), patch("vela_bench.pilot.reserve_vt"), \
                patch("vela_bench.pilot.resolve_targets", return_value={"vela": {"identity": 1}}), \
                patch("vela_bench.pilot.stop_user"), patch("vela_bench.pilot.reset_profile", side_effect=BenchError("stop before desktop")):
            with self.assertRaisesRegex(BenchError, "stop before desktop"):
                worker(self.host, self.layout)
        self.assertEqual(read_json(directory / "state.json")["conditions_snapshot"], snapshot)
        self.assertIn(["systemctl", "stop", "display-manager.service"], self.host.calls)

    def test_snapshot_failure_keeps_greeter_running_and_restores_originating_vt(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", plan={"controlled_conditions": True})
        atomic_json(directory / "state.json", state)
        error = BenchError("output announcement deadline")
        error.__cause__ = HostOperationError(["/usr/bin/ddcutil", "getvcp", "10"], 1, "monitor not ready on stdout", "")
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
                patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", return_value=[]), \
                patch("vela_bench.pilot.snapshot_conditions", side_effect=error):
            with self.assertRaisesRegex(BenchError, "announcement deadline"):
                worker(self.host, self.layout)
        diagnostic = read_json(directory / "failed-operation.json")
        self.assertEqual(diagnostic["returncode"], 1)
        self.assertEqual(diagnostic["stdout"], "monitor not ready on stdout")
        self.assertNotIn(["systemctl", "stop", "display-manager.service"], self.host.calls)
        self.assertTrue(read_json(directory / "state.json")["restore_required"])
        self.recover_without_fixture_io()
        self.assertIn(["chvt", "2"], self.host.calls)
        self.assertEqual(read_json(directory / "state.json")["stage"], "failed")

    def test_login_during_snapshot_refuses_takeover_without_replaying_hardware_settings(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", conditions_restore_required=False, plan={"controlled_conditions": True})
        atomic_json(directory / "state.json", state)
        login = {"Id": "new", "Type": "wayland", "Class": "user", "State": "online", "Active": "yes"}
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
                patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", side_effect=[[], [], [login]]), \
                patch("vela_bench.pilot.snapshot_conditions", return_value={"fixture": "read-only snapshot"}):
            with self.assertRaisesRegex(BenchError, "appeared after authorization"):
                worker(self.host, self.layout)
        self.assertNotIn(["systemctl", "stop", "display-manager.service"], self.host.calls)
        with patch("vela_bench.pilot.restore_conditions") as restore:
            self.recover_without_fixture_io()
        restore.assert_not_called()
        self.assertIn(["chvt", "2"], self.host.calls)

    def test_crash_fixture_targets_its_worker_and_persists_intent_before_the_signal(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        state = read_json(directory / "state.json")
        state.update(stage="queued", fault="crash", plan={"targets": {"vela": {"identity": 1}}, "seconds_per_target": 0})
        atomic_json(directory / "state.json", state)
        observed = []
        def signal_worker(pid, signum):
            observed.append((pid, signum, read_json(directory / "state.json")["fault_injected"]))
        with patch("vela_bench.pilot.load_setup", return_value=read_json(self.layout.record)), \
                patch("vela_bench.pilot.create_fixture"), patch("vela_bench.pilot.logind_sessions", return_value=[]), \
                patch("vela_bench.pilot.reserve_vt"), patch("vela_bench.pilot.stop_user"), \
                patch("vela_bench.pilot.reset_profile"), patch("vela_bench.pilot.verify_session", return_value={}), \
                patch("vela_bench.pilot.prepare_fault_input", return_value=(None, "/sys/devices/virtual/input/input999999")), \
                patch("vela_bench.pilot.resolve_targets", return_value={"vela": {"identity": 1}}), \
                patch("vela_bench.pilot.os.kill", side_effect=signal_worker):
            with self.assertRaisesRegex(BenchError, "unexpectedly returned"):
                worker(self.host, self.layout)
        self.assertEqual(observed, [(os.getpid(), signal.SIGKILL, "crash")])

    def test_failed_hardware_restore_prevents_success_even_when_worker_passed(self):
        self.setup_account()
        directory = self.pilot_state(result="passed")
        state = read_json(directory / "state.json")
        state["conditions_snapshot"] = {"fixture": True}
        atomic_json(directory / "state.json", state)
        with patch("vela_bench.pilot.restore_conditions", return_value={"status": "failed", "errors": ["brightness mismatch"]}):
            with self.assertRaisesRegex(BenchError, "brightness mismatch"):
                self.recover_without_fixture_io()
        self.assertEqual(read_json(directory / "state.json")["stage"], "cleanup-failed")

    def test_hang_fixture_checks_all_uid_fields_and_signals_only_opened_pidfds(self):
        from vela_bench.pilot import freeze_reserved_uid
        host = SimpleNamespace(pids=lambda uid: [101, 102, 103])
        with patch("vela_bench.pilot.os.pidfd_open", side_effect=[11, 12, ProcessLookupError()]), \
                patch("vela_bench.pilot.Path.read_text", side_effect=["Uid:\t1240\t1240\t1240\t1240\n", "Uid:\t1240\t1000\t1240\t1240\n"]), \
                patch("vela_bench.pilot.signal.pidfd_send_signal") as send, \
                patch("vela_bench.pilot.os.close") as close:
            freeze_reserved_uid(host, 1240)
        send.assert_called_once_with(11, signal.SIGSTOP)
        self.assertEqual([call.args[0] for call in close.call_args_list], [11, 12])

    def test_interrupted_worker_records_pid1_outcome_and_never_keeps_pending_result(self):
        self.setup_account()
        directory = self.pilot_state(result="pending")
        with patch.dict(os.environ, {"SERVICE_RESULT": "signal", "EXIT_CODE": "killed", "EXIT_STATUS": "KILL"}):
            self.recover_without_fixture_io()
        state = read_json(directory / "state.json")
        self.assertEqual(state["result"], "failed")
        self.assertEqual(state["stage"], "failed")
        self.assertEqual(state["supervision"]["SERVICE_RESULT"], "signal")

    def test_vt_reservation_preserves_existing_masks(self):
        self.setup_account()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        state["masked_units"] = []
        parent=self.layout.path('etc/systemd/system');parent.mkdir(parents=True,exist_ok=True)
        (parent/'getty@tty8.service').symlink_to('/dev/null')
        calls=[]
        def invoke(argv,**kwargs):
            calls.append(argv)
            if argv[1]=='mask':
                path=self.layout.path('run/systemd/system')/argv[-1]
                path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to('/dev/null')
            return SimpleNamespace(stdout='ActiveState=inactive\nLoadState=masked\nUnitFileState=masked\n')
        with patch.object(self.host, "run", side_effect=invoke):
            reserve_vt(self.host, directory, state,self.layout)
        self.assertEqual(state["masked_units"], ["autovt@tty8.service"])
        self.assertEqual([c for c in calls if c[1]=='mask'],[["systemctl", "mask", "--runtime", "autovt@tty8.service"]])

    def test_active_getty_prevents_tty_takeover(self):
        self.setup_account()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        state["masked_units"] = []
        with patch.object(self.host, "run", return_value=SimpleNamespace(stdout="ActiveState=active\n")) as run:
            with self.assertRaisesRegex(BenchError, "VT is occupied"):
                reserve_vt(self.host, directory, state,self.layout)
        self.assertEqual(run.call_count, 1)

    def test_recovery_does_not_restore_an_old_boot_display_state(self):
        self.setup_account()
        directory = self.pilot_state()
        state = read_json(directory / "state.json")
        state.update(boot_id="old-boot", masked_units=["autovt@tty8.service"])
        atomic_json(directory / "state.json", state)
        self.recover_without_fixture_io()
        self.assertNotIn(["systemctl", "start", "display-manager.service"], self.host.calls)
        self.assertNotIn(["chvt", "2"], self.host.calls)
        self.assertEqual(read_json(directory / "state.json")["stage"], "failed")


class Guards(unittest.TestCase):
    def test_user_manager_may_use_default_xdg_paths_but_not_a_foreign_home(self):
        home, runtime = "/var/lib/vela-benchmark/home", "/run/user/1240"
        check_profile_environment({"HOME": home, "XDG_RUNTIME_DIR": runtime}, home, runtime)
        with self.assertRaisesRegex(BenchError, "Unexpected HOME"):
            check_profile_environment({"HOME": "/personal", "XDG_RUNTIME_DIR": runtime}, home, runtime)

    def test_foreign_user_manager_bus_or_config_is_rejected(self):
        home, runtime = "/var/lib/vela-benchmark/home", "/run/user/1240"
        for key in ("DBUS_SESSION_BUS_ADDRESS", "XDG_CONFIG_HOME"):
            with self.subTest(key=key), self.assertRaisesRegex(BenchError, key):
                check_profile_environment({"HOME": home, "XDG_RUNTIME_DIR": runtime, key: "/personal"}, home, runtime)

    def test_logind_manager_session_does_not_count_as_a_second_desktop(self):
        desktop = {"User": "1240", "Class": "user", "Type": "wayland"}
        manager = {"User": "1240", "Class": "manager", "Type": "unspecified"}
        other = {"User": "1000", "Class": "user", "Type": "tty"}
        self.assertEqual(select_login_session([desktop, manager, other], 1240), (desktop, [manager]))

    def test_additional_reserved_uid_login_is_rejected(self):
        sessions = [{"User": "1240", "Class": "user", "Type": "wayland"},
                    {"User": "1240", "Class": "user", "Type": "tty"}]
        with self.assertRaisesRegex(BenchError, "exactly one"):
            select_login_session(sessions, 1240)

    def test_desktop_environment_does_not_inherit_operator_endpoints(self):
        user = SimpleNamespace(pw_uid=1240, pw_name=ACCOUNT, pw_dir="/var/lib/vela-benchmark/home", pw_shell="/bin/sh")
        inherited = {"XDG_SESSION_ID": "c12", "XDG_SEAT": "seat0", "XDG_VTNR": "8", "HOME": "/personal",
                     "WAYLAND_DISPLAY": "personal", "DISPLAY": ":0", "DBUS_SESSION_BUS_ADDRESS": "personal",
                     "SSH_AUTH_SOCK": "personal", "PYTHONPATH": "personal", "LD_PRELOAD": "personal"}
        with patch("vela_bench.pilot.os.getuid", return_value=1240), patch("vela_bench.pilot.os.geteuid", return_value=1240):
            env = desktop_environment("vela", user, inherited)
        self.assertEqual(env["HOME"], user.pw_dir)
        self.assertEqual(env["XDG_RUNTIME_DIR"], "/run/user/1240")
        for key in ("WAYLAND_DISPLAY", "DISPLAY", "SSH_AUTH_SOCK", "PYTHONPATH", "LD_PRELOAD"):
            self.assertNotIn(key, env)
        self.assertNotEqual(env["DBUS_SESSION_BUS_ADDRESS"], "personal")

    def test_root_cannot_execute_desktop(self):
        user = SimpleNamespace(pw_uid=1240, pw_name=ACCOUNT)
        with patch("vela_bench.pilot.os.getuid", return_value=0), self.assertRaisesRegex(BenchError, "unprivileged"):
            desktop_environment("vela", user, {})

    def test_unsafe_private_home_layout_and_control_characters_rejected(self):
        for path in ("/", "/var", "relative"):
            with self.subTest(path=path), self.assertRaises(BenchError):
                isolation_properties(1240, Layout(), [path])
        with self.assertRaises(BenchError):
            unit_quote("/home/unsafe\nExecStart=/bin/false")
        self.assertEqual(unit_quote("/srv/100% space"), '"/srv/100%% space"')

    def test_privileged_paths_reject_writable_files(self):
        with patch.object(Path, "lstat", return_value=SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG | 0o666)):
            with self.assertRaisesRegex(BenchError, "Untrusted privileged path"):
                trusted("/usr/local/libexec/example")

    def test_unsupported_pam_stack_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(BenchError, "system-login"):
                pam_policy(Path(temp))

    def test_setup_dry_run_does_not_require_root_or_mutate(self):
        with contextlib.redirect_stdout(io.StringIO()), patch("vela_bench.cli.install", side_effect=AssertionError("mutation")):
            self.assertEqual(main(["setup", "--dry-run"]), 0)

    def test_live_setup_without_root_refused(self):
        with patch("vela_bench.host.os.geteuid", return_value=1000), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(["setup"]), 2)


if __name__ == "__main__":
    unittest.main()
