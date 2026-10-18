# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import contextlib
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import pty
import select
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

from vela_report import cli, collectors
from vela_report.collectors import Collector, discover, process_identity, query_state, resource_sample
from vela_report.core import Environment, Redactor, Report, Result, read_regular, run_command


class ReportingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.proc = self.root / "proc"
        self.proc.mkdir()
        self.etc = self.root / "etc"
        self.etc.mkdir()
        self.environment = Environment(proc=self.proc, etc=self.etc, home=self.home, env={
            "XDG_CURRENT_DESKTOP": "OtherDesktop", "VELA_DEBUG_SYNC": "1",
            "UNRELATED_SECRET": "do-not-collect-this",
        })
        self.report = Report(self.environment, {"commit": "known-test-build"})
        self.addCleanup(self.temporary.cleanup)
        self.addCleanup(self.report.close)

    def fake_process(self, pid=123, start=100, supervisor=False):
        directory = self.proc / str(pid)
        directory.mkdir(exist_ok=True)
        fields = ["0"] * 23
        fields[0], fields[11], fields[12], fields[19] = "S", "50", "10", str(start)
        (directory / "stat").write_text(f"{pid} (name with spaces and a ) bracket) " + " ".join(fields))
        if not (directory / "exe").is_symlink():
            (directory / "exe").symlink_to("/usr/bin/vela-compositor")
        (directory / "cmdline").write_bytes(b"vela-compositor\0" + (b"--supervise\0" if supervisor else b""))
        (directory / "status").write_text("VmRSS:\t100 kB\nThreads:\t2\nPrivateField: exclude\n")
        (directory / "limits").write_text("Max open files            1024                 4096                 files\n")
        fd = directory / "fd"
        fd.mkdir(exist_ok=True)
        targets = ["anon_inode:[sync_file]", "socket:[123]", "pipe:[456]",
                   str(self.home / "secret-document (deleted)"), "anon_inode:[eventfd]"]
        for index, target in enumerate(targets):
            if not (fd / str(index)).is_symlink():
                (fd / str(index)).symlink_to(target)
        return process_identity(directory)

    def export(self):
        target = self.root / "report.zip"
        self.report.export(target)
        return zipfile.ZipFile(target)

    def test_redaction_preserves_errors_timestamps_and_valid_json(self):
        redactor = Redactor(home="/home/alice", user="alice", host="private-host")
        source = '2026-10-08T13:20:00+02:00 errno=24 /home/alice alice private-host token=abcd\n'
        result = redactor.text(source)
        self.assertNotIn("abcd", result)
        self.assertNotIn("alice", result)
        self.assertIn("errno=24", result)
        self.assertIn("2026-10-08T13:20:00+02:00", result)
        text = redactor.text('{"access_token": "private", "error": 24}')
        self.assertEqual(json.loads(text), {"access_token": "<redacted>", "error": 24})
        self.assertEqual(redactor.value({"token": ["a", "b"]})["token"], "<redacted>")
        self.assertNotIn("password", redactor.text("https://alice:password@example.com/log"))

    def test_zip_integrity_permissions_and_original_unchanged(self):
        source = self.home / "source.log"
        source.write_text(f"errno=24 {self.home}/document\npassword=private\n")
        original = source.read_bytes()
        Collector(self.report).file("logs", source, "logs/vela.log")
        self.report.results["logs"] = {"status": "collected"}
        with self.export() as archive:
            self.assertIsNone(archive.testzip())
            manifest = json.loads(archive.read("manifest.json"))
            data = archive.read("logs/vela.log")
            self.assertEqual(hashlib.sha256(data).hexdigest(),
                             manifest["artifacts"]["logs/vela.log"]["sha256"])
            self.assertNotIn(b"private", data)
            self.assertNotIn(str(self.home).encode(), archive.read("manifest.json"))
            self.assertIn(b"errno=24", data)
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(stat.S_IMODE(self.report.directory.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((self.root / "report.zip").stat().st_mode), 0o600)

    def test_review_exclusion_removes_all_content_and_provenance(self):
        self.report.add_text("logs", "logs/a.txt", "unique-incident-secret", source="unique-private-source")
        self.report.add_text("logs", "logs/b.txt", "also-excluded")
        self.report.remove_collection("logs")
        self.assertFalse((self.report.directory / "logs/a.txt").exists())
        with self.export() as archive:
            self.assertEqual(set(archive.namelist()), {"report.md", "manifest.json"})
            data = b"".join(archive.read(name) for name in archive.namelist())
            self.assertNotIn(b"unique-", data)
            self.assertNotIn(b"also-excluded", data)
            self.assertEqual(json.loads(archive.read("manifest.json"))["collectors"]["logs"]["status"], "excluded")

    def test_excluding_resources_removes_derived_observations(self):
        identity = self.fake_process()
        self.report.context = {"processes": [identity], "duration": 1}
        with patch("vela_report.collectors.time.sleep"):
            Collector(self.report).resources(lambda _: None)
        self.report.remove_collection("resources")
        with self.export() as archive:
            self.assertNotIn(b"descriptors changed", archive.read("report.md"))
            self.assertEqual(json.loads(archive.read("manifest.json"))["observations"], [])

    def test_attachment_names_never_overwrite_after_removal(self):
        source = self.root / "private.first.name.png"
        source.write_bytes(b"first")
        first = self.report.attach(source, "screenshot")
        source.write_bytes(b"second")
        second = self.report.attach(source, "screenshot")
        self.report.remove(first)
        source.write_bytes(b"third")
        third = self.report.attach(source, "screenshot")
        self.assertNotEqual(second, third)
        self.assertEqual((self.report.directory / second).read_bytes(), b"second")
        self.assertTrue(self.report.artifacts[third]["sensitive"])
        self.assertTrue(third.endswith(".png"))
        self.assertNotIn("private", third)
        self.assertNotIn(".first", third)

    def test_symlinks_fifo_and_unsafe_names_refused(self):
        original = self.root / "source"
        original.write_text("secret")
        link = self.root / "link"
        link.symlink_to(original)
        with self.assertRaises(ValueError):
            self.report.attach(link)
        with self.assertRaises(OSError):
            read_regular(link)
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            read_regular(fifo)
        for path in ("../escape", "/tmp/escape"):
            with self.assertRaises(ValueError):
                self.report.add_text("logs", path, "test")
        with self.assertRaises(ValueError):
            self.report.attach(original, "../unexpected")

    def test_text_truncation_and_binary_attachment_limits(self):
        source = self.root / "large"
        source.write_bytes(b"0123456789")
        value, metadata = read_regular(source, limit=4)
        self.assertEqual(value, "0123")
        self.assertTrue(metadata["truncated"])
        with patch("vela_report.core.ATTACHMENT_LIMIT", 4):
            with self.assertRaises(ValueError):
                self.report.attach(source, "core")
        self.assertFalse(self.report.artifacts)

    def test_existing_zip_and_export_failure_leave_no_partial(self):
        target = self.root / "report.zip"
        target.write_bytes(b"existing")
        with self.assertRaises(FileExistsError):
            self.report.export(target)
        self.assertEqual(target.read_bytes(), b"existing")
        with patch("vela_report.core.zipfile.ZipFile.writestr", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.report.export(self.root / "other.zip")
        self.assertFalse(list(self.root.glob(".vela-report-*")))
        self.assertFalse((self.root / "other.zip").exists())
        self.report.export(target, overwrite=True)
        self.assertTrue(zipfile.is_zipfile(target))

    def test_no_clobber_when_destination_appears_during_export(self):
        target = self.root / "raced.zip"
        real_link = os.link

        def competing_link(source, destination):
            Path(destination).write_bytes(b"competitor")
            return real_link(source, destination)

        with patch("vela_report.core.os.link", side_effect=competing_link):
            with self.assertRaises(FileExistsError):
                self.report.export(target)
        self.assertEqual(target.read_bytes(), b"competitor")
        self.assertFalse(list(self.root.glob(".vela-report-*")))

    def test_minimal_plan_executes_no_external_commands(self):
        def forbidden(*args, **kwargs):
            self.fail("An excluded collector executed a command")
        Collector(self.report, runner=forbidden).collect([], {"boot": "-1"}, {"title": "Previous incident"})
        self.assertFalse(self.report.artifacts)
        self.assertTrue(all(result["status"] == "excluded" for result in self.report.results.values()))

    def test_historical_log_from_another_desktop_keeps_provenance(self):
        source = self.root / "old-vela.log"
        source.write_text("old-session errno=24")
        context = {"log_path": str(source), "incident_time": "2026-10-01T12:00:00+02:00", "boot": "-1"}
        Collector(self.report).collect(["logs", "system"], context, {"changed_since_incident": "Updated Vela"})
        with self.export() as archive:
            manifest = json.loads(archive.read("manifest.json"))
            runtime = json.loads(archive.read("system/runtime.json"))
            self.assertEqual(manifest["incident"]["boot"], "-1")
            self.assertEqual(manifest["incident"]["incident_time"], context["incident_time"])
            self.assertEqual(runtime["reporting_desktop"], "OtherDesktop")
            self.assertIn(b"old-session errno=24", archive.read("logs/vela.log"))
            self.assertNotIn("full_vela_commit", runtime)

    def test_failed_collectors_do_not_prevent_other_evidence(self):
        def denied(*args, **kwargs):
            return Result("permission_denied", reason="No journal access")
        Collector(self.report, runner=denied).collect(
            ["logs", "system", "kernel_journal", "resources"], {"boot": "0"}, {"title": "Missing sources"})
        self.assertEqual(self.report.results["logs"]["status"], "unavailable")
        self.assertEqual(self.report.results["kernel_journal"]["status"], "permission_denied")
        self.assertEqual(self.report.results["system"]["status"], "partial")
        self.assertIn("system/runtime.json", self.report.artifacts)
        with self.export() as archive:
            self.assertIn("manifest.json", archive.namelist())

    def test_os_release_known_symlink_is_supported(self):
        original = self.root / "distribution"
        original.write_text("NAME=Fixture\n")
        (self.etc / "os-release").symlink_to(original)
        self.assertEqual(Collector(self.report).system().status, "collected")
        self.assertEqual(self.report.preview("system/os-release.txt"), "NAME=Fixture\n")

    def test_config_only_includes_allowlisted_environment(self):
        Collector(self.report).collect(["config"], {}, {})
        text = self.report.preview("config/current-overrides.json")
        self.assertIn("VELA_DEBUG_SYNC", text)
        self.assertNotIn("UNRELATED_SECRET", text)
        self.assertNotIn("do-not-collect-this", text)

    def test_graphics_excludes_other_pci_devices(self):
        output = ("00:01.0 Ethernet controller [0200]: Private NIC\n\tKernel driver in use: test\n"
                  "01:00.0 VGA compatible controller [0300]: NVIDIA Quadro P1000\n"
                  "\tKernel driver in use: nvidia\n")
        with patch("vela_report.collectors.shutil.which", return_value=None):
            Collector(self.report, runner=lambda *a, **k: Result("collected", output)).graphics()
        text = self.report.preview("system/pci-graphics.txt")
        self.assertIn("Quadro P1000", text)
        self.assertIn("nvidia", text)
        self.assertNotIn("Private NIC", text)

    def test_discovery_distinguishes_supervisor_and_retained_sessions(self):
        self.fake_process(pid=123)
        self.fake_process(pid=124, supervisor=True)
        state = self.environment.state_dir()
        state.mkdir(parents=True)
        (state / "vela.log").write_text("current")
        (state / "vela.log.old").write_text("previous")
        info = discover(self.environment)
        self.assertEqual([p["role"] for p in info["processes"]], ["vela-compositor", "vela-supervisor"])
        self.assertEqual(len(info["logs"]), 2)
        self.assertEqual(info["recommendations"]["Crash"], sorted(collectors.recommended("Crash")))
        self.assertNotIn("vendor", info["recommendations"]["Crash"])

    def test_resource_categories_and_pid_reuse(self):
        identity = self.fake_process()
        sample = resource_sample(self.environment, identity)
        self.assertEqual(sample["status"], "collected")
        self.assertEqual(sample["fd_count"], 5)
        self.assertEqual(sample["nofile"], {"soft": "1024", "hard": "4096"})
        self.assertEqual(sample["fd_types"]["sync_file"], 1)
        self.assertNotIn("secret-document", json.dumps(sample))
        self.fake_process(start=999)
        self.assertEqual(resource_sample(self.environment, identity)["status"], "pid_reused")
        (self.proc / "123/stat").unlink()
        self.assertEqual(resource_sample(self.environment, identity)["status"], "exited")

    def test_pid_replacement_mid_sample_does_not_export_mixed_metrics(self):
        identity = self.fake_process()
        replacement = dict(identity, start_ticks=999)
        with patch("vela_report.collectors.process_identity", side_effect=[identity, replacement]):
            sample = resource_sample(self.environment, identity)
        self.assertEqual(sample["status"], "pid_reused")
        self.assertNotIn("fd_count", sample)

    def test_ordinary_filenames_do_not_look_like_sync_descriptors(self):
        identity = self.fake_process()
        (self.proc / "123/fd/10").symlink_to("/home/private/sync_file-not-an-inode")
        sample = resource_sample(self.environment, identity)
        self.assertEqual(sample["fd_types"]["sync_file"], 1)
        self.assertEqual(sample["fd_types"]["other_file"], 1)

    def test_resource_recording_stop_preserves_valid_samples(self):
        identity = self.fake_process()
        self.report.context = {"processes": [identity], "duration": 60}
        with patch("vela_report.collectors.time.sleep", side_effect=KeyboardInterrupt):
            result = Collector(self.report).resources(lambda _: None)
        self.assertEqual(result.status, "partial")
        samples = [json.loads(line) for line in self.report.preview("samples/resources.jsonl").splitlines()]
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["status"], "collected")

    def test_resource_output_cap_keeps_complete_json_lines(self):
        identity = self.fake_process()
        self.report.context = {"processes": [identity], "duration": 3}
        with patch("vela_report.collectors.TEXT_LIMIT", 900), patch("vela_report.collectors.time.sleep"):
            result = Collector(self.report).resources(lambda _: None)
        self.assertEqual(result.status, "partial")
        data = (self.report.directory / "samples/resources.jsonl").read_bytes()
        self.assertLessEqual(len(data), 900)
        for line in data.splitlines():
            json.loads(line)

    def test_journal_window_boot_scope_and_invalid_context(self):
        self.report.context = {"boot": "-1", "incident_time": "2026-10-01T12:00:00+02:00"}
        calls = []
        runner = lambda args, **kwargs: calls.append(args) or Result("collected", "fixture")
        Collector(self.report, runner).session_journal()
        args = calls[0]
        self.assertEqual(args[args.index("--boot") + 1], "-1")
        start = dt.datetime.fromisoformat(args[args.index("--since") + 1])
        self.assertEqual(start.minute, 50)
        self.assertIn("_COMM=vela-compositor", args)
        self.assertNotIn("--all", args)
        for boot, incident in (("--all", ""), ("0", "2026-10-01T12:00:00")):
            self.report.context = {"boot": boot, "incident_time": incident}
            with self.assertRaises(ValueError):
                Collector(self.report).journal_args()

    def test_incident_log_above_old_limit_is_complete_and_redacted(self):
        source = self.home / "incident.log"
        text = "startup\n" + "normal frame\n" * 210000 + "Too many open files\npassword=private\nfinal failure\n"
        source.write_text(text)
        self.report.context = {"log_path": str(source)}
        result = Collector(self.report).logs()
        self.assertEqual(result.status, "collected")
        collected = (self.report.directory / "logs/vela.log").read_text()
        self.assertIn("startup", collected)
        self.assertIn("final failure", collected)
        self.assertNotIn("private", collected)
        self.assertFalse(self.report.artifacts["logs/vela.log"]["metadata"]["truncated"])
        self.assertIn("file descriptor exhaustion", self.report.observations[0])
        self.assertEqual(source.read_text(), text)

    def test_oversized_log_preserves_startup_and_last_failure(self):
        source = self.home / "large.log"
        source.write_text("startup evidence\n" + "normal frame\n" * 1000 + "latest fatal failure\n")
        self.report.context = {"log_path": str(source)}
        with patch("vela_report.collectors.LOG_LIMIT", 512):
            result = Collector(self.report).logs()
        self.assertEqual(result.status, "partial")
        collected = (self.report.directory / "logs/vela.log").read_text()
        self.assertIn("startup evidence", collected)
        self.assertIn("latest fatal failure", collected)
        self.assertIn("middle of oversized log omitted", collected)
        self.assertLessEqual(len(collected.encode()), 512)

    def test_log_is_refreshed_after_reproduction_sampling(self):
        source = self.home / "live.log"
        source.write_text("before reproduction\n")
        identity = self.fake_process()
        context = {"log_path": str(source), "processes": [identity], "duration": 1}
        def append_failure(_):
            source.write_text(source.read_text() + "Too many open files after recording started\n")
        with patch("vela_report.collectors.time.sleep", side_effect=append_failure):
            Collector(self.report).collect(["logs", "resources"], context, {})
        self.assertIn("after recording started", self.report.preview("logs/vela.log"))

    def test_rotated_log_does_not_replace_selected_incident_snapshot(self):
        source = self.home / "live.log"
        source.write_text("original incident\n")
        identity = self.fake_process()
        context = {"log_path": str(source), "processes": [identity], "duration": 1}
        def rotate(_):
            source.rename(source.with_suffix(".old"))
            source.write_text("different session\n")
        with patch("vela_report.collectors.time.sleep", side_effect=rotate):
            Collector(self.report).collect(["logs", "resources"], context, {})
        self.assertEqual(self.report.results["logs"]["status"], "partial")
        self.assertIn("original incident", self.report.preview("logs/vela.log"))
        self.assertNotIn("different session", self.report.preview("logs/vela.log"))

    def test_auto_journal_uses_boot_recorded_in_incident_log(self):
        source = self.home / "incident.log"
        boot = "b" * 32
        source.write_text("vela-session: started_at=2026-10-08T15:13:26-0400 "
                          "boot_id=bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\nincident\n")
        os.utime(source, (1791486800, 1791486800))
        calls = []
        runner = lambda args, **kw: calls.append(args) or Result("collected", "fixture")
        Collector(self.report, runner).collect(["logs", "kernel_journal"], {"log_path": str(source)}, {})
        query = next(c for c in calls if "-k" in c)
        self.assertEqual(query[query.index("--boot") + 1], boot)
        self.assertEqual(self.report.context["journal_scope"]["time_source"], "log_last_write")
        self.assertIn("--reverse", query)

    def test_rotation_at_open_does_not_replace_incident_snapshot(self):
        source = self.home / "live.log"
        source.write_text("original incident\n")
        reader = collectors.read_regular
        reads = 0
        def rotate_on_refresh(path, *args, **kwargs):
            nonlocal reads
            reads += 1
            if reads == 2:
                source.rename(source.with_suffix(".old"))
                source.write_text("different session\n")
            return reader(path, *args, **kwargs)
        with patch("vela_report.collectors.read_regular", side_effect=rotate_on_refresh):
            Collector(self.report).collect(["logs", "resources"], {"log_path": str(source)}, {})
        self.assertEqual(self.report.results["logs"]["status"], "partial")
        self.assertEqual(self.report.preview("logs/vela.log"), "original incident\n")

    def test_legacy_log_automatically_matches_previous_boot_by_mtime(self):
        source = self.home / "legacy.log"
        source.write_text("vela-supervise: socket wayland-0; starting the compositor\n")
        incident = dt.datetime.fromisoformat("2026-10-08T19:13:48+00:00")
        os.utime(source, (incident.timestamp(), incident.timestamp()))
        old, new = "b" * 32, "c" * 32
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            if "--list-boots" in args:
                return Result("collected", f"-1 {old} Thu 2026-10-08 18:00:00 UTC — Thu 2026-10-08 19:14:00 UTC\n"
                              f"0 {new} Thu 2026-10-08 19:16:13 UTC — Thu 2026-10-08 19:18:43 UTC\n")
            return Result("collected", "incident kernel evidence")
        Collector(self.report, runner).collect(["logs", "kernel_journal"], {"log_path": str(source)}, {})
        query = next(c for c in calls if "-k" in c)
        self.assertEqual(query[query.index("--boot") + 1], old)
        when = dt.datetime.fromisoformat(query[query.index("--since") + 1])
        self.assertEqual(when, incident - dt.timedelta(minutes=10))

    def test_unknown_incident_boot_never_substitutes_current_boot(self):
        calls = []
        runner = lambda args, **kw: calls.append(args) or Result("collected")
        Collector(self.report, runner).collect(["kernel_journal"], {}, {})
        self.assertEqual(self.report.results["kernel_journal"]["status"], "unavailable")
        self.assertEqual(calls, [])
        self.assertIn("not substituted", self.report.results["kernel_journal"]["reason"])

    def test_explicit_wrong_boot_conflicts_with_session_header(self):
        self.report.context = {"boot": "0", "log_session": {"boot_id": "b" * 32}}
        path = self.proc / "sys/kernel/random"
        path.mkdir(parents=True)
        (path / "boot_id").write_text("c" * 32)
        with self.assertRaisesRegex(ValueError, "conflicts"):
            Collector(self.report).journal_args()

    def test_unknown_time_for_explicit_previous_boot_queries_that_whole_boot(self):
        self.report.context = {"boot": "-1"}
        self.assertEqual(Collector(self.report).journal_args(), ["--boot", "-1"])

    def test_explicit_previous_boot_index_is_checked_against_incident_header(self):
        self.report.context = {"boot": "-1", "log_session": {"boot_id": "b" * 32}}
        runner = lambda args, **kw: Result("collected", f"-1 {'c' * 32} dates\n")
        with self.assertRaisesRegex(ValueError, "conflicts"):
            Collector(self.report, runner).journal_args()
        runner = lambda args, **kw: Result("collected", f"-1 {'b' * 32} dates\n")
        self.assertEqual(Collector(self.report, runner).journal_args(), ["--boot", "b" * 32])

    def test_incident_boot_listing_is_shared_across_collectors(self):
        when = "2026-10-01T12:00:00+00:00"
        boot = "b" * 32
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            if "--list-boots" in args:
                return Result("collected", f"-1 {boot} Thu 2026-10-01 10:00:00 UTC — Thu 2026-10-01 13:00:00 UTC\n")
            return Result("collected", "fixture")
        Collector(self.report, runner).collect(["kernel_journal", "session_journal", "crashes"],
                                              {"incident_time": when}, {})
        self.assertEqual(sum("--list-boots" in args for args in calls), 1)
        queries = [args for args in calls if "--boot" in args]
        self.assertTrue(all(args[args.index("--boot") + 1] == boot for args in queries))

    def test_header_like_injected_arguments_are_not_accepted_as_boot_identity(self):
        source = self.home / "untrusted.log"
        source.write_text("vela-session: started_at=--all boot_id=--all\n")
        self.report.context = {"log_path": str(source)}
        Collector(self.report).logs()
        self.assertNotIn("boot_id", self.report.context["log_session"])

    def test_session_launcher_records_boot_identity_and_preserves_previous_log(self):
        bindir = self.root / "bin with spaces"
        bindir.mkdir()
        compositor = bindir / "vela-compositor"
        compositor.write_text('#!/bin/sh\nprintf "compositor arguments: %s\\n" "$*" >&2\n')
        compositor.chmod(0o700)
        template = Path(__file__).resolve().parents[2] / "session/vela-session.in"
        launcher = self.root / "session"
        launcher.write_text(template.read_text().replace("@CMAKE_INSTALL_FULL_BINDIR@", str(bindir)))
        state = self.home / "state"
        (state / "vela").mkdir(parents=True)
        (state / "vela/vela.log").write_text("previous incident\n")
        result = subprocess.run(["sh", str(launcher), "--test"], capture_output=True,
                                env=dict(os.environ, XDG_STATE_HOME=str(state)), timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((state / "vela/vela.log.old").read_text(), "previous incident\n")
        context = {"log_path": str(state / "vela/vela.log")}
        Collector(self.report).collect(["logs"], context, {})
        self.assertRegex(self.report.context["log_session"]["boot_id"], r"^[0-9a-f]{32}$")
        self.assertIn("started_at", self.report.context["log_session"])
        self.assertIn("--supervise -s", self.report.preview("logs/vela.log"))
        self.assertIn("--test", self.report.preview("logs/vela.log"))

    def test_retained_resource_samples_survive_without_live_process(self):
        source = self.home / "incident.log"
        source.write_text("vela-supervise: resources pid=123 elapsed_s=2 fds=100 sync_file=75 nofile=1024\n"
                          "vela-supervise: resources pid=123 elapsed_s=4 fds=900 sync_file=875 nofile=1024\n")
        Collector(self.report).collect(["logs"], {"log_path": str(source)}, {})
        samples = [json.loads(line) for line in self.report.preview("logs/resource-history.jsonl").splitlines()]
        self.assertEqual([s["sync_file"] for s in samples], [75, 875])
        self.report.remove_collection("logs")
        self.assertNotIn("log_session", self.report.context)
        self.assertNotIn("logs/resource-history.jsonl", self.report.artifacts)

    def test_resource_history_survives_interleaved_wlroots_log_prefix(self):
        source = self.home / "flood.log"
        source.write_text("[00:00:12.000] [ERROR] vela-supervise: resources "
                          "pid=123 fds=1000 sync_file=950 nofile=1024\n")
        Collector(self.report).collect(["logs"], {"log_path": str(source)}, {})
        sample = json.loads(self.report.preview("logs/resource-history.jsonl"))
        self.assertEqual(sample["sync_file"], 950)

    def test_shell_protocol_failures_are_reported_without_fd_exhaustion(self):
        source = self.home / "shell.log"
        source.write_text('zwlr_layer_surface_v1#71: error 2: layer_surface has never been configured\n'
                          '[ERROR] Shell "/usr/bin/vela-shell" exited (code 255): restarting it\n'
                          '[INFO] Shell "/usr/bin/vela-shell" exited (code 0)\n')
        Collector(self.report).collect(["logs"], {"log_path": str(source)}, {})
        summary = json.loads(self.report.preview("logs/error-summary.json"))
        self.assertEqual(summary["shell_restarts"], 1)
        self.assertEqual(summary["layer_surface_unconfigured_errors"], 1)
        self.assertEqual(summary["file_descriptor_exhaustion_messages"], 0)
        self.assertEqual(len(self.report.observations), 2)
        self.report.remove_collection("logs")
        self.assertEqual(self.report.observations, [])

    def test_refresh_removes_derived_errors_no_longer_in_selected_log(self):
        source = self.home / "current.log"
        source.write_text('layer_surface has never been configured\n')
        self.report.context = {"log_path": str(source)}
        collector = Collector(self.report)
        collector.logs()
        source.write_text('new contents, no errors\n')
        collector.logs()
        self.assertNotIn("logs/error-summary.json", self.report.artifacts)
        self.assertEqual(self.report.observations, [])

    def test_no_coredumps_is_collected_empty_evidence(self):
        self.report.context = {"boot": "a" * 32}
        result = Collector(self.report, lambda args, **kw: Result(
            "failed", "No coredumps found.\n", "Command exited with status 1.", 1)).crashes()
        self.assertEqual(result.status, "collected")

    def test_crash_metadata_never_requests_process_memory(self):
        boot = "a" * 32
        self.report.context = {"boot": boot}
        calls = []
        Collector(self.report, lambda args, **kw: calls.append(args) or Result("collected")).crashes()
        self.assertIn("_BOOT_ID=" + boot, calls[0])
        self.assertIn("COREDUMP_UID=" + str(os.getuid()), calls[0])
        self.assertIn("list", calls[0])
        self.assertNotIn("dump", calls[0])

    def test_previous_boot_crashes_resolve_without_mixing_other_boots(self):
        boot = "b" * 32
        self.report.context = {"boot": "-1"}
        calls = []

        def runner(args, **kwargs):
            calls.append(args)
            return Result("collected", f"-1 {boot} previous dates\n0 {'c' * 32} current dates\n")

        Collector(self.report, runner).crashes()
        self.assertIn("_BOOT_ID=" + boot, calls[1])
        self.assertNotIn("_BOOT_ID=" + "c" * 32, calls[1])

    def test_report_total_limit_refuses_evidence_without_writing_it(self):
        with patch("vela_report.core.ARCHIVE_LIMIT", 10):
            self.report.add_text("logs", "logs/first.txt", "12345678")
            with self.assertRaises(ValueError):
                self.report.add_text("logs", "logs/second.txt", "abc")
        self.assertFalse((self.report.directory / "logs/second.txt").exists())

    def test_vendor_generation_stays_sensitive_and_excludable(self):
        def runner(args, **kwargs):
            self.assertEqual(args, ["nvidia-bug-report.sh"])
            self.assertEqual(kwargs["timeout"], 120)
            (Path(kwargs["cwd"]) / "nvidia-bug-report.log.gz").write_bytes(b"unredacted-token")
            return Result("collected")
        Collector(self.report, runner).collect(["vendor"], {}, {})
        name = "vendor/nvidia-bug-report.log.gz"
        self.assertTrue(self.report.artifacts[name]["sensitive"])
        self.assertNotIn("attachments", self.report.results)
        self.report.remove_collection("vendor")
        with self.export() as archive:
            self.assertNotIn(name, archive.namelist())
            self.assertNotIn(b"unredacted-token", archive.read("manifest.json"))

    def test_run_command_timeout_output_limit_and_missing_tool(self):
        result = run_command([sys.executable, "-c", "import time; time.sleep(5)"], timeout=0.15)
        self.assertEqual(result.status, "timed_out")
        result = run_command([sys.executable, "-c", "print('x'*20000)"], limit=1000)
        self.assertEqual(result.status, "partial")
        self.assertEqual(len(result.output), 1000)
        self.assertEqual(run_command(["/missing/vela-report-tool"]).status, "unavailable")

    def test_command_timeout_kills_descendants_holding_pipes(self):
        code = ("import subprocess, sys\n"
                "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(10)'])\n")
        before = time.monotonic()
        self.assertEqual(run_command([sys.executable, "-c", code], timeout=0.2).status, "timed_out")
        self.assertLess(time.monotonic() - before, 2)

    def test_runtime_socket_uses_read_only_command_and_omits_titles(self):
        state = {"workspace": 1, "outputs": [{"name": "DP-1", "scale": 1.25, "serial": "secret"}],
                 "windows": [{"app": "firefox", "title": "private document", "x": 10}],
                 "unexpected": "private"}
        connection = unittest.mock.MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.return_value = (json.dumps(state) + "\n").encode()
        info = argparse.Namespace(st_mode=stat.S_IFSOCK, st_uid=os.getuid())
        with patch("vela_report.collectors.Path.lstat", return_value=info), \
                patch("vela_report.collectors.socket.socket", return_value=connection):
            result = query_state("/fixture/socket", os.getuid())
        connection.sendall.assert_called_once_with(b"state\n")
        self.assertNotIn("title", result["windows"][0])
        self.assertNotIn("serial", result["outputs"][0])
        self.assertNotIn("private", json.dumps(result))

    def test_brightness_probes_the_displays_and_asks_the_compositor(self):
        state = {"outputs": [{"name": "DP-3", "description": "Mi Monitor", "mechanism": "ddc",
                              "state": "no-answer", "value": None, "max": 0, "seq": 0, "serial": "secret"}]}
        connection = unittest.mock.MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.return_value = (json.dumps(state) + "\n").encode()
        info = argparse.Namespace(st_mode=stat.S_IFSOCK, st_uid=os.getuid())
        probe = "DP-3: bus /dev/i2c-9 (aux)\n  result: no-answer in 540 ms (lock 0 ms), transfers rdwr\n"
        calls = []
        runner = lambda args, **kwargs: calls.append(args) or Result("collected", probe)
        self.report.context = {"socket": "/fixture/socket"}
        with patch("vela_report.collectors.brightness_helper", return_value=Path("/usr/lib/vela/vela-brightness")), \
                patch("vela_report.collectors.Path.lstat", return_value=info), \
                patch("vela_report.collectors.socket.socket", return_value=connection):
            result = Collector(self.report, runner).brightness()
        self.assertEqual(result.status, "collected")
        self.assertEqual(calls, [["/usr/lib/vela/vela-brightness", "--probe"]])
        connection.sendall.assert_called_once_with(b"brightness\n")
        self.assertIn("no-answer", self.report.preview("system/brightness-probe.txt"))
        exported = self.report.preview("session/brightness-state.json")
        self.assertIn("Mi Monitor", exported)
        self.assertNotIn("secret", exported)

    def test_brightness_without_the_helper_or_a_session(self):
        def forbidden(*args, **kwargs):
            self.fail("No helper: nothing to run")
        with patch("vela_report.collectors.brightness_helper", return_value=None):
            result = Collector(self.report, forbidden).brightness()
        self.assertEqual(result.status, "unavailable")
        self.assertFalse(self.report.artifacts)
        # Opt-in: it opens every display's bus.
        self.assertFalse(any("brightness" in collectors.recommended(c) for c in collectors.CATEGORIES))

    def test_cancel_plan_does_not_collect_and_cleans_staging(self):
        options = argparse.Namespace(description_file=None, state_dir=None, log=None,
                                     incident_time=None, boot="0", duration=60, output=None)
        answers = ["test", "7", ".", ".", ".", "", "", "1", "no", "", "0", "no", "no"]
        staged = self.report.directory
        with patch("vela_report.cli.Report", return_value=self.report), \
                patch("builtins.input", side_effect=answers), contextlib.redirect_stdout(io.StringIO()), \
                patch("vela_report.cli.Collector") as collector:
            self.assertEqual(cli.interactive(options, {}), 130)
        collector.assert_not_called()
        self.assertFalse(staged.exists())

    def test_terminal_previews_escape_control_sequences(self):
        self.assertEqual(cli.display("\x1b[2Jerror=24\x00"), "[0x1b][2Jerror=24[0x00]")

    def test_noninteractive_cli_requires_interface_instead_of_collecting(self):
        with patch("sys.stdin.isatty", return_value=False), \
                contextlib.redirect_stderr(io.StringIO()), patch("vela_report.cli.interactive") as interactive:
            self.assertEqual(cli.main(["--cli"]), 2)
        interactive.assert_not_called()

    def test_failed_graphical_probe_falls_back_to_guided_cli(self):
        helper = self.root / "helper"
        helper.write_text("fixture")
        with patch.dict(os.environ, {"DISPLAY": ":invalid"}), \
                patch("vela_report.cli.subprocess.run", side_effect=subprocess.TimeoutExpired("probe", 10)), \
                patch("sys.stdin.isatty", return_value=True), patch("sys.stdout.isatty", return_value=True), \
                patch("vela_report.cli.interactive", return_value=130) as interactive:
            self.assertEqual(cli.main([], gui_path=str(helper)), 130)
            interactive.assert_called_once()
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(cli.main(["--gui"], gui_path=str(helper)), 2)
            self.assertEqual(interactive.call_count, 1)

    def test_pure_cli_end_to_end_without_display_or_vela(self):
        output = self.root / "cli-report.zip"
        environment = dict(os.environ, HOME=str(self.home), XDG_STATE_HOME=str(self.home / "state"),
                           PYTHONPATH=str(Path(__file__).resolve().parents[1]), PATH="/nonexistent")
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"):
            environment.pop(key, None)
        primary, secondary = pty.openpty()
        process = subprocess.Popen([sys.executable, "-m", "vela_report", "--cli", "--output", str(output)],
                                   stdin=secondary, stdout=secondary, stderr=secondary, env=environment)
        os.close(secondary)
        # Explicitly customize and refuse every collection, then review and export.
        answers = ["TTY incident", "7", ".", ".", ".", "", "", "1", "no", "", "0", "yes"]
        answers += ["no"] * (len(collectors.SPECS) - 1)  # vendor tool is unavailable
        answers += ["yes", "view 1", "export", "", "yes"]
        os.write(primary, ("\n".join(answers) + "\n").encode())
        transcript = bytearray()
        deadline = time.monotonic() + 15
        try:
            while time.monotonic() < deadline:
                if select.select([primary], [], [], 0.1)[0]:
                    try:
                        transcript.extend(os.read(primary, 65536))
                    except OSError:
                        break
                if process.poll() is not None:
                    break
            if process.poll() is None:
                process.kill()
            self.assertEqual(process.wait(), 0, transcript.decode(errors="replace"))
            with zipfile.ZipFile(output) as archive:
                manifest = json.loads(archive.read("manifest.json"))
                self.assertEqual(manifest["description"]["title"], "TTY incident")
                self.assertFalse(manifest["artifacts"])
                self.assertIn(b"TTY incident", archive.read("report.md"))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(primary)


if __name__ == "__main__":
    unittest.main()
