#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run autonomous benchmark regressions; never start the physical TTY runner."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / 'benchmarks/tests'
sys.path.insert(0, str(ROOT / 'benchmarks'))
from vela_bench.archive import atomic_json


def cases(suite, build):
    python = sys.executable
    selected = [
        ('cpu', [python, '-m', 'unittest', 'discover', '-s', 'benchmarks/tests'], 60, {}),
        ('systemd-instruments', [python, str(TESTS / 'instrument_integration.py')], 30, {}),
        ('systemd-greeter-release', [python, str(TESTS / 'greeter_integration.py')], 30, {}),
        ('systemd-takeover', [python, str(TESTS / 'quiescence_integration.py')], 60, {}),
        ('systemd-recovery', [python, str(TESTS / 'manager_integration.py')], 40, {}),
    ]
    if suite == 'systemd':
        return selected
    selected.insert(1, ('native-build', ['cmake', '--build', str(build), '--parallel', '2', '--target',
        'vela-compositor', 'vela-shell', 'vela-bench-probe', 'vela-bench-desktop', 'vela-bench-scene',
        'vela-screen-dump', 'vela-bench-input', 'vela-input', 'vela-bench-kwin-input-fixture',
        'vela-pattern', 'vela-slowgpu', 'vela-bench-cpuload', 'vela-bench-corpus'], 600, {}))
    selected += [('controls-' + target, [python, str(TESTS / 'session_integration.py'), target], 150,
                  {'VELA_SELFTEST_EVIDENCE': 'evidence'}) for target in ('vela', 'plasma')]
    selected += [('p3-motion-' + target,
        [python, str(TESTS / 'desktop_integration.py'), *(['--plasma'] if target == 'plasma' else []), 'motion'],
        240, {'VELA_P3_EVIDENCE': 'evidence'}) for target in ('vela', 'plasma')]
    if suite == 'full':
        selected += [('p2-slow-apps', [python, str(TESTS / 'contention_integration.py')], 700,
                      {'VELA_P2_EVIDENCE': 'evidence'})]
        for target in ('vela', 'plasma'):
            for phase in ('responsiveness', 'slow-apps'):
                selected.append(('p3-' + phase + '-' + target,
                    [python, str(TESTS / 'desktop_integration.py'), *(['--plasma'] if target == 'plasma' else []), phase],
                    900, {'VELA_P3_EVIDENCE': 'evidence'}))
        selected += [
            ('p4-vela', [python, str(TESTS / 'p4_integration.py')], 600,
             {'VELA_TEST_CORPUS': 'full', 'VELA_P4_EVIDENCE': 'evidence'}),
            ('p4-plasma', [python, str(TESTS / 'plasma_scaling_integration.py'), str(build / 'tools/vela-bench-desktop'), '--corpus'],
             600, {}),
            ('resources-vela', [python, str(TESTS / 'interaction_integration.py')], 120, {}),
            ('resources-plasma', [python, str(TESTS / 'plasma_scaling_integration.py'), str(build / 'tools/vela-bench-desktop'), '--interactions'],
             120, {}),
        ]
    return selected


