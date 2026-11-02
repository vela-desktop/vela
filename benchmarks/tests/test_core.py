# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json, create_check_archive, inspect_archive, read_events, read_json
from vela_bench.cli import main
from vela_bench.discovery import power_sources, sessions
from vela_bench.model import load_campaign, make_plan, MEASUREMENTS
from vela_bench.report import compare_archives, render_report


def host():
    return {"environment": {"kernel": "test-1", "gpu": ["test-gpu"]},
            "targets": {name: {"argv": [f"/usr/bin/{name}"], "binary_source_revision": None} for name in ("vela", "plasma")},
            "suite": {"version": "test"}, "limitations": [], "rejected_sessions": [], "live_execution_available": False}


class Planning(unittest.TestCase):
    def setUp(self):
        self.campaign = load_campaign()
        self.host = host()

    def test_default_runs_all_families_three_times(self):
        plan = make_plan(self.campaign, self.host)
        self.assertEqual(plan["phases"], list(MEASUREMENTS))
        self.assertEqual(len(plan["blocks"]), 30)
        self.assertEqual(len({x["id"] for x in plan["blocks"]}), 30)
        for phase in MEASUREMENTS:
            self.assertEqual(sum(x["phase"] == phase for x in plan["blocks"]), 6)

    def test_fast_changes_repetition_count_only(self):
        normal = make_plan(self.campaign, self.host)
        fast = make_plan(self.campaign, self.host, fast=True)
        first_pass = [x for x in normal["blocks"] if x["repetition"] == 1]
        self.assertEqual(fast["blocks"], first_pass)
        self.assertEqual(fast["display_policy"], normal["display_policy"])
        self.assertEqual(fast["repetitions"], 1)
        self.assertEqual(fast["mode"], "fast")

    def test_order_does_not_depend_on_flag_order(self):
        a = make_plan(self.campaign, self.host, phases=["overhead", "motion"], targets=["plasma", "vela"])
        b = make_plan(self.campaign, self.host, phases=["motion", "overhead"], targets=["vela", "plasma"])
        self.assertEqual(a, b)
        self.assertEqual(a["phases"], ["motion", "overhead"])

    def test_seed_is_reproducible_and_rotates_targets(self):
        plan = make_plan(self.campaign, self.host, phases=["motion"])
        self.assertEqual(plan, make_plan(self.campaign, self.host, phases=["motion"]))
        self.assertEqual([x["target"] for x in plan["blocks"]], ["vela", "plasma", "plasma", "vela", "vela", "plasma"])
        self.campaign["seed"] = 1
        changed = make_plan(self.campaign, self.host, phases=["motion"])
        self.assertEqual(changed["blocks"][0]["target"], "plasma")

    def test_target_missing_is_error_not_silent_drop(self):
        with self.assertRaisesRegex(BenchError, "gnome"):
            make_plan(self.campaign, self.host, targets=["gnome", "vela"])

    def test_unknown_and_duplicate_selections_rejected(self):
        for selection in ([], ["fake"], ["motion", "motion"], ["motion", "check"], ["calibrate", "overhead"], ["report"]):
            with self.subTest(selection=selection), self.assertRaises(BenchError):
                make_plan(self.campaign, self.host, phases=selection)

    def test_check_has_no_session_blocks(self):
        plan = make_plan(self.campaign, self.host, phases=["check"])
        self.assertEqual(plan["blocks"], [])
        self.assertFalse(plan["takeover"])

    def test_standalone_calibration_is_not_triplicated(self):
        plan = make_plan(self.campaign, self.host, phases=["calibrate"])
        self.assertEqual(len(plan["blocks"]), 2)
        self.assertEqual({x["repetition"] for x in plan["blocks"]}, {1})

    def test_subset_has_only_its_requirements(self):
        plan = make_plan(self.campaign, self.host, phases=["overhead"], targets=["vela"])
        self.assertEqual(plan["unselected_families"], list(MEASUREMENTS[:-1]))
        self.assertTrue(all("capture" not in x["requirements"] for x in plan["blocks"]))

    def test_no_installed_target_is_error(self):
        self.host["targets"] = {}
        with self.assertRaisesRegex(BenchError, "No supported"):
            make_plan(self.campaign, self.host)

    def test_budgets_include_preparation_and_deadlines(self):
        plan = make_plan(self.campaign, self.host, fast=True)
        estimate = plan["estimate"]
        expected = 10 + sum(x["measurement_budget_seconds"] + x["preparation_budget_seconds"] for x in plan["blocks"])
        self.assertEqual(estimate["minimum_seconds"], expected)
        self.assertGreater(estimate["maximum_seconds"], expected)
        self.assertIn("uncalibrated", estimate["basis"])

    def test_reject_campaign_typos_and_invalid_policy(self):
        invalid = ('target = ["vela"]', 'seed = true', 'seed = -1', 'schema = true', 'schema = 2',
                   'phases = []', 'targets = ["invalid"]', 'targets = "vela"',
                   '[display]\nbrightness_percent = 50', '[display]\nscale = 1', '[display]\npower_profile = "balanced"')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "campaign.toml"
            for value in invalid:
                with self.subTest(value=value):
                    path.write_text(value)
                    with self.assertRaises(BenchError):
                        load_campaign(path)


