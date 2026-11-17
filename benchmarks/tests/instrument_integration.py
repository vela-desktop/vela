#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production instrument commands and cleanup against real disposable user units."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench.diagnostics import stop_instrument, user_command
from vela_bench.host import Layout
from private_manager import OPERATIONS, PrivateManager


def main():
    record = {'uid': os.getuid(), 'gid': os.getgid()}
    cases = []
    with tempfile.TemporaryDirectory(prefix='vela-instrument-proof-') as temporary:
        layout = Layout(Path(temporary))
        with PrivateManager() as host:
            for operation in OPERATIONS:
                # Real --wait --pipe --collect completion removes the unit.
                # Reusing the same name also verifies cleanup for subsequent
                # controls and repetitions rather than just the first launch.
                for repeat in range(3):
                    result = host.run(user_command(record, layout, operation, ['/usr/bin/true']))
                    assert result.returncode == 0
                    name = host.unit('vela-benchmark-' + operation + '.service')
                    deadline = time.monotonic() + 5
                    while host.native(['systemctl', '--user', 'show', name, '--property=LoadState', '--value']).stdout.strip() != 'not-found':
                        if time.monotonic() >= deadline:
                            raise RuntimeError('Completed transient unit was not collected: ' + name)
                        time.sleep(.02)
                    missing = host.native(['systemctl', '--user', 'stop', name], check=False)
                    assert missing.returncode == 5, missing
                    stop_instrument(host, record, layout, operation)
                cases.append({'operation': operation, 'collected_stop_returncode': 5, 'repetitions': 3})
            with (Path(temporary) / 'live.stderr').open('wb') as errors:
                process = host.spawn(user_command(record, layout, 'calibration', ['/usr/bin/sleep', '60']), errors)
                host.wait('calibration', 'active')
                stop_instrument(host, record, layout, 'calibration')
                process.wait(timeout=5)
            failed = host.run(user_command(record, layout, 'calibration', ['/usr/bin/false']), check=False)
            assert failed.returncode != 0, 'Workload failure must retain its failure status'
            stop_instrument(host, record, layout, 'calibration')
    print(json.dumps({'scope': 'real-systemd-private-user-units', 'physical_acceptance': False,
                      'cases': cases, 'live_instrument_stopped': True,
                      'workload_failure_preserved': True, 'cleanup_verified': True}, indent=2))


if __name__ == '__main__':
    main()
