#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production quiescence against real, disposable user-manager sleep services.

The unit identity, unit-file publication and system/user bus are substituted.
All condition, freezer, signal, stop and recovery replies come from systemd.
Never addresses a display manager, PAM, a VT, evdev or the operator's desktop.
Run explicitly as an unprivileged user with a running user manager.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench import BenchError, pilot
from vela_bench.archive import read_json
from vela_bench.host import HostOperationError, Layout


def main():
    if os.geteuid() == 0:
        raise RuntimeError('Run this private user-manager proof unprivileged')
    name = 'vela-bench-quiescence-' + uuid.uuid4().hex + '.service'
    runtime = Path('/run/user') / str(os.getuid()) / 'systemd/user'
    unit = runtime / name
    actual_override = runtime / (name + '.d') / '99-vela-benchmark-quiescence.conf'
    definition = '[Unit]\nDescription=Private Vela quiescence proof\n[Service]\nExecStart=/usr/bin/sleep 120\nRestart=always\n'
    if unit.exists() or unit.is_symlink() or actual_override.parent.exists():
        raise RuntimeError('Refusing an existing private proof name')

    def run(command, *, check=True, timeout=15):
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise HostOperationError(command, result.returncode, result.stdout, result.stderr)
        return result

    def ctl(*args, **kwargs):
        return run(['systemctl', '--user', *args], **kwargs)

    def value(prop):
        return ctl('show', name, '--property=' + prop, '--value').stdout.strip()

    results = []
    try:
        runtime.mkdir(parents=True, exist_ok=True)
        with unit.open('x') as stream:
            stream.write(definition)
        ctl('daemon-reload')
        with tempfile.TemporaryDirectory(prefix='vela-quiescence-proof-') as temporary:
            layout = Layout(Path(temporary))
            fake_mask = layout.path('run/systemd/system/plasmalogin.service')
            fake_override = fake_mask.parent / 'plasmalogin.service.d/99-vela-benchmark-quiescence.conf'

            def publish():
                # Emulate a lower-priority installed unit without writing to
                # the operator's ~/.config or any system-manager directory.
                if fake_mask.is_symlink():
                    assert value('ActiveState') in ('inactive', 'failed')
                    if not unit.is_symlink():
                        assert unit.read_text() == definition
                        unit.unlink()
                        unit.symlink_to('/dev/null')
                elif unit.is_symlink():
                    assert os.readlink(unit) == '/dev/null'
                    unit.unlink()
                    unit.write_text(definition)
                if fake_override.exists():
                    actual_override.parent.mkdir(exist_ok=True)
                    actual_override.write_bytes(fake_override.read_bytes())
                else:
                    actual_override.unlink(missing_ok=True)
                    if actual_override.parent.exists():
                        actual_override.parent.rmdir()

            class MappedHost:
                def __init__(self, mode):
                    self.mode = mode
                    self.blocked_start = False
                    self.formatted_conditions = None

                def pids(self, uid):
                    assert uid == os.getuid()
                    return []  # Reserved UID is a fixture, never our real UID.

                def run(self, command, **kwargs):
                    if command[0] == 'busctl':
                        assert command[1:] == ['--system', '--json=short', 'get-property',
                            'org.freedesktop.systemd1', '/org/freedesktop/systemd1/unit/plasmalogin_2eservice',
                            'org.freedesktop.systemd1.Unit', 'Conditions']
                        response = run(['busctl', '--user', '--json=short', 'call',
                            'org.freedesktop.systemd1', '/org/freedesktop/systemd1',
                            'org.freedesktop.systemd1.Manager', 'GetUnit', 's', name])
                        object_path = json.loads(response.stdout)['data'][0]
                        self.formatted_conditions = value('Conditions')
                        return run(['busctl', '--user', '--json=short', 'get-property',
                            'org.freedesktop.systemd1', object_path, 'org.freedesktop.systemd1.Unit', 'Conditions'], **kwargs)
                    assert command[0] == 'systemctl'
                    if command[1] == 'daemon-reload':
                        publish()
                    mapped = ['systemctl', '--user', *[
                        name if item in ('display-manager.service', 'plasmalogin.service') else item
                        for item in command[1:]]]
                    result = run(mapped, **kwargs)
                    if self.mode == 'after-sigstop' and '--signal=SIGSTOP' in command:
                        raise BenchError('Fixture interrupted after SIGSTOP')
                    if '--property=Id' in command:
                        assert result.stdout.strip() == name
                        result.stdout = 'plasmalogin.service\n'
                    if command[1] == 'stop' and fake_override.exists():
                        assert not fake_mask.is_symlink()
                        assert value('FreezerState') == 'running'
                        ctl('start', name)
                        assert value('ActiveState') in ('inactive', 'failed')
                        assert value('ConditionResult') == 'no'
                        self.blocked_start = True
                    return result

            for mode in ('normal', 'login-opening', 'after-sigstop', 'output-release-error'):
                directory = Path(temporary) / uuid.uuid4().hex
                directory.mkdir()
                state = {'stage': 'takeover', 'run_id': directory.name}
                host = MappedHost(mode)
                ctl('start', name)
                assert value('ActiveState') == 'active'
                sessions = [{'Type': 'unspecified', 'State': 'opening', 'Class': 'user'}] if mode == 'login-opening' else []
                with patch.object(pilot, 'trusted'), patch.object(pilot, 'logind_sessions', return_value=sessions):
                    if mode in ('login-opening', 'after-sigstop'):
                        try:
                            pilot.quiesce_display_manager(host, directory, state, layout)
                        except BenchError as error:
                            assert ('after authorization' if mode == 'login-opening' else 'after SIGSTOP') in str(error), error
                        else:
                            raise AssertionError('Opening login must block takeover')
                        assert value('FreezerState') == 'frozen'
                        assert not fake_mask.is_symlink()
                    else:
                        pilot.quiesce_display_manager(host, directory, state, layout)
                        assert value('LoadState') == 'masked'
                        assert host.blocked_start
                    pilot.release_display_manager_quiescence(host, directory, state, layout)
                    pilot.restore_display_manager(host, directory, state)
                    if mode == 'output-release-error':
                        state.update(display_manager_was_active=True, conditions_snapshot={})
                        rejected = {'status': 'failed', 'errors': ['no outputs'], 'output_restart_eligible': True}
                        with patch.object(pilot, 'restore_conditions', return_value=rejected), \
                             patch.object(pilot, 'stop_greeter_manager', side_effect=BenchError('injected release refusal')):
                            outcome = pilot.restore_hardware_conditions(host, layout, directory, state, {'uid': os.getuid()})
                        assert outcome['status'] == 'failed'
                        assert 'injected release refusal' in outcome['errors'][-1]
                        assert read_json(directory / 'conditions-recovery.json')['display_manager_fallback'] == 'started'
                        assert state['output_greeter_restart'] is True
                assert value('ActiveState') == 'active'
                assert value('FreezerState') == 'running'
                assert value('Restart') == 'always'
                assert read_json(directory / 'state.json')['display_manager_quiescence']['status'] == 'released'
                results.append({'case': mode, 'recovered': True,
                    'systemctl_conditions': host.formatted_conditions,
                    'start_inhibition_verified': host.blocked_start,
                    'login_restarted_after_release_refusal': mode == 'output-release-error'})
                ctl('stop', name)
                ctl('reset-failed', name, check=False)
        print(json.dumps({'private_user_units_only': True, 'physical_acceptance': False, 'cases': results}, indent=2))
    finally:
        # Do not kill a frozen unit: thaw/continue first, so PID 1 cannot retain
        # stale freezer state after the final process exits.
        ctl('thaw', name, check=False)
        ctl('kill', '--kill-whom=all', '--signal=SIGCONT', name, check=False)
        ctl('stop', name, check=False)
        if unit.is_symlink():
            assert os.readlink(unit) == '/dev/null'
        unit.unlink(missing_ok=True)
        actual_override.unlink(missing_ok=True)
        if actual_override.parent.exists():
            actual_override.parent.rmdir()
        ctl('daemon-reload')
        ctl('reset-failed', name, check=False)


if __name__ == '__main__':
    main()
