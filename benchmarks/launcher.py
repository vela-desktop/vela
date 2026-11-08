#!/usr/bin/python3 -I
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Installed root-owned entry point. Never install setuid or grant sudoers rules."""

from pathlib import Path
import importlib.abc
import importlib.machinery
import importlib.util
import os
import stat
import sys

sys.dont_write_bytecode = True
if len(sys.argv) > 1 and sys.argv[1] == "_session":
    print(f"vela-benchmark-session: interpreter started pid={os.getpid()} uid={os.getuid()}", file=sys.stderr, flush=True)
directory = Path(__file__).resolve().parent
MODULES = ("__init__.py", "archive.py", "model.py", "discovery.py", "host.py", "pilot.py",
           "diagnostics.py", "conditions.py", "calibration.py", "qualification.py", "campaign.py",
           "measurements.py", "desktop.py", "workloads.py", "platform_workloads.py", "pixels.py",
           "resources.py", "interactions.py", "corpus.py", "contention.py", "desktop_legacy.py",
           "desktop_pixels.py", "input_isolation.py", "session_profile.py")
# Check before importing privileged implementation code. A permissions change
# on an installed module must not allow it to run before load_setup can reject
# the installation. Python -I excludes operator PYTHONPATH/site packages.
for source in (directory / "launcher.py", *(directory / "vela_bench" / name for name in MODULES)):
    for path in (source, *source.parents):
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            sys.exit(f"vela-benchmark-launcher: untrusted installed code path: {path}")
class SourceOnly(importlib.machinery.SourceFileLoader):
    def get_code(self, fullname):
        # Do not consult __pycache__: disabling writes alone still reads it.
        return self.source_to_code(self.get_data(self.path), self.path)


class InstalledModules(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname != 'vela_bench' and not fullname.startswith('vela_bench.'):
            return None
        name = '__init__.py' if fullname == 'vela_bench' else fullname.removeprefix('vela_bench.') + '.py'
        if name not in MODULES:
            raise ImportError('Module is outside the verified installed launcher: ' + fullname)
        source = directory / 'vela_bench' / name
        return importlib.util.spec_from_file_location(fullname, source, loader=SourceOnly(fullname, str(source)),
            submodule_search_locations=[str(source.parent)] if fullname == 'vela_bench' else None)


sys.meta_path.insert(0, InstalledModules())
from vela_bench.pilot import launcher_main

raise SystemExit(launcher_main())
