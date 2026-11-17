# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The built and installed reporter must use CMake's selected interpreter."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(all(shutil.which(tool) for tool in ("cmake", "cc", "c++")),
                     "CMake and C/C++ compilers required")
class ReporterLauncherTests(unittest.TestCase):
    def test_configured_interpreter_wins_over_path_for_build_and_install(self):
        with tempfile.TemporaryDirectory(prefix="vela-reporter-launcher-") as directory:
            root = Path(directory)
            build = root / "build"
            prefix = root / "installed"
            configure = subprocess.run([
                "cmake", "-S", str(ROOT), "-B", str(build),
                "-DVELA_BUILD_COMPOSITOR=OFF", "-DVELA_BUILD_SHELL=OFF",
                "-DVELA_BUILD_POLKIT=OFF", "-DVELA_BUILD_TOOLS=OFF", "-DVELA_REPORT_GUI=OFF",
                f"-DPython3_EXECUTABLE={sys.executable}",
                f"-DCMAKE_INSTALL_PREFIX={prefix}",
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            install = subprocess.run(
                ["cmake", "--install", str(build / "report")],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(install.returncode, 0, install.stdout + install.stderr)

            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_python = fake_bin / "python3"
            fake_python.write_text("#!/bin/sh\nexit 97\n")
            fake_python.chmod(0o755)
            env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ.get("PATH", ""))
            for launcher in (build / "report/vela-report", prefix / "bin/vela-report"):
                with self.subTest(launcher=launcher):
                    self.assertEqual(launcher.read_text().splitlines()[0], f"#!{sys.executable}")
                    result = subprocess.run([str(launcher), "--help"], env=env,
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("Create a reviewed local Vela diagnostic ZIP", result.stdout)
