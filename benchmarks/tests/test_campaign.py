# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real checkpoint/export I/O; all account, session and hardware work mocked."""

import contextlib
import copy
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json, read_json
from vela_bench.campaign import blocks_for, condition_identity, execute, export_campaign, freeze_conditions, inspect_campaign, latest, read_export
from vela_bench.host import Layout


class CalibrationCampaign(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.layout = Layout(Path(self.temporary.name))
        (self.layout.base / "pilots").mkdir(parents=True)
        self.setup = {"account_tag": "tag", "uid": 1001, "files": {}}
        self.snapshot = {"profile": {"primary": "DP-1", "mode": {"width": 1920, "height": 1080, "refresh_mhz": 60000}},
                         "brightness": {"backend": "fixture", "connector": "card1-DP-1", "maximum": 100, "edid_sha256": "edid"},
                         "power_sources": {}}
        self.context = {"targets": {"vela": {}, "plasma": {}}}
        self.calls, self.fail_plasma = [], True
        self.addCleanup(patch.stopall)
        patch("vela_bench.campaign.trusted").start()
        patch("vela_bench.campaign.load_setup", return_value=self.setup).start()
        patch("vela_bench.campaign.require_workers_stopped").start()
        patch("vela_bench.campaign.context", side_effect=lambda *args: copy.deepcopy(self.context)).start()
        patch("vela_bench.pilot.resolve_targets", return_value={"vela": {}, "plasma": {}}).start()
        patch("vela_bench.pilot.tty_preconditions").start()
        self.begin = patch("vela_bench.pilot.begin", side_effect=self.fake_begin).start()
        patch("vela_bench.campaign.audit_pilot", return_value={"status": "passed", "calibration": {
            "input": {"session_accounting": {"cpu_usage_begin_usec": 100, "cpu_usage_end_usec": 200},
                      "metrics": {"actions": 20, "injection_to_presentation_upper_ms": {"p50": 5}}}}}).start()

    def fake_begin(self, host, targets, **options):
        self.calls.append(targets[0])
        if targets == ["plasma"] and self.fail_plasma:
            raise BenchError("synthetic collector failure")
        path = self.layout.base / "pilots" / options["run_id"]
        path.mkdir()
        freeze_conditions(self.layout, options["campaign_run_id"], self.snapshot)
        atomic_json(path / "state.json", {"conditions_snapshot": self.snapshot})
        (path / "sample.jsonl").write_text('{"sample":1}\n')
        return 0

    def campaign(self, **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return execute(SimpleNamespace(), assume_yes=True, layout=self.layout, **options)

    def test_resume_retains_completed_blocks_and_restarts_only_incomplete_attempts(self):
        self.assertEqual(self.campaign(), 1)
        first = latest(self.layout)
        directory = self.layout.base / "campaigns" / first["run_id"]
        completed = directory / "blocks/calibrate-vela-r1/complete.json"
        before = completed.read_bytes()
        self.assertEqual([block["status"] for block in first["blocks"]], ["complete", "failed"])
        self.fail_plasma = False
        self.assertEqual(self.campaign(resume=first["run_id"]), 0)
        self.assertEqual(self.calls, ["vela", "plasma", "plasma"])
        self.assertEqual(completed.read_bytes(), before)
        self.assertEqual(len(list((directory / "blocks/calibrate-plasma-r1").glob("attempt-*.json"))), 2)
        result = latest(self.layout)
        self.assertTrue(result["selection_complete"])
        self.assertEqual(result["qualification"]["status"], "pending")
        self.assertFalse(result["full_suite_complete"])

    def test_changed_completed_evidence_is_not_reused_or_overwritten(self):
        self.campaign()
        result = latest(self.layout)
        source = self.layout.base / "pilots" / result["blocks"][0]["pilot_run_id"] / "sample.jsonl"
        source.write_text("changed")
        before = list(self.calls)
        with self.assertRaisesRegex(BenchError, "evidence changed"):
            self.campaign(resume=result["run_id"])
        self.assertEqual(self.calls, before)
        self.assertEqual(source.read_text(), "changed")

    def test_resume_rejects_changed_environment_or_mode_before_starting_a_session(self):
        self.campaign()
        result = latest(self.layout)
        self.context["environment"] = {"kernel": "different"}
        before = list(self.calls)
        with self.assertRaisesRegex(BenchError, "identical"):
            self.campaign(resume=result["run_id"])
        self.assertEqual(self.calls, before)

    def test_modified_immutable_plan_is_rejected(self):
        self.campaign()
        result = latest(self.layout)
        directory = self.layout.base / "campaigns" / result["run_id"]
        plan = read_json(directory / "plan.json")
        plan["case"] = "all"
        atomic_json(directory / "plan.json", plan)
        with self.assertRaisesRegex(BenchError, "fingerprint"):
            inspect_campaign(directory)

    def test_hardware_resolution_is_written_once_and_rejects_changed_mode(self):
        self.campaign()
        data = latest(self.layout)
        directory = self.layout.base / "campaigns" / data["run_id"]
        before = (directory / "conditions.json").read_bytes()
        self.assertEqual(data["resolved_conditions"], condition_identity(self.snapshot))
        freeze_conditions(self.layout, data["run_id"], self.snapshot)
        self.assertEqual((directory / "conditions.json").read_bytes(), before)
        different = copy.deepcopy(self.snapshot)
        different["profile"]["mode"]["refresh_mhz"] = 144000
        with self.assertRaisesRegex(BenchError, "immutable campaign resolution"):
            freeze_conditions(self.layout, data["run_id"], different)
        self.assertEqual((directory / "conditions.json").read_bytes(), before)

    def test_corrupted_hardware_resolution_is_rejected_on_resume(self):
        self.campaign()
        data = latest(self.layout)
        path = self.layout.base / "campaigns" / data["run_id"] / "conditions.json"
        record = read_json(path)
        record["conditions"]["primary"] = "changed"
        atomic_json(path, record)
        before = list(self.calls)
        with self.assertRaisesRegex(BenchError, "conditions changed"):
            self.campaign(resume=data["run_id"])
        self.assertEqual(self.calls, before)

    def test_cancel_before_start_never_probes_hardware_or_launches_a_worker(self):
        with patch("builtins.input", return_value="CANCEL"), patch("vela_bench.pilot.snapshot_conditions") as snapshot, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(BenchError, "not authorized"):
                execute(SimpleNamespace(), assume_yes=False, layout=self.layout)
        snapshot.assert_not_called()
        self.begin.assert_not_called()
        self.assertFalse(list((self.layout.base / "campaigns").glob("*/conditions.json")))

    def test_full_qualification_needs_independent_blocks_and_both_fault_types(self):
        self.fail_plasma = False
        self.assertEqual(self.campaign(case="all"), 0)
        data = latest(self.layout)
        self.assertEqual(len(data["blocks"]), 8)
        self.assertEqual(data["qualification"]["status"], "passed")
        self.assertTrue(all(data["qualification"]["checks"].values()))
        self.assertFalse(data["performance_comparison_available"])

    def test_export_retains_raw_files_detects_tampering_and_escapes_errors(self):
        self.campaign()
        data = latest(self.layout)
        data["blocks"][1]["error"] = "<script>failure</script>"
        output = export_campaign(self.layout.path("exports"), data, self.layout)
        self.assertTrue((output / "calibrate-vela-r1/sample.jsonl").is_file())
        self.assertEqual(read_export(output)["run_id"], data["run_id"])
        document = (output / "report.html").read_text()
        self.assertNotIn("<script>", document)
        self.assertIn("&lt;script&gt;", document)
        (output / "calibrate-vela-r1/sample.jsonl").write_text("tampered")
        with self.assertRaisesRegex(BenchError, "evidence changed"):
            read_export(output)

    def test_fixed_selection_order_and_seed_do_not_change_workload(self):
        default = blocks_for(["vela", "plasma"], "all")
        rotated = blocks_for(["vela", "plasma"], "all", 1)
        self.assertEqual([row["target"] for row in default[:4]], ["vela", "plasma", "plasma", "vela"])
        self.assertEqual([row["target"] for row in rotated[:4]], ["plasma", "vela", "vela", "plasma"])
        self.assertEqual({row["id"] for row in default}, {row["id"] for row in rotated})
        self.assertEqual(len(blocks_for(["vela", "plasma"], "calibrate")), 2)
        with self.assertRaises(BenchError):
            blocks_for([], "all")


if __name__ == "__main__":
    unittest.main()
