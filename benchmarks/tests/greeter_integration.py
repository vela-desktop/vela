#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real independent services reproduce a greeter outliving its manager cgroup.

Only NSS/logind identity, runtime-directory and system/user boundary are
fixtures. All process/service liveness and stop replies come from systemd.
"""
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench import pilot
from vela_bench.archive import read_json
from vela_bench.diagnostics import stop_instrument, user_command
from vela_bench.host import Layout
from private_manager import PrivateManager


def main():
    uid, gid = os.getuid(), os.getgid()
    record = {'uid': uid, 'gid': gid}
    user = SimpleNamespace(pw_name='plasmalogin', pw_uid=uid, pw_gid=gid)
    with tempfile.TemporaryDirectory(prefix='vela-greeter-release-') as temporary:
        directory = Path(temporary)
        layout = Layout(directory)
        runtime = layout.path(f'run/user/{uid}')
        runtime.mkdir(parents=True)
        state = {'stage': 'takeover', 'conditions_snapshot': {'greeter':
                 {'account': 'plasmalogin', 'uid': uid, 'gid': gid}}}
        with PrivateManager() as manager, (directory / 'services.stderr').open('wb') as errors:
            for operation in ('probe', 'scene'):
                manager.spawn(user_command(record, layout, operation, ['/usr/bin/sleep', '60']), errors)
                manager.wait(operation, 'active')
            stop_instrument(manager, record, layout, 'probe')
            manager.wait('scene', 'active')  # DM cgroup stop did not stop KWin.

            class MappedHost:
                def run(self, command, **kwargs):
                    if command == ['systemctl', 'show', 'display-manager.service', '--property=ActiveState', '--value']:
                        name = manager.unit('vela-benchmark-probe.service')
                        return manager.native(['systemctl', '--user', 'show', name, '--property=ActiveState', '--value'], **kwargs)
                    assert command == ['systemctl', 'stop', f'user@{uid}.service', f'user-runtime-dir@{uid}.service'], command
                    name = manager.unit('vela-benchmark-scene.service')
                    result = manager.native(['systemctl', '--user', 'stop', name], **kwargs)
                    runtime.rmdir()
                    return result

                def pids(self, target):
                    assert target == uid
                    name = manager.unit('vela-benchmark-scene.service')
                    pid = int(manager.native(['systemctl', '--user', 'show', name, '--property=MainPID', '--value']).stdout.strip())
                    return [pid] if pid else []

            with patch.object(pilot.pwd, 'getpwnam', return_value=user), \
                 patch.object(pilot.pwd, 'getpwuid', return_value=user), \
                 patch.object(pilot, 'logind_sessions', return_value=[]):
                pilot.stop_greeter_manager(MappedHost(), directory, state, layout)
            manager.wait('scene', 'inactive')
            assert read_json(directory / 'greeter-user-release.json')['status'] == 'released'
    print(json.dumps({'physical_acceptance': False, 'independent_greeter_service_outlived_dm': True,
                      'native_service_release_verified': True, 'operator_user_manager_untouched': True}))


if __name__ == '__main__':
    main()
