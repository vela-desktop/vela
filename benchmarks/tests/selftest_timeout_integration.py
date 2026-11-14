#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove timeout cleanup of a setsid child and independent instrument service."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench.diagnostics import user_command
from vela_bench.host import Layout
from private_manager import PrivateManager
from selftest import execute


def child(directory):
    # Deliberately omit Python cleanup and ignore TERM. The outer private slice
    # must own both this process tree (including setsid) and the separate unit.
    manager = PrivateManager()
    errors = (directory / 'instrument.stderr').open('wb')
    manager.spawn(user_command({'uid': os.getuid(), 'gid': os.getgid()}, Layout(directory),
                               'scene', ['/usr/bin/sleep', '60']), errors)
    manager.wait('scene', 'active')
    separate = subprocess.Popen(['/usr/bin/sleep', '60'], start_new_session=True)
    (directory / 'owned.json').write_text(json.dumps({'pid': separate.pid,
        'unit': manager.unit('vela-benchmark-scene.service')}))
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(60)


def main():
    with tempfile.TemporaryDirectory(prefix='vela-selftest-timeout-') as temporary:
        directory = Path(temporary)
        runtime = directory / 'runtime'
        runtime.mkdir(mode=0o700)
        owned_slice = 'vela_selftest_' + uuid.uuid4().hex + '.slice'
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                   VELA_SELFTEST_PRIVATE_BUS='1', VELA_SELFTEST_SLICE=owned_slice)
        control = dict(env, XDG_RUNTIME_DIR=f'/run/user/{os.getuid()}',
                       DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{os.getuid()}/bus')
        started = time.monotonic()
        try:
            status, error = execute([sys.executable, str(Path(__file__).resolve()), '--child', str(directory)],
                                    directory / 'test.log', env, 2)
            assert (directory / 'owned.json').exists(), (directory / 'test.log').read_text()[-4000:]
            owned = json.loads((directory / 'owned.json').read_text())
            assert status is None and error
            result = subprocess.run(['systemctl', '--user', 'show', owned['unit'], '--property=ActiveState', '--value'],
                env=control, capture_output=True, text=True, check=True, timeout=5)
            assert result.stdout.strip() == 'inactive', result.stdout
            pid = Path(f"/proc/{owned['pid']}/status")
            deadline = time.monotonic() + 3
            while pid.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            assert not pid.exists() or '\nState:\tZ' in pid.read_text(), 'setsid child survived slice stop'
        finally:
            subprocess.run(['systemctl', '--user', 'stop', owned_slice], env=control,
                           capture_output=True, timeout=8, check=False)
        print(json.dumps({'physical_acceptance': False, 'deadline_failure_preserved': True,
                          'term_ignoring_controller_stopped': True, 'setsid_child_inactive': True,
                          'independent_instrument_inactive': True, 'seconds': round(time.monotonic() - started, 3)}))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--child':
        child(Path(sys.argv[2]))
    else:
        main()
