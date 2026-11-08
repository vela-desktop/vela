# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench import BenchError
from vela_bench.archive import read_json
from vela_bench.host import Layout
from vela_bench.pilot import quiesce_display_manager, stop_greeter_manager


class GreeterRelease(unittest.TestCase):
    @staticmethod
    def manager_session(class_='manager-early'):
        return {'Id': '4', 'User': '957', 'Class': class_, 'Type': 'unspecified',
                'Service': 'systemd-user', 'Remote': 'no', 'Seat': '', 'TTY': '', 'VTNr': '0'}

    def test_manager_sessions_and_remaining_pam_scopes_are_released_in_order(self):
        identity = {'account': 'plasmalogin', 'uid': 957, 'gid': 957}
        user = SimpleNamespace(pw_name='plasmalogin', pw_uid=957, pw_gid=957)
        for class_ in ('manager', 'manager-early'):
            with self.subTest(class_=class_), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                state = {'stage': 'takeover', 'conditions_snapshot': {'greeter': identity}}
                sessions = [self.manager_session(class_),
                            {'Id': 'c2', 'User': '957', 'Class': 'greeter', 'Type': 'wayland',
                             'Remote': 'no', 'Seat': 'seat0', 'Scope': 'session-c2.scope'}]
                def run(command, **kwargs):
                    stdout = ('ActiveState=inactive\nLoadState=not-found\n' if command[1] == 'show'
                              and command[2] == 'session-c2.scope' else 'inactive')
                    return SimpleNamespace(returncode=0, stdout=stdout, stderr='')
                host = SimpleNamespace(run=Mock(side_effect=run), pids=Mock(return_value=[]))
                with patch('vela_bench.pilot.pwd.getpwnam', return_value=user), \
                     patch('vela_bench.pilot.pwd.getpwuid', return_value=user), \
                     patch('vela_bench.pilot.logind_sessions', return_value=sessions):
                    stop_greeter_manager(host, directory, state, Layout(directory))
                commands = [call.args[0] for call in host.run.call_args_list]
                self.assertLess(commands.index(['systemctl', 'stop', 'session-c2.scope']),
                                commands.index(['systemctl', 'stop', 'user@957.service', 'user-runtime-dir@957.service']))
                receipt = read_json(directory / 'greeter-user-release.json')
                self.assertEqual(receipt['status'], 'released')
                self.assertEqual(receipt['sessions'], [{'Id': 'c2', 'Scope': 'session-c2.scope'}])

    def test_interactive_or_malformed_manager_sessions_refuse_before_quiescence(self):
        identity = {'account': 'plasmalogin', 'uid': 957, 'gid': 957}
        user = SimpleNamespace(pw_name='plasmalogin', pw_uid=957, pw_gid=957)
        for changed in ({'Class': 'user'}, {'Class': 'background'}, {'Class': 'unknown'},
                        {'Service': 'sshd'}, {'Type': 'tty'}, {'Remote': 'yes'},
                        {'Seat': 'seat0'}, {'TTY': 'tty1'}, {'VTNr': '1'}, {'Service': ''}):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                host = Mock()
                state = {'stage': 'takeover', 'conditions_snapshot': {'greeter': identity}}
                with patch('vela_bench.pilot.pwd.getpwnam', return_value=user), \
                     patch('vela_bench.pilot.pwd.getpwuid', return_value=user), \
                     patch('vela_bench.pilot.logind_sessions', return_value=[dict(self.manager_session(), **changed)]):
                    with self.assertRaisesRegex(BenchError, 'unexpected session'):
                        quiesce_display_manager(host, Path(temporary), state, Layout(Path(temporary)))
                host.run.assert_not_called()
                self.assertNotIn('display_manager_quiescence', state)

    def test_scope_stop_failure_or_active_readback_prevents_manager_release(self):
        identity = {'account': 'plasmalogin', 'uid': 957, 'gid': 957}
        user = SimpleNamespace(pw_name='plasmalogin', pw_uid=957, pw_gid=957)
        greeter = {'Id': 'c2', 'User': '957', 'Class': 'greeter', 'Type': 'wayland',
                   'Remote': 'no', 'Seat': 'seat0', 'Scope': 'session-c2.scope'}
        for code, active, load, succeeds in ((5, 'inactive', 'not-found', True),
                                             (1, 'inactive', 'not-found', False),
                                             (5, 'active', 'loaded', False),
                                             (0, 'active', 'loaded', False)):
            with self.subTest(code=code, active=active), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                state = {'stage': 'takeover', 'conditions_snapshot': {'greeter': identity}}
                def run(command, **kwargs):
                    return SimpleNamespace(returncode=code if command == ['systemctl', 'stop', 'session-c2.scope'] else 0,
                        stdout=f'ActiveState={active}\nLoadState={load}\n' if command[2] == 'session-c2.scope' else 'inactive', stderr='stop error')
                host = SimpleNamespace(run=Mock(side_effect=run), pids=Mock(return_value=[]))
                with patch('vela_bench.pilot.pwd.getpwnam', return_value=user), \
                     patch('vela_bench.pilot.pwd.getpwuid', return_value=user), \
                     patch('vela_bench.pilot.logind_sessions', return_value=[self.manager_session(), greeter]):
                    if succeeds:
                        stop_greeter_manager(host, directory, state, Layout(directory))
                    else:
                        with self.assertRaises(BenchError):
                            stop_greeter_manager(host, directory, state, Layout(directory))
                commands = [call.args[0] for call in host.run.call_args_list]
                self.assertEqual(['systemctl', 'stop', 'user@957.service', 'user-runtime-dir@957.service'] in commands, succeeds)

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
        for snapshot in ({}, None):
            stop_greeter_manager(host, Path('/unused'), {'conditions_snapshot': snapshot}, Layout())
        host.run.assert_not_called()