def source_fingerprint():
    digest = hashlib.sha256()
    for path in sorted([*TESTS.glob('*.py'), *(ROOT / 'benchmarks/vela_bench').glob('*.py')]):
        digest.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def execute(command, path, env, timeout):
    with path.open('wb') as log:
        wrapped = ([sys.executable, str(TESTS / 'selftest_entry.py'), *command[1:]]
                   if command[0] == sys.executable else command)
        process = subprocess.Popen(wrapped, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=timeout), None
        except subprocess.TimeoutExpired:
            return None, f'Case exceeded {timeout} seconds'
        finally:
            # Own the whole test group, including private D-Bus activations.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
            # Instruments are independent user services, not children of the
            # controller's process group. Stop only this case's private slice
            # as well, including if the Python process exceeded its deadline.
            if owned_slice := env.get('VELA_SELFTEST_SLICE'):
                subprocess.run(['systemctl', '--user', 'stop', owned_slice], env=env,
                               capture_output=True, timeout=12, check=False)
                state = subprocess.run(['systemctl', '--user', 'show', owned_slice,
                    '--property=ActiveState', '--value'], env=env, capture_output=True, text=True, timeout=5)
                if state.returncode or state.stdout.strip() not in ('inactive', 'failed'):
                    raise RuntimeError('Could not verify private test slice cleanup: ' + owned_slice)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-native-displays'))
    parser.add_argument('--suite', choices=('quick', 'full', 'systemd'), default='quick')
    parser.add_argument('--case', action='append', help='Run only a named case; repeat for multiple cases (see --list)')
    parser.add_argument('--output', type=Path, help='New directory for checkpointed results, logs and native evidence')
    parser.add_argument('--list', action='store_true', help='Print cases without running them')
    args = parser.parse_args(argv)
    build = args.build.resolve()
    selected = cases(args.suite, build)
    if args.case:
        wanted = set(args.case)
        unknown = wanted - {name for name, _, _, _ in selected}
        if unknown:
            parser.error('Cases outside this suite: ' + ', '.join(sorted(unknown)))
        if any(name.startswith(('controls-', 'p2-', 'p3-', 'p4-', 'resources-')) for name in wanted):
            wanted.add('native-build')
        selected = [case for case in selected if case[0] in wanted]
    if args.list:
        print(json.dumps([{'case': name, 'timeout_seconds': timeout} for name, _, timeout, _ in selected], indent=2))
        return 0
    if os.geteuid() == 0:
        parser.error('run unprivileged; this command never takes over the physical desktop')
    output = (args.output or ROOT / 'benchmark-results' / ('selftest-' +
              datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8])).resolve()
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    env = dict(os.environ, PYTHONPATH=str(ROOT / 'benchmarks'), VELA_BUILD=str(build),
               XDG_RUNTIME_DIR=f'/run/user/{os.getuid()}',
               DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{os.getuid()}/bus')
    # Evidence destinations are owned by each case, never inherited from a
    # previous manual invocation. Each compositor provisions its own HOME/bus.
    for key in ('VELA_P3_DEFER_AUDIT', 'VELA_P3_EVIDENCE', 'VELA_P2_EVIDENCE', 'VELA_P4_EVIDENCE',
                'VELA_TEST_CORPUS', 'VELA_SELFTEST_EVIDENCE'):
        env.pop(key, None)
    summary = {'schema': 1, 'suite': args.suite, 'source_fingerprint': source_fingerprint(),
               'build': str(build), 'physical_acceptance': False, 'performance_comparison_available': False,
               'coverage_limits': ['kernel evdev/uinput', 'PAM/logind and system-manager UID isolation',
                                   'physical DRM/VT, monitor, brightness and power restoration'],
               'status': 'running', 'cases': []}
    atomic_json(output / 'summary.json', summary)
    began = time.monotonic()
    for name, command, timeout, extra in selected:
        directory = output / name
        directory.mkdir()
        case_env = dict(env, **{key: str(directory / value) if value == 'evidence' else value
                               for key, value in extra.items()})
        case_env['VELA_SELFTEST_SLICE'] = 'vela_selftest_' + uuid.uuid4().hex + '.slice'
        print('[selftest] ' + name, flush=True)
        start = time.monotonic()
        try:
            status, error = execute(command, directory / 'test.log', case_env, timeout)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            status, error = None, str(exc)
        result = {'case': name, 'status': 'passed' if status == 0 else 'incomplete' if status == 77 else 'failed',
                  'returncode': status, 'seconds': round(time.monotonic() - start, 3),
                  'log': str((directory / 'test.log').relative_to(output))}
        if error:
            result['error'] = error
        summary['cases'].append(result)
        atomic_json(output / 'summary.json', summary)
        print(f"[selftest] {name}: {result['status']} ({result['seconds']:.1f}s)", flush=True)
    summary.update(status='passed' if all(case['status'] == 'passed' for case in summary['cases']) else 'failed',
                   seconds=round(time.monotonic() - began, 3))
    summary['source_fingerprint_at_end'] = source_fingerprint()
    if summary['source_fingerprint_at_end'] != summary['source_fingerprint']:
        summary.update(status='failed', source_changed_during_run=True)
    binaries = [*sorted((build / 'tools').glob('vela-*')), build / 'compositor/vela-compositor', build / 'shell/vela-shell']
    summary['native_binary_sha256'] = {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in binaries if path.is_file() and os.access(path, os.X_OK)}
    atomic_json(output / 'summary.json', summary)
    print('Selftest: ' + summary['status'] + '; report: ' + str(output / 'summary.json'), flush=True)
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
