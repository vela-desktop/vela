# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""DDC transport readiness and deadlines; no monitor commands are executed."""

import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, call, patch

from vela_bench import BenchError
from vela_bench.conditions import DDC_COMMAND, read_ddc_brightness, set_brightness
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


class DdcRestoration(unittest.TestCase):
    def setUp(self):
        self.saved = {"backend": "ddcutil-vcp10", "connector": "card1-HDMI-A-1",
                      "bus": 6, "edid_sha256": "original", "value": 48, "maximum": 100}
        self.bright = dict(self.saved, value=100)
        self.command = [*DDC_COMMAND, "--bus", "6", "setvcp", "10", "48"]
        self.failure = HostOperationError(self.command, 1, "DDC communication failed for monitor on bus /dev/i2c-6")
        self.host = Mock()
        self.read = self.enterContext(patch("vela_bench.conditions.brightness_snapshot", self.host.read))
        self.enterContext(patch("vela_bench.conditions.time.sleep"))

    def test_restoration_retries_after_confirming_the_same_monitor_is_still_at_100(self):
        self.read.side_effect = [self.bright, self.bright, self.saved]
        self.host.run.side_effect = [self.failure, None]
        self.assertEqual(set_brightness(self.host, self.saved, 48, retry_ddc_write=True), self.saved)
        self.assertEqual([entry[0] for entry in self.host.mock_calls], ["read", "run", "read", "run", "read"])
        self.assertEqual(self.host.run.call_args_list, [call(self.command, timeout=5)] * 2)

    def test_timed_out_write_that_reached_the_target_needs_no_repeat(self):
        self.read.side_effect = [self.bright, self.saved]
        self.host.run.side_effect = HostOperationError(self.command, timeout=5)
        self.assertEqual(set_brightness(self.host, self.saved, 48, retry_ddc_write=True), self.saved)
        self.host.run.assert_called_once_with(self.command, timeout=5)

    def test_repeated_communication_failure_keeps_restoration_failed(self):
        self.read.return_value = self.bright
        self.host.run.side_effect = self.failure
        with self.assertRaisesRegex(BenchError, "DDC brightness write failed on bus 6"):
            set_brightness(self.host, self.saved, 48, retry_ddc_write=True)
        self.assertEqual(self.host.run.call_count, 2)

    def test_changed_edid_during_verification_prevents_another_write(self):
        self.read.side_effect = [self.bright, dict(self.saved, edid_sha256="replacement")]
        self.host.run.side_effect = self.failure
        with self.assertRaisesRegex(BenchError, "identity changed"):
            set_brightness(self.host, self.saved, 48, retry_ddc_write=True)
        self.host.run.assert_called_once_with(self.command, timeout=5)

    def test_configuration_and_permanent_errors_do_not_retry(self):
        for retry, error in ((False, self.failure), (True, HostOperationError(self.command, 1, "invalid option"))):
            with self.subTest(retry=retry):
                self.host.reset_mock()
                self.read.return_value = self.bright
                self.host.run.side_effect = error
                with self.assertRaisesRegex(BenchError, "DDC brightness write failed"):
                    set_brightness(self.host, self.saved, 48, retry_ddc_write=retry)
                self.host.run.assert_called_once_with(self.command, timeout=5)
                self.read.assert_called_once()


if __name__ == "__main__":
    unittest.main()
