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
from vela_bench import BenchError, pilot
from vela_bench.archive import read_json
from vela_bench.diagnostics import stop_instrument, user_command
from vela_bench.host import Layout
from private_manager import PrivateManager


def main():
    uid, gid = os.getuid(), os.getgid()
    record = {'uid': uid, 'gid': gid}
    user = SimpleNamespace(pw_name='plasmalogin', pw_uid=uid, pw_gid=gid)
    results = []
    for class_ in ('manager', 'manager-early'):
        results.append(release_case(class_, uid, gid, record, user))
    print(json.dumps({'physical_acceptance': False, 'cases': results,
                      'operator_user_manager_untouched': True}))


def release_case(class_, uid, gid, record, user):
    with tempfile.TemporaryDirectory(prefix='vela-greeter-release-') as temporary:
        directory = Path(temporary)
        layout = Layout(directory)
        runtime = layout.path(f'run/user/{uid}')
        runtime.mkdir(parents=True)
        state = {'stage': 'takeover', 'conditions_snapshot': {'greeter':
                 {'account': 'plasmalogin', 'uid': uid, 'gid': gid}}}
        with PrivateManager() as manager, (directory / 'services.stderr').open('wb') as errors:
            # Independent units model the DM, its surviving logind/PAM scope,
            # and the KWin user service, respectively. No real login is touched.
            for operation in ('probe', 'observation', 'scene'):
                manager.spawn(user_command(record, layout, operation, ['/usr/bin/sleep', '60']), errors)
                manager.wait(operation, 'active')
            class MappedHost:
                def run(self, command, **kwargs):
                    if command == ['systemctl', 'show', 'display-manager.service', '--property=ActiveState', '--value']:
                        name = manager.unit('vela-benchmark-probe.service')
                        return manager.native(['systemctl', '--user', 'show', name, '--property=ActiveState', '--value'], **kwargs)
                    if command[:3] in (['systemctl', 'stop', 'session-c2.scope'], ['systemctl', 'show', 'session-c2.scope']):
                        name = manager.unit('vela-benchmark-observation.service')
                        return manager.native(['systemctl', '--user', command[1], name, *command[3:]], **kwargs)
                    assert command == ['systemctl', 'stop', f'user@{uid}.service', f'user-runtime-dir@{uid}.service'], command
                    name = manager.unit('vela-benchmark-scene.service')
                    result = manager.native(['systemctl', '--user', 'stop', name], **kwargs)
                    runtime.rmdir()
                    return result

                def pids(self, target):
                    assert target == uid
                    pids = []
                    for operation in ('scene', 'observation'):
                        name = manager.unit('vela-benchmark-' + operation + '.service')
                        pid = int(manager.native(['systemctl', '--user', 'show', name, '--property=MainPID', '--value']).stdout.strip())
                        if pid:
                            pids.append(pid)
                    return pids

            sessions = [{'Id': '4', 'User': str(uid), 'Class': class_, 'Type': 'unspecified',
                         'Service': 'systemd-user', 'Remote': 'no', 'Seat': '', 'TTY': '', 'VTNr': '0'},
                        {'Id': 'c2', 'User': str(uid), 'Class': 'greeter', 'Type': 'wayland',
                         'Remote': 'no', 'Seat': 'seat0', 'Scope': 'session-c2.scope'}]
            with patch.object(pilot.pwd, 'getpwnam', return_value=user), \
                 patch.object(pilot.pwd, 'getpwuid', return_value=user):
                # A same-UID interactive session must refuse before changing DM.
                personal = dict(sessions[0], Class='user', Type='tty', Service='login')
                with patch.object(pilot, 'logind_sessions', return_value=[personal]):
                    try:
                        pilot.quiesce_display_manager(MappedHost(), directory, state, layout)
                    except BenchError as error:
                        assert 'unexpected session' in str(error), error
                    else:
                        raise AssertionError('Interactive session did not block takeover')
                for operation in ('probe', 'scene', 'observation'):
                    manager.wait(operation, 'active')
                with patch.object(pilot, 'logind_sessions', return_value=sessions):
                    pilot.greeter_release_preflight(MappedHost(), state)
                    stop_instrument(manager, record, layout, 'probe')
                    manager.wait('scene', 'active')
                    manager.wait('observation', 'active')
                    pilot.stop_greeter_manager(MappedHost(), directory, state, layout)
            manager.wait('scene', 'inactive')
            manager.wait('observation', 'inactive')
            receipt = read_json(directory / 'greeter-user-release.json')
            assert receipt['status'] == 'released'
            assert receipt['sessions'] == [{'Id': 'c2', 'Scope': 'session-c2.scope'}]
    return {'manager_session_class': class_, 'independent_greeter_service_outlived_dm': True,
            'independent_pam_session_outlived_dm': True, 'native_service_release_verified': True,
            'interactive_session_refused_before_dm_stop': True}


if __name__ == '__main__':
    main()
