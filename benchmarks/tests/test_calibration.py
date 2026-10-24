# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import base64
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest

from vela_bench import BenchError
from vela_bench.calibration import collect_capture, parse_input
from vela_bench.host import Layout
from test_diagnostics import stream


def samples(count=3, delay=0):
    rows = stream(count + 1, delay)
    rows[1].update(method="wayland-input-presentation-v1", scope="software-input-to-reported-presentation")
    result, injections = rows[:2], []
    for frame in range(1, count + 2):
        commit, feedback = copy.deepcopy(rows[frame * 2:frame * 2 + 2])
        timestamp = frame * 100_000_000
        commit.update(commit_clock_ns=timestamp, monotonic_ns=timestamp, input_id=frame - 1,
                      input_received_monotonic_ns=timestamp - delay * 1_000_000 - 1_000_000 if frame > 1 else 0,
                      input_delivered_ms=(timestamp - delay * 1_000_000 - 3_000_000) // 1_000_000 if frame > 1 else 0)
        feedback.update(commit_clock_ns=timestamp, presentation_ns=timestamp + 3_000_000,
                        received_monotonic_ns=timestamp + 4_000_000)
        if frame > 1:
            received = commit["input_received_monotonic_ns"]
            result.append({"event": "input", "action_id": frame - 1, "received_monotonic_ns": received,
                           "delivered_ms": commit["input_delivered_ms"]})
            injections.append({"event": "injected", "action_id": frame - 1,
                               "before_monotonic_ns": received - 2_000_000, "after_monotonic_ns": received - 1_990_000})
        result.extend([commit, feedback])
        if frame == 1:
            result.append({"event": "input-ready", "monotonic_ns": timestamp + 4_000_000})
    result.append(rows[-1])
    return result, injections


class CommonInputCalibration(unittest.TestCase):
    def test_injection_interval_is_preserved_and_known_delay_is_detected(self):
        rows, injections = samples()
        baseline = parse_input(rows, injections, count=3)
        delayed_rows, delayed_injections = samples(delay=40)
        delayed = parse_input(delayed_rows, delayed_injections, count=3, delay=40)
        self.assertEqual(baseline["injection_to_presentation_upper_ms"]["p50"], 6)
        self.assertAlmostEqual(baseline["injection_interval_ms"]["max"], .01)
        self.assertEqual(delayed["injection_to_presentation_upper_ms"]["p50"] - baseline["injection_to_presentation_upper_ms"]["p50"], 40)

    def test_lost_actions_incompatible_clocks_and_reordered_times_are_rejected(self):
        for case in ("missing", "clock", "duplicate", "boolean", "time", "delivered-clock"):
            rows, injections = samples()
            if case == "missing":
                rows = [row for row in rows if not (row["event"] == "input" and row["action_id"] == 2)]
            elif case == "clock":
                rows[0]["presentation_clock_id"] = 2
            elif case == "duplicate":
                next(row for row in rows if row["event"] == "commit" and row["frame"] == 3)["input_id"] = 1
            elif case == "boolean":
                injections[0]["action_id"] = True
            elif case == "time":
                injections[0]["before_monotonic_ns"] = 999_000_000
            else:
                next(row for row in rows if row["event"] == "input")["delivered_ms"] = 999
            with self.subTest(case=case), self.assertRaises(BenchError):
                parse_input(rows, injections, count=3)

    def test_capture_rejects_lost_markers_wrong_backend_geometry_and_nonmonotonic_clocks(self):
        # The native decoder is covered by real headless PNG tests. This
        # fixture tests the coordinator's acceptance boundary, not PNG codecs.
        png = b"\x89PNG\r\n\x1a\n" + b"\0" * 8 + struct.pack("!II", 800, 600)
        original = [{"event": "capture", "backend": "kwin-screenshot2-v1", "index": index,
                     "width": 800, "height": 600, "begin_monotonic_ns": str(index * 100 + 1),
                     "end_monotonic_ns": str(index * 100 + 10), "png_base64": base64.b64encode(png).decode(),
                     "markers": [{"marker": "vela-frame-id-v2", "frame": index, "x": 16, "y": 16}]}
                    for index in range(12)]
        for case in ("good", "missing", "backend", "geometry", "clock", "position", "boolean-id"):
            rows = copy.deepcopy(original)
            if case == "missing": rows[3]["markers"] = []
            elif case == "backend": rows[3]["backend"] = "portal-fallback"
            elif case == "geometry": rows[3]["width"] = 640
            elif case == "clock": rows[3]["begin_monotonic_ns"] = "1"
            elif case == "position": rows[3]["markers"][0]["x"] = 700
            elif case == "boolean-id": rows[3]["markers"][0]["frame"] = True
            host = SimpleNamespace(run=lambda *args, **kwargs: SimpleNamespace(returncode=0, stderr="", stdout="\n".join(json.dumps(row) for row in rows)))
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                arguments = (host, {"uid": 1001, "gid": 1001}, Path(temporary), "plasma",
                             {"primary": "DP-1", "mode": {"width": 800, "height": 600}}, Layout())
                if case == "good":
                    self.assertEqual(len(collect_capture(*arguments)), 12)
                else:
                    with self.assertRaises(BenchError): collect_capture(*arguments)


if __name__ == "__main__":
    unittest.main()
