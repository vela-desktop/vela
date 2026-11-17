#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""P0 controls with native private compositors and actual systemd instruments.

Only the input device and privileged UID/VT boundaries are fixtures. The
production controllers, presentation, captures, services and cleanup are real.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'benchmarks'), str(ROOT / 'tests/functional')]
from harness import BUILD, Session, tool
from vela_bench.archive import atomic_json, read_json
from vela_bench.calibration import parse_input, run_input
from vela_bench.conditions import normalized_outputs
from vela_bench.diagnostics import SCENARIOS, run_probe
from vela_bench.host import ACCOUNT, Layout
from vela_bench.model import vela_environment
from vela_bench.pilot import desktop_environment
from private_manager import PrivateManager, prepare_tools
from contention_integration import KEYBOARD

KWIN_INPUT = '''import json,subprocess,sys
p=subprocess.Popen([sys.argv[1]],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
try:
 row=json.loads(p.stdout.readline());row['method']='linux-uinput-f24-v1'
 print(json.dumps(row),flush=True)
 for line in sys.stdin:
  p.stdin.write('pulse\\n');p.stdin.flush()
  print(p.stdout.readline(),end='',flush=True)
finally:p.stdin.close();p.wait(timeout=3)
'''


def check(target, directory, env):
    layout = Layout(directory)
    prepare_tools(layout, BUILD)
    profile = {'primary': 'HEADLESS-1', 'mode': {'width': 1920, 'height': 1080}}
    if target == 'plasma':
        outputs = normalized_outputs(subprocess.check_output([tool('vela-bench-desktop'), '--outputs'],
                                                             env=env, text=True), target)
        profile['primary'] = next(row['name'] for row in outputs if row['enabled'])
    record = {'uid': os.getuid(), 'gid': os.getgid(),
              'files': {str(layout.probe): hashlib.sha256(layout.probe.read_bytes()).hexdigest()}}

    class InputHost(PrivateManager):
        def spawn(self, command, errors):
            if command[0].endswith('vela-bench-input'):
                args = ([sys.executable, '-c', KEYBOARD, tool('vela-input')] if target == 'vela' else
                        [sys.executable, '-c', KWIN_INPUT, tool('vela-bench-kwin-input-fixture')])
                return subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=errors, bufsize=0)
            return super().spawn(command, errors)

    results = []
    try:
        with InputHost(env, layout) as host:
            for scenario in SCENARIOS:
                result = run_probe(host, record, directory, target, scenario, layout)
                assert result['status'] == 'complete', result
                results.append(scenario)
            for name, options in (('input', {}), ('input-capture', {'capture_profile': profile}),
                                  ('input-delay', {'delay': 40})):
                result = run_input(host, record, directory, target, layout, lambda: None, **options)
                assert result['status'] == 'passed', result
                prefix = directory / (target + '-' + name)
                rows = [json.loads(line) for line in prefix.with_suffix('.jsonl').read_text().splitlines()]
                injections = read_json(prefix.with_suffix('.injections.json'))
                reopened = parse_input(rows, injections, count=5 if name == 'input-delay' else 20,
                                       delay=40 if name == 'input-delay' else 0)
                assert all(result['metrics'][key] == value for key, value in reopened.items())
                results.append(name)
        atomic_json(directory / 'selftest-result.json', {'status': 'passed', 'target': target,
                    'controls': results, 'real_user_manager': True, 'cleanup_verified': True,
                    'physical_acceptance': False, 'kernel_uinput': False})
    finally:
        atomic_json(directory / 'development-scope.json', {'physical_acceptance': False,
                    'kernel_uinput': False, 'uid_isolation': False, 'pam_logind_vt': False,
                    'real_user_manager': True, 'native_capture': True, 'profile': profile})


