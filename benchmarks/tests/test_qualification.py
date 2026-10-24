# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Acceptance rejects missing PID 1 recovery and irreproducible instruments."""

import copy
from pathlib import Path
import tempfile
import unittest

from vela_bench.archive import atomic_json
from vela_bench.host import FAULT_UNIT
from vela_bench.qualification import audit_pilot, qualification_summary


class QualificationAcceptance(unittest.TestCase):
    def test_fault_requires_pid1_result_input_cleanup_and_complete_hardware_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            atomic_json(directory / "vela-session.json", {"fixture_hidden_from_both_trees": True, "graphical_session_target": "active"})
            atomic_json(directory / "vela-conditions.json", {"status": "passed"})
            good = {"stage": "failed", "result": "failed", "conditions_snapshot": {}, "conditions_restore": {"status": "passed"},
                    "fault_injected": "crash", "unit": FAULT_UNIT, "supervision": {"SERVICE_RESULT": "signal"}, "fault_uinput_cleanup": "passed"}
            for case in ("good", "wrong-exit", "leaked-input", "restore-failed", "wrong-fault"):
                state = copy.deepcopy(good)
                if case == "wrong-exit": state["supervision"]["SERVICE_RESULT"] = "success"
                elif case == "leaked-input": state["fault_uinput_cleanup"] = "failed"
                elif case == "restore-failed": state["conditions_restore"]["status"] = "failed"
                elif case == "wrong-fault": state["fault_injected"] = "hang"
                atomic_json(directory / "state.json", state)
                with self.subTest(case=case):
                    self.assertEqual(audit_pilot(directory, "vela", "crash")["status"], "passed" if case == "good" else "failed")

    def test_independent_block_variation_cannot_be_hidden_by_complete_execution(self):
        def block(value):
            return {"target": "vela", "status": "complete", "fault": None, "profile": {"mode": {"refresh_mhz": 60000}},
                    "audit": {"status": "passed", "calibration": {"input": {"metrics": {"injection_to_presentation_upper_ms": {"p50": value}}}}}}
        blocks = [block(5), block(50)] + [{"target": "vela", "status": "complete", "fault": fault, "audit": {"status": "passed"}}
                                               for fault in ("crash", "hang")]
        result = qualification_summary(blocks, ["vela"])
        self.assertEqual(result["status"], "pending")
        self.assertFalse(result["checks"]["vela/A-A-repeatability"])
        self.assertEqual(result["repeatability"]["vela"]["p50_range_ms"], 45)
        self.assertFalse(result["performance_comparison_available"])


if __name__ == "__main__":
    unittest.main()