class Discovery(unittest.TestCase):
    def test_normal_entries_only_and_no_command_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            # Missing Type is a normal display-manager session convention.
            (root / "plasma.desktop").write_text('[Desktop Entry]\nExec=/bin/true "literal;$(touch /tmp/not-executed)"\nTryExec=/bin/true\n')
            (root / "vela-no-scanout.desktop").write_text('[Desktop Entry]\nExec=/bin/true\n')
            with patch("vela_bench.discovery.subprocess.run", side_effect=AssertionError("must not execute")):
                found, rejected = sessions(root)
            self.assertEqual(list(found), ["plasma"])
            self.assertEqual(rejected, [])
            self.assertEqual(found["plasma"]["argv"][1], "literal;$(touch /tmp/not-executed)")
            self.assertIsNone(found["plasma"]["binary_source_revision"])
            self.assertTrue(found["plasma"]["executables"][0]["sha256"])

    def test_hidden_invalid_and_unavailable_entries_rejected(self):
        for body in ('Hidden=true\nExec=/bin/true', 'Exec=/missing/desktop', 'Exec=/bin/true %U', 'Type=Link\nExec=/bin/true'):
            with self.subTest(body=body), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "vela.desktop").write_text('[Desktop Entry]\n' + body + '\n')
                found, rejected = sessions(root)
                self.assertEqual(found, {})
                self.assertEqual(rejected[0]["target"], "vela")

    def test_peripheral_and_ambiguous_batteries_not_system_energy(self):
        with tempfile.TemporaryDirectory() as temp:
            for name, scope in (("mouse", "Device"), ("battery", "System"), ("ambiguous", None)):
                directory = Path(temp) / "class/power_supply" / name
                directory.mkdir(parents=True)
                (directory / "type").write_text("Battery")
                (directory / "status").write_text("Discharging")
                if scope:
                    (directory / "scope").write_text(scope)
            found = {x["name"]: x for x in power_sources(temp)}
            self.assertFalse(found["mouse"]["whole_system_candidate"])
            self.assertFalse(found["ambiguous"]["whole_system_candidate"])
            self.assertTrue(found["battery"]["whole_system_candidate"])
            self.assertIn("validation", found["battery"]["reason"])


