# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""DDC transport readiness and deadlines; no monitor commands are executed."""

import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench.conditions import DDC_COMMAND, read_ddc_brightness
from vela_bench.host import HostOperationError


class DdcReadiness(unittest.TestCase):
    def command(self):
        return [*DDC_COMMAND, "--bus", "6", "--terse", "getvcp", "10"]

    def test_transient_failure_retries_the_same_read_and_preserves_the_attempt(self):
        command = self.command()
        first = HostOperationError(command, 1, "Display communication failed", "")
        host = SimpleNamespace(run=Mock(side_effect=[first, SimpleNamespace(stdout="VCP 10 C 100 100\n", stderr="")]))
        with patch("vela_bench.conditions.time.sleep"):
            raw, attempts = read_ddc_brightness(host, 6)
        self.assertEqual(raw, "VCP 10 C 100 100\n")
        self.assertEqual([row["returncode"] for row in attempts], [1, 0])
        self.assertEqual(attempts[0]["stdout"], "Display communication failed")
        self.assertTrue(all(call.args[0] == command for call in host.run.call_args_list))
        self.assertNotIn("setvcp", command)

    def test_syntax_errors_and_signaled_processes_are_not_readiness_retries(self):
        for code in (2, -11):
            host = SimpleNamespace(run=Mock(side_effect=HostOperationError(self.command(), code, "unsupported option", "")))
            with self.subTest(code=code), patch("vela_bench.conditions.time.sleep") as sleep:
                with self.assertRaises(HostOperationError):
                    read_ddc_brightness(host, 6)
                self.assertEqual(host.run.call_count, 1)
                sleep.assert_not_called()

    def test_deadline_caps_combined_subprocess_time_and_retains_partial_outputs(self):
        clock, timeouts = [0.], []
        def run(command, **kwargs):
            timeout = kwargs["timeout"]
            timeouts.append(timeout)
            clock[0] += timeout
            raise HostOperationError(command, stdout="partial DDC output", timeout=timeout)
        with patch("vela_bench.conditions.time.monotonic", side_effect=lambda: clock[0]), \
                patch("vela_bench.conditions.time.sleep", side_effect=lambda value: clock.__setitem__(0, clock[0] + value)):
            with self.assertRaises(HostOperationError) as caught:
                read_ddc_brightness(SimpleNamespace(run=run), 6)
        self.assertAlmostEqual(clock[0], 8)
        self.assertLessEqual(sum(timeouts), 8)
        self.assertEqual(len(timeouts), 2)
        attempts = caught.exception.operation["attempts"]
        self.assertEqual(len(attempts), 2)
        self.assertTrue(all(row["stdout"] == "partial DDC output" for row in attempts))
        json.dumps(caught.exception.operation)  # No recursive diagnostic objects.

    def test_fast_transport_failures_have_a_fixed_attempt_limit(self):
        host = SimpleNamespace(run=Mock(side_effect=lambda argv, **kwargs: self.fail_read(argv)))
        with patch("vela_bench.conditions.time.sleep"), self.assertRaises(HostOperationError) as caught:
            read_ddc_brightness(host, 6)
        self.assertEqual(host.run.call_count, 4)
        self.assertEqual(len(caught.exception.operation["attempts"]), 4)

    def fail_read(self, command):
        raise HostOperationError(command, 1, "no response", "")


if __name__ == "__main__":
    unittest.main()
