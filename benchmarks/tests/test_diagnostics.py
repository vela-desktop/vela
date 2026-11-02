# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from vela_bench import BenchError
from vela_bench.archive import atomic_json, read_json
from vela_bench.diagnostics import export_report, parse_probe, pilot_results, probe_command, run_probe
from vela_bench.host import Layout


def stream(frames=3, delay=0):
    rows = [{"event": "clock", "presentation_clock_id": 1, "monotonic_clock_id": 1},
            {"event": "probe", "schema": 1, "method": "wayland-presentation-v1",
             "scope": "diagnostic-client-only", "frames": frames, "delay_requested_ms": delay}]
    for frame in range(1, frames + 1):
        timestamp = frame * max(16_000_000, delay * 1_000_000)
        rows += [{"event": "commit", "frame": frame, "commit_clock_ns": timestamp,
                  "monotonic_ns": timestamp, "delay_elapsed_ns": delay * 1_000_000},
                 {"event": "presented", "frame": frame, "presentation_ns": timestamp + 3_000_000,
                  "commit_clock_ns": timestamp, "received_monotonic_ns": timestamp + 4_000_000,
                  "refresh_ns": 16_666_667, "sequence": frame, "flags": 1}]
    rows.append({"event": "summary", "complete": True, "committed": frames, "presented": frames, "discarded": 0})
    return rows


def encode(rows):
    return "".join(json.dumps(row) + "\n" for row in rows)


