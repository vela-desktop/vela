# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test-only transport: actual user units, private names and per-unit environments.

Never imports environment into the user's manager and never addresses system
units. UID/namespace boundaries are fixtures; unit replies are never simulated.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'benchmarks'))
from vela_bench.host import HostOperationError

OPERATIONS = ('probe', 'display', 'capture', 'calibration', 'measurement', 'producer', 'scene', 'observation')


def prepare_tools(layout, build):
    layout.home.mkdir(parents=True, exist_ok=True)
    layout.helper.parent.mkdir(parents=True, exist_ok=True)
    for path in (Path(build) / 'tools').iterdir():
        destination = layout.helper.parent / path.name
        if path.is_file() and os.access(path, os.X_OK) and not destination.exists():
            destination.symlink_to(path.resolve())


class PrivateManager:
    def __init__(self, environment=None, layout=None):
        if os.geteuid() == 0:
            raise RuntimeError('Private manager tests must run unprivileged')
        runtime = f'/run/user/{os.getuid()}'
        self.environment = dict(environment or {})
        self.control_env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8',
                            'XDG_RUNTIME_DIR': runtime, 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=' + runtime + '/bus'}
        self.layout = layout
        self.slice = os.environ.get('VELA_SELFTEST_SLICE')
        if self.slice and not re.fullmatch(r'vela_selftest_[0-9a-f]{32}\.slice', self.slice):
            raise ValueError('Invalid private test slice')
        self.prefix = 'vela-selftest-' + uuid.uuid4().hex + '-'
        self.units = set()
        self.processes = []
        self.native(['systemctl', '--user', 'show', '--property=SystemState', '--value'])

    def native(self, command, *, check=True, timeout=20):
        result = subprocess.run(command, env=self.control_env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise HostOperationError(command, result.returncode, result.stdout, result.stderr)
        return result

    def unit(self, name):
        if name not in {'vela-benchmark-' + operation + '.service' for operation in OPERATIONS}:
            raise ValueError('Unexpected test unit: ' + name)
        mapped = self.prefix + name.removeprefix('vela-benchmark-')
        self.units.add(mapped)
        return mapped

    def mapped(self, command):
        # Replace only the reserved UID envelope and the unit identity.
        assert command[:4] == ['setpriv', f'--reuid={os.getuid()}', f'--regid={os.getgid()}', '--clear-groups'], command
        if '/usr/bin/systemctl' in command:
            index = command.index('/usr/bin/systemctl')
            args = command[index:]
            assert args[1] == '--user' and args[2] in ('show', 'stop'), args
            args[3] = self.unit(args[3])
            return args
        index = command.index('/usr/bin/systemd-run')
        args = list(command[index:])
        start = next(i for i, item in enumerate(args[1:], 1) if not item.startswith('--'))
        for i, item in enumerate(args[:start]):
            if item.startswith('--unit='):
                args[i] = '--unit=' + self.unit(item.removeprefix('--unit='))
        native = args[start:]
        if self.layout and '_capture' in native:
            at = native.index('_capture')
            native = [sys.executable, str(Path(__file__).with_name('capture_entry.py')),
                      str(self.layout.root), *native[at + 1:]]
        if Path(native[0]).name == 'vela-bench-cpuload':
            cpus = ','.join(map(str, sorted(os.sched_getaffinity(0))[:2]))
            native = ['taskset', '-c', cpus, *native]
        # Set environment on this unit only. The control connection always uses
        # the real user bus; the native client uses its private compositor bus.
        settings = ['--setenv=' + key + '=' + value for key, value in self.environment.items()
                    if key in ('PATH', 'LANG', 'HOME', 'WAYLAND_DISPLAY', 'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS')
                    or key.startswith(('XDG_', 'QT_', 'VK_', 'WLR_', 'VELA_'))]
        slice_option = ['--slice=' + self.slice] if self.slice else []
        return [*args[:start], *slice_option, *settings, *native]

    def run(self, command, **options):
        return self.native(self.mapped(command), **options)

    def spawn(self, command, errors):
        process = subprocess.Popen(self.mapped(command), env=self.control_env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=errors, bufsize=0, start_new_session=True)
        self.processes.append(process)
        return process

    def wait(self, operation, wanted):
        name = self.unit('vela-benchmark-' + operation + '.service')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = self.native(['systemctl', '--user', 'show', name, '--property=ActiveState', '--value']).stdout.strip()
            if state == wanted:
                return
            time.sleep(.02)
        raise RuntimeError(f'{name}: expected {wanted}, got {state}')

    def close(self):
        errors = []
        for name in sorted(self.units):
            self.native(['systemctl', '--user', 'stop', name], check=False)
            self.native(['systemctl', '--user', 'reset-failed', name], check=False)
            result = self.native(['systemctl', '--user', 'show', name, '--property=ActiveState', '--value'])
            if result.stdout.strip() not in ('inactive', 'failed'):
                errors.append(name)
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
                errors.append('transport ' + str(process.pid))
            for stream in (process.stdin, process.stdout):
                if stream and not stream.closed:
                    stream.close()
        if errors:
            raise RuntimeError('Private test cleanup failed: ' + ', '.join(errors))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