def main(target, vela_sync_test=False):
    if vela_sync_test and target != 'vela':
        raise ValueError('Sync-test controls require Vela')
    with tempfile.TemporaryDirectory(prefix='vela-session-selftest-') as temporary:
        base = Path(temporary)
        directory = base / 'evidence'
        directory.mkdir()
        layout = Layout(directory)
        prepare_tools(layout, BUILD)
        runtime = base / 'runtime'
        runtime.mkdir(mode=0o700)
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'HOME': str(layout.home),
               'XDG_RUNTIME_DIR': str(runtime), 'XDG_CONFIG_HOME': str(layout.home / '.config'),
               'XDG_DATA_HOME': str(layout.home / '.local/share'), 'XDG_STATE_HOME': str(layout.home / '.local/state'),
               'XDG_CACHE_HOME': str(layout.home / '.cache'), 'QT_QPA_PLATFORM': 'wayland',
               'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=' + str(base / 'no-system-bus'),
               'VELA_BUILD': str(BUILD)}
        if vela_sync_test:
            user = SimpleNamespace(pw_uid=os.getuid(), pw_name=ACCOUNT, pw_dir=str(layout.home), pw_shell='/bin/sh')
            created = desktop_environment('vela', user,
                {'XDG_SESSION_ID': 'private-test', 'XDG_SEAT': 'seat0', 'XDG_VTNR': '3'}, vela_sync_test=True)
            env.update({key: created[key] for key in vela_environment(True)})
        if 'VELA_SELFTEST_SLICE' in os.environ:
            env['VELA_SELFTEST_SLICE'] = os.environ['VELA_SELFTEST_SLICE']
        bus = None
        try:
            if target == 'vela':
                address = 'unix:path=' + str(base / 'session-bus')
                bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--nopidfile', '--address=' + address],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                deadline = time.monotonic() + 5
                while not (base / 'session-bus').exists():
                    if bus.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Private session bus did not start')
                    time.sleep(.02)
                env['DBUS_SESSION_BUS_ADDRESS'] = address
                with Session(size='1920x1080@60', env=env) as session:
                    if vela_sync_test:
                        raw = Path(f'/proc/{session.process.pid}/environ').read_bytes()
                        observed = dict(item.decode().split('=', 1) for item in raw.split(b'\0') if b'=' in item)
                        assert all(observed.get(key) == value for key, value in vela_environment(True).items())
                        policy = session.query('scanout-policy')
                        # enabled is the config preference; envDisabled takes
                        # precedence on the actual scanout candidate path.
                        assert policy['envDisabled'] is True, policy
                        session.wait_for_log("VELA_READY_WAIT=0: frames wait for the apps' GPU")
                        atomic_json(directory / 'runtime-profile.json', {'environment': vela_environment(True),
                            'scanout_policy': policy, 'gpu_wait_log_verified': True, 'physical_acceptance': False})
                    check(target, directory, session.client_env)
            else:
                command = ['dbus-run-session', '--', 'kwin_wayland', '--virtual', '--width', '1920', '--height', '1080',
                    '--socket', 'wayland-session-selftest', '--no-lockscreen', '--no-kactivities', '--exit-with-session',
                    shlex.join([sys.executable, str(Path(__file__).resolve()), '--plasma-child', str(directory)])]
                with (directory / 'kwin.log').open('w') as log:
                    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    try:
                        status = process.wait(timeout=100)
                        if status:
                            raise RuntimeError('Private KWin controls failed: ' + (directory / 'kwin.log').read_text()[-5000:])
                    finally:
                        try:
                            os.killpg(process.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=3)
            print(json.dumps(read_json(directory / 'selftest-result.json'), indent=2))
        finally:
            if bus:
                try:
                    os.killpg(bus.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    bus.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(bus.pid, signal.SIGKILL)
                    bus.wait(timeout=3)
            if destination := os.environ.get('VELA_SELFTEST_EVIDENCE'):
                shutil.copytree(directory, destination)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--plasma-child':
        check('plasma', Path(sys.argv[2]), dict(os.environ))
    else:
        args = argparse.ArgumentParser(description=__doc__)
        args.add_argument('target', choices=('vela', 'plasma'))
        args.add_argument('--vela-sync-test', action='store_true')
        options = args.parse_args()
        main(options.target, options.vela_sync_test)
