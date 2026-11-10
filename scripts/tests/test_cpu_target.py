# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""CPU-target and session preflight regressions; no Vela dependencies needed."""

import json
from pathlib import Path
import platform
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/VelaCpuTarget.cmake"
CHECK = ROOT / "session/vela-cpu-check"
X86 = platform.machine().lower() in ("x86_64", "amd64")


@unittest.skipUnless(X86, "x86-64-only CPU target")
class CpuTargetTests(unittest.TestCase):
    def setUp(self):
        if not all(shutil.which(name) for name in ("cmake", "cc", "c++")):
            self.skipTest("CMake and C/C++ compilers required")
        temp = tempfile.TemporaryDirectory(prefix="vela-cpu-cmake-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        src = self.root / "src"
        src.mkdir()
        (src / "c.c").write_text("int main(void) { return 0; }\n")
        (src / "cxx.cpp").write_text("int main() { return 0; }\n")
        (src / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(vela_cpu_test LANGUAGES C CXX)\n'
            'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
            f'include("{MODULE.as_posix()}")\n'
            'add_executable(cpu-c c.c)\n'
            'add_executable(cpu-cxx cxx.cpp)\n'
        )

    def configure(self, target):
        return subprocess.run(
            ["cmake", "-S", str(self.root / "src"), "-B", str(self.root / "build"),
             f"-DVELA_CPU_TARGET={target}"], capture_output=True, text=True,
            timeout=60,
        )

    def test_default_v3_is_applied_to_c_and_cxx(self):
        run = subprocess.run(["cmake", "-S", str(self.root / "src"),
                              "-B", str(self.root / "build")],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        commands = json.loads((self.root / "build/compile_commands.json").read_text())
        self.assertEqual(len(commands), 2)
        self.assertTrue(all("-march=x86-64-v3" in shlex.split(entry["command"]) for entry in commands))

    def test_generic_override_for_comparison_only(self):
        run = self.configure("x86-64")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        commands = json.loads((self.root / "build/compile_commands.json").read_text())
        self.assertTrue(all("-march=x86-64" in shlex.split(entry["command"]) for entry in commands))

    def test_invalid_target_fails_with_actionable_error(self):
        run = self.configure("not-a-real-architecture")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("Compiler does not support VELA_CPU_TARGET", run.stdout + run.stderr)
        self.assertNotIn("Unknown arguments specified", run.stdout + run.stderr)

    def test_empty_target_fails_cleanly(self):
        run = self.configure("")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("VELA_CPU_TARGET must not be empty", run.stdout + run.stderr)

    def test_cxx_only_probe_failure_is_rejected(self):
        # A failed C++ probe must fail configuration even if C succeeds.
        run = subprocess.run(
            ["cmake", "-S", str(self.root / "src"), "-B", str(self.root / "build"),
             "-DVELA_C_MARCH_x86_64_v3=1", "-DVELA_CXX_MARCH_x86_64_v3="],
            capture_output=True, text=True, timeout=60,
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("Compiler does not support VELA_CPU_TARGET", run.stdout + run.stderr)
        self.assertNotIn("Unknown arguments specified", run.stdout + run.stderr)


@unittest.skipUnless(X86, "x86-64-only preflight")
class CpuPreflightTests(unittest.TestCase):
    V3 = "cx16 lahf_lm popcnt pni sse4_1 sse4_2 ssse3 avx avx2 bmi1 bmi2 f16c fma abm movbe xsave"

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="vela-cpu-preflight-")
        self.addCleanup(temp.cleanup)
        self.cpuinfo = Path(temp.name) / "cpuinfo"

    def run_check(self, features):
        self.cpuinfo.write_text("processor : 0\nflags : " + features + "\n")
        return subprocess.run(["sh", str(CHECK), str(self.cpuinfo)],
                              capture_output=True, text=True, timeout=5)

    def test_supported_cpu_passes(self):
        run = self.run_check(self.V3)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_missing_one_feature_reports_it(self):
        run = self.run_check(self.V3.replace(" bmi2", ""))
        self.assertEqual(run.returncode, 1)
        self.assertIn("missing: bmi2", run.stderr)

    def test_all_logical_cpus_must_pass(self):
        self.cpuinfo.write_text("flags : " + self.V3 + "\n\nflags : " + self.V3.replace(" fma", "") + "\n")
        run = subprocess.run(["sh", str(CHECK), str(self.cpuinfo)],
                             capture_output=True, text=True, timeout=5)
        self.assertEqual(run.returncode, 1)
        self.assertIn("missing: fma", run.stderr)

    def test_missing_flags_fails_closed(self):
        run = self.run_check("")
        self.assertEqual(run.returncode, 1)
        self.assertIn("does not support", run.stderr)

    def test_no_cpuinfo_fails_closed(self):
        self.cpuinfo.write_text("processor : 0\n")
        run = subprocess.run(["sh", str(CHECK), str(self.cpuinfo)],
                             capture_output=True, text=True, timeout=5)
        self.assertEqual(run.returncode, 1)
        self.assertIn("cannot determine", run.stderr)


if __name__ == "__main__":
    unittest.main()
