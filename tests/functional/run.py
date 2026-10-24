#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Vela's functional tests, for ctest.

    run.py              every module (ctest runs each on its own, as
                        functional-<module>)
    run.py test_shell   only these modules
    run.py --sharpness  the bit-for-bit test of scripts/test-sharpness.sh

Needs a GPU with Vulkan 1.4: the renderer has no software fallback. Without
one (such as on CI servers) it exits with 77, which ctest counts as a
skipped test, not a failed one."""

import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SKIP = 77

sys.path.insert(0, HERE)
from harness import gpu_available # noqa: E402


def main():
    if not gpu_available():
        print("No GPU (/dev/dri/renderD*): functional tests skipped.")
        return SKIP
    if "--sharpness" in sys.argv:
        # The pixel check (scripts/sharpness-check.py) runs with the python3 on
        # PATH: that's where numpy and Pillow are needed.
        if subprocess.run(["python3", "-c", "import numpy, PIL"], capture_output=True).returncode != 0:
            print("python3 needs numpy and Pillow: sharpness test skipped.")
            return SKIP
        env = {key: value for key, value in os.environ.items() if key not in ("WAYLAND_DISPLAY", "DISPLAY")}
        env["VELA_TEST_MARK"] = "menu"
        return subprocess.run(["sh", "scripts/test-sharpness.sh"], cwd=ROOT, env=env).returncode
    modules = [argument for argument in sys.argv[1:] if not argument.startswith("-")]
    loader = unittest.defaultTestLoader
    if modules:
        suite = loader.loadTestsFromNames(modules)
    else:
        suite = loader.loadTestsFromNames(sorted(name[:-3] for name in os.listdir(HERE)
                                                 if name.startswith("test_") and name.endswith(".py")))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
