# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""CPU-only regression checks for strict hardware GPU CI guards."""

import tempfile
from pathlib import Path
import unittest

from renderer_identity import verify_renderer_vendor
from run import unittest_result_exit_code


class RendererIdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.sysfs = Path(self.tmp.name)

    def add_device(self, name, vendor):
        location = self.sysfs / name / "device"
        location.mkdir(parents=True)
        (location / "vendor").write_text(vendor, encoding="ascii")

    def test_accepts_selected_nvidia_node(self):
        self.add_device("renderD129", "0x10de\n")
        log = "[INFO] Vela renderer: NVIDIA GeForce RTX 4070 (/dev/dri/renderD129)\n"
        self.assertEqual(
            verify_renderer_vendor(log, "0x10de", self.sysfs), ["/dev/dri/renderD129"]
        )

    def test_rejects_selected_intel_node(self):
        self.add_device("renderD128", "0x8086\n")
        log = "[INFO] Vela renderer: Intel Graphics (/dev/dri/renderD128)\n"
        with self.assertRaisesRegex(AssertionError, "expected 0x10de"):
            verify_renderer_vendor(log, "0x10de", self.sysfs)

    def test_rejects_missing_selection(self):
        with self.assertRaisesRegex(AssertionError, "No Vela renderer"):
            verify_renderer_vendor("WAYLAND_DISPLAY=wayland-1\n", "0x10de", self.sysfs)

    def test_rejects_missing_sysfs_vendor(self):
        log = "Vela renderer: NVIDIA (/dev/dri/renderD129)\n"
        with self.assertRaisesRegex(AssertionError, "Cannot determine PCI vendor"):
            verify_renderer_vendor(log, "0x10de", self.sysfs)

    def test_rejects_mixed_renderer_selections(self):
        self.add_device("renderD129", "0x10de\n")
        self.add_device("renderD128", "0x8086\n")
        log = ("Vela renderer: NVIDIA (/dev/dri/renderD129)\n"
               "Vela renderer: Intel (/dev/dri/renderD128)\n")
        with self.assertRaisesRegex(AssertionError, "expected 0x10de"):
            verify_renderer_vendor(log, "0x10de", self.sysfs)


class StrictUnittestResultTests(unittest.TestCase):
    def test_rejects_skipped_python_case_in_hardware_ci(self):
        result = unittest.TestResult()
        result.testsRun = 3
        result.skipped.append(("test_x11", "Xwayland unavailable"))
        self.assertTrue(result.wasSuccessful())  # Standard unittest hides these skips.
        self.assertEqual(unittest_result_exit_code(result, strict=True), 1)
        self.assertEqual(unittest_result_exit_code(result, strict=False), 0)

    def test_rejects_empty_suite_in_hardware_ci(self):
        result = unittest.TestResult()
        self.assertEqual(unittest_result_exit_code(result, strict=True), 1)

    def test_accepts_full_success(self):
        result = unittest.TestResult()
        result.testsRun = 1
        self.assertEqual(unittest_result_exit_code(result, strict=True), 0)

    def test_rejects_real_failures_with_or_without_strict_mode(self):
        result = unittest.TestResult()
        result.testsRun = 1
        result.failures.append(("test_failure", "failure"))
        self.assertEqual(unittest_result_exit_code(result, strict=True), 1)
        self.assertEqual(unittest_result_exit_code(result, strict=False), 1)


if __name__ == "__main__":
    unittest.main()