class Archives(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.host = host()
        self.plan = make_plan(load_campaign(), self.host, phases=["check"])
        self.directory = create_check_archive(self.root, self.plan, self.host)

    def test_complete_preflight_never_claims_performance(self):
        _, _, summary = inspect_archive(self.directory)
        self.assertTrue(summary["selection_complete"])
        self.assertTrue(summary["execution_complete"])
        self.assertFalse(summary["full_suite_complete"])
        self.assertFalse(summary["performance_results"])
        self.assertEqual(summary["valid_measurement_repetitions"], 0)
        self.assertEqual(set(summary["families"].values()), {"not-selected"})

    def test_archive_is_private_and_never_reused(self):
        self.assertEqual(self.directory.stat().st_mode & 0o777, 0o700)
        other = create_check_archive(self.root, self.plan, self.host)
        self.assertNotEqual(self.directory, other)
        self.assertEqual(inspect_archive(other)[1], self.plan)

    def test_missing_completion_record_is_interrupted(self):
        (self.directory / "blocks/check.json").unlink()
        summary = inspect_archive(self.directory)[2]
        self.assertFalse(summary["execution_complete"])
        self.assertFalse(summary["selection_complete"])

    def test_tampered_evidence_rejected(self):
        (self.directory / "check.json").write_text('{"forged":true}\n')
        with self.assertRaisesRegex(BenchError, "evidence mismatch"):
            inspect_archive(self.directory)

    def test_changed_plan_rejected(self):
        changed = copy.deepcopy(self.plan)
        changed["seed"] = 99
        atomic_json(self.directory / "plan.json", changed)
        with self.assertRaisesRegex(BenchError, "fingerprint mismatch"):
            inspect_archive(self.directory)

    def test_truncated_journal_is_not_a_complete_run(self):
        (self.directory / "blocks/check.json").unlink()
        with (self.directory / "events.jsonl").open("ab") as stream:
            stream.write(b'{"event":')
        with self.assertRaisesRegex(BenchError, "Truncated"):
            inspect_archive(self.directory)

    def test_completion_cannot_reference_arbitrary_paths(self):
        marker = read_json(self.directory / "blocks/check.json")
        marker["files"] = {"../../outside": "hash"}
        atomic_json(self.directory / "blocks/check.json", marker)
        with self.assertRaisesRegex(BenchError, "completion record"):
            inspect_archive(self.directory)

    def test_atomic_write_failure_preserves_previous_file(self):
        path = self.root / "atomic.json"
        atomic_json(path, {"previous": True})
        with self.assertRaises(ValueError):
            atomic_json(path, {"invalid": float("nan")})
        self.assertEqual(read_json(path), {"previous": True})
        self.assertEqual(list(self.root.glob(".atomic.json.*")), [])

    def test_duplicate_keys_and_nonfinite_json_are_rejected(self):
        path = self.root / "malformed.json"
        for value in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":Infinity}', '{"a":1e999}'):
            path.write_text(value)
            with self.subTest(value=value), self.assertRaises(BenchError):
                read_json(path)

    def test_malformed_manifest_is_rejected_without_a_key_error(self):
        manifest = read_json(self.directory / "manifest.json")
        del manifest["run_id"]
        atomic_json(self.directory / "manifest.json", manifest)
        with self.assertRaisesRegex(BenchError, "Invalid archived manifest"):
            inspect_archive(self.directory)

    def test_journal_rejects_backward_clocks_and_duplicate_keys(self):
        path = self.root / "bad-events.jsonl"
        for value in ('{"monotonic_ns":2,"event":"a"}\n{"monotonic_ns":1,"event":"b"}\n',
                      '{"monotonic_ns":1,"event":"a","event":"b"}\n'):
            path.write_text(value)
            with self.subTest(value=value), self.assertRaises(BenchError):
                read_events(path)

    def test_report_escapes_observed_text_and_has_no_external_assets(self):
        observed = host()
        observed["environment"]["kernel"] = '<script src="https://bad.test/x"></script>'
        plan = make_plan(load_campaign(), observed, phases=["check"])
        directory = create_check_archive(self.root, plan, observed)
        render_report(directory)
        page = (directory / "report.html").read_text()
        self.assertNotIn("<script", page)
        self.assertIn("&lt;script", page)
        self.assertIn("no performance measurements", page)
        self.assertIn("default-src 'none'", page)

    def test_environment_change_is_prominent_and_never_a_regression_claim(self):
        changed = host()
        changed["environment"]["kernel"] = "test-2"
        plan = make_plan(load_campaign(), changed, phases=["check"])
        other = create_check_archive(self.root, plan, changed)
        result = compare_archives(self.directory, other)
        self.assertFalse(result["observed_metadata_match"])
        self.assertFalse(result["performance_comparison_available"])
        self.assertEqual(result["differences"][0]["field"], "environment.kernel")

    def test_matching_metadata_is_not_runtime_qualification(self):
        result = compare_archives(self.directory, self.directory)
        self.assertTrue(result["observed_metadata_match"])
        self.assertFalse(result["performance_comparison_available"])


class CLI(unittest.TestCase):
    def invoke(self, args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), patch("vela_bench.cli.discover", return_value=host()):
            code = main(args)
        return code, out.getvalue(), err.getvalue()

    def test_live_run_fails_before_creating_output_or_mutating_host(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "must-not-exist"
            with patch('vela_bench.cli.require_root',side_effect=BenchError('explicit root invocation required')):
                code, _, error = self.invoke(["run", "--fast", "--output", str(output)])
            self.assertEqual(code, 2)
            self.assertIn("explicit root invocation", error)
            self.assertFalse(output.exists())

    def test_check_run_produces_machine_readable_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            code, output, error = self.invoke(["run", "--phase", "check", "--output", temp, "--json"])
            self.assertEqual(code, 0, error)
            data = json.loads(output)
            self.assertTrue((Path(data["archive"]) / "report.html").is_file())
            self.assertFalse(data["summary"]["performance_results"])

    def test_missing_explicit_target_is_error(self):
        code, _, error = self.invoke(["plan", "--target", "gnome"])
        self.assertEqual(code, 2)
        self.assertIn("gnome", error)

    def test_json_plan_is_only_json(self):
        code, output, error = self.invoke(["plan", "--fast", "--json"])
        self.assertEqual(code, 0, error)
        self.assertEqual(json.loads(output)["repetitions"], 1)


if __name__ == "__main__":
    unittest.main()
