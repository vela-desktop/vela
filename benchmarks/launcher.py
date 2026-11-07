#!/usr/bin/python3 -I
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Installed root-owned entry point. Never install setuid or grant sudoers rules."""

from pathlib import Path
import os
import stat
import sys

sys.dont_write_bytecode = True
if len(sys.argv) > 1 and sys.argv[1] == "_session":
    print(f"vela-benchmark-session: interpreter started pid={os.getpid()} uid={os.getuid()}", file=sys.stderr, flush=True)
directory = Path(__file__).resolve().parent
# Check before importing privileged implementation code. A permissions change
# on an installed module must not allow it to run before load_setup can reject
# the installation. Python -I excludes operator PYTHONPATH/site packages.
for source in (directory / "launcher.py", *(directory / "vela_bench" / name for name in
        ("__init__.py", "archive.py", "model.py", "discovery.py", "host.py", "pilot.py", "diagnostics.py", "conditions.py", "calibration.py", "qualification.py", "campaign.py", "measurements.py", "desktop.py"))):
    for path in (source, *source.parents):
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            sys.exit(f"vela-benchmark-launcher: untrusted installed code path: {path}")
sys.path.insert(0, str(directory))
from vela_bench.pilot import launcher_main

raise SystemExit(launcher_main())