class PresentationDiagnostics(unittest.TestCase):
    def test_statistics_use_matching_submission_clock_and_explicit_client_scope(self):
        result = parse_probe(encode(stream()), frames=3)
        self.assertEqual(result["submission_to_presentation_ms"]["p95"], 3)
        self.assertEqual(result["presentation_intervals_ms"]["mean"], 16)
        self.assertEqual(result["scope"], "diagnostic-client-only")
        delayed = parse_probe(encode(stream(delay=40)), frames=3, delay=40)
        self.assertEqual(delayed["client_sleep_ms"]["min"], 40)

    def test_discarded_feedback_is_counted_and_excluded_from_latency_distribution(self):
        rows = stream()
        rows[3] = {"event": "discarded", "frame": 1, "received_monotonic_ns": 20_000_000}
        rows[-1].update(presented=2, discarded=1)
        result = parse_probe(encode(rows), frames=3)
        self.assertEqual(result["discarded"], 1)
        self.assertEqual(result["submission_to_presentation_ms"]["samples"], 2)

    def test_truncated_duplicate_missing_or_forged_feedback_never_produces_statistics(self):
        mutations = []
        rows = stream(); rows[-1]["complete"] = False; mutations.append(rows)
        rows = stream(); rows[-1]["presented"] = 2; mutations.append(rows)
        rows = stream(); del rows[3]; mutations.append(rows)
        rows = stream(); rows.insert(4, copy.deepcopy(rows[3])); mutations.append(rows)
        rows = stream(); rows[3]["commit_clock_ns"] += 1; mutations.append(rows)
        rows = stream(); rows[3]["presentation_ns"] = 0; mutations.append(rows)
        rows = stream(); rows[2]["frame"] = True; mutations.append(rows)
        rows = stream(); rows[1]["scope"] = "desktop-latency"; mutations.append(rows)
        rows = stream(); rows[0]["presentation_clock_id"] = -1; mutations.append(rows)
        for rows in mutations:
            with self.subTest(rows=rows), self.assertRaises(BenchError):
                parse_probe(encode(rows), frames=3)
        for raw in (encode(stream())[:-1], '{"event":"clock","event":"probe"}\n', '[NaN]\n'):
            with self.subTest(raw=raw), self.assertRaises(BenchError):
                parse_probe(raw, frames=3)
        with self.assertRaises(BenchError):
            parse_probe(encode(stream(delay=40)), frames=3, delay=0)

    def test_probe_runs_as_reserved_uid_through_its_user_manager(self):
        layout = Layout()
        command = probe_command({"uid": 1001, "gid": 1001}, "client-delay", layout)
        self.assertEqual(command[:4], ["setpriv", "--reuid=1001", "--regid=1001", "--clear-groups"])
        self.assertIn("--user", command)
        self.assertIn("--wait", command)
        self.assertIn("XDG_RUNTIME_DIR=/run/user/1001", command)
        self.assertIn(str(layout.probe), command)
        self.assertEqual(command[-6:], ["--frames", "120", "--delay-ms", "40", "--timeout", "20"])
        with self.assertRaises(BenchError):
            probe_command({"uid": 1001, "gid": 1001}, "arbitrary", layout)

    def test_raw_evidence_survives_failure_and_report_revalidates_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            layout = Layout(directory)
            record = {"uid": 1001, "gid": 1001, "files": {str(layout.probe): "probe hash"}}
            class Host:
                def run(self, command, **kwargs):
                    return SimpleNamespace(stdout=encode(stream(120)), stderr="", returncode=0)
            outcome = run_probe(Host(), record, directory, "vela", "baseline", layout)
            self.assertEqual(outcome["status"], "complete")
            state = {"run_id": "a" * 32, "stage": "done", "result": "passed",
                     "plan": {"targets": {"vela": {}}, "target_order": ["vela"], "presentation_probe": True}}
            data = pilot_results(directory, state)
            self.assertFalse(data["diagnostics_complete"])
            self.assertEqual(data["samples"][1]["status"], "not-run")
            self.assertIn("raw_jsonl", data["samples"][0])
            raw = directory / "vela-baseline.jsonl"
            raw.write_text(encode(stream(120, 40)))
            with self.assertRaises(BenchError):
                pilot_results(directory, state)
            class FailedHost:
                def run(self, command, **kwargs):
                    return SimpleNamespace(stdout='{"event":"clock"}\n', stderr="missing protocol", returncode=1)
            failure = run_probe(FailedHost(), record, directory, "plasma", "baseline", layout)
            self.assertEqual(failure["status"], "failed")
            self.assertNotIn("metrics", failure)
            self.assertEqual((directory / "plasma-baseline.stderr").read_text(), "missing protocol")
            self.assertTrue((directory / "plasma-baseline.jsonl").exists())

    def test_timeout_is_persisted_as_failure_and_not_a_zero_latency_sample(self):
        with tempfile.TemporaryDirectory() as temporary:
            class Host:
                def run(self, *args, **kwargs):
                    raise BenchError("deadline expired")
            result = run_probe(Host(), {"uid": 1001, "gid": 1001, "files": {}}, Path(temporary),
                               "vela", "baseline", Layout())
            self.assertEqual(result["status"], "failed")
            self.assertIn("deadline", result["error"])
            self.assertNotIn("metrics", result)

    def test_report_escapes_errors_refuses_reuse_and_never_claims_a_desktop_ranking(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            atomic_json(root / "vela-session-log.json", {"status": "collected", "text": "<script>session error</script>"})
            (root / "vela-launch.stderr").write_text("x" * 140000 + "\nlauncher failed before desktop exec\n")
            data = pilot_results(root, {"run_id": "b" * 32, "stage": "failed", "result": "failed",
                                        "error": "<script>bad</script>", "plan": {"targets": {"vela": {}}}})
            directory = export_report(root, data)
            document = (directory / "report.html").read_text()
            self.assertNotIn("<script>", document)
            self.assertIn("&lt;script&gt;", document)
            self.assertIn("vela-session-log.json", document)
            self.assertIn("session error", document)
            self.assertEqual(read_json(directory / "summary.json")["session_logs"]["vela-session-log.json"]["status"], "collected")
            launch = data["session_logs"]["vela-launch.stderr"]
            self.assertTrue(launch["truncated"])
            self.assertEqual(len(launch["text"]), 131072)
            self.assertIn("launcher failed before desktop exec", document)
            self.assertIn("do not establish which desktop is faster", document)
            self.assertFalse(read_json(directory / "summary.json")["full_suite_complete"])
            self.assertFalse(data["performance_comparison_available"])
            with self.assertRaises(FileExistsError):
                export_report(root, data)


if __name__ == "__main__":
    unittest.main()
