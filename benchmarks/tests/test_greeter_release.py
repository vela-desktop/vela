# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import copy
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench import BenchError
from vela_bench.archive import read_json
from vela_bench.host import Layout
from vela_bench.pilot import stop_greeter_manager


class GreeterRelease(unittest.TestCase):
    def test_account_session_and_manager_state_gate_any_uid_stop(self):
        good = {'account': 'plasmalogin', 'uid': 957, 'gid': 957}
        for case in ('good', 'personal-name', 'root', 'boolean', 'changed-name', 'changed-gid',
                     'active-dm', 'personal-session', 'same-uid-tty', 'live-pid', 'runtime'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                layout = Layout(directory)
                identity = dict(good)
                if case == 'personal-name': identity['account'] = 'personal'
                if case == 'root': identity['uid'] = 0
                if case == 'boolean': identity['uid'] = True
                user = SimpleNamespace(pw_name='personal' if case == 'changed-name' else 'plasmalogin',
                                       pw_uid=957, pw_gid=958 if case == 'changed-gid' else 957)
                state = {'stage': 'takeover', 'conditions_snapshot': {'greeter': identity}}
                sessions = ([{'Type':'wayland', 'Class':'user', 'State':'online', 'User':'1000'}] if case == 'personal-session'
                            else [{'Type':'tty', 'Class':'user', 'User':'957'}] if case == 'same-uid-tty' else [])
                runtime = layout.path('run/user/957')
                if case == 'runtime': runtime.mkdir(parents=True)
                host = SimpleNamespace(run=Mock(return_value=SimpleNamespace(stdout='active' if case == 'active-dm' else 'inactive')),
                                       pids=Mock(return_value=[55] if case == 'live-pid' else []))
                with patch('vela_bench.pilot.pwd.getpwnam', return_value=user), \
                     patch('vela_bench.pilot.pwd.getpwuid', return_value=user), \
                     patch('vela_bench.pilot.logind_sessions', return_value=sessions):
                    if case == 'good':
                        stop_greeter_manager(host, directory, state, layout)
                        self.assertEqual(read_json(directory / 'greeter-user-release.json')['status'], 'released')
                    else:
                        with self.assertRaises(BenchError):
                            stop_greeter_manager(host, directory, state, layout)
                stops = [call.args[0] for call in host.run.call_args_list if 'stop' in call.args[0]]
                self.assertEqual(bool(stops), case in ('good', 'live-pid', 'runtime'))
                if stops:
                    self.assertEqual(stops, [['systemctl', 'stop', 'user@957.service', 'user-runtime-dir@957.service']])
                if case in ('live-pid', 'runtime'):
                    self.assertEqual(read_json(directory / 'state.json')['greeter_user_release']['status'], 'stopping')

    def test_historical_snapshot_does_not_guess_an_account_to_stop(self):
        host = Mock()
        stop_greeter_manager(host, Path('/unused'), {'conditions_snapshot': {}}, Layout())
        host.run.assert_not_called()
