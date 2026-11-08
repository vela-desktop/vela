# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real schedule/journal/checkpoint/export I/O; session and hardware calls mocked."""

import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json, read_json, read_events
from vela_bench.campaign import block_request, execute, export_campaign, latest, read_export
from vela_bench.cli import main
from vela_bench.model import load_campaign, make_plan


class Execution(unittest.TestCase):
    def setUp(self):
        from test_campaign import CalibrationCampaign
        # Reuse synthetic setup/snapshot and mock interfaces, not its test methods.
        CalibrationCampaign.setUp(self)
        self.audit = patch('vela_bench.campaign.audit_pilot', return_value={
            'status': 'passed', 'checks': {'restored': True}, 'calibration': {}}).start()
        self.options = []
        self.interrupt = False
        self.corrupt_receipt = False
        self.change_context = False
        self.live_pids = []

    def fake_begin(self, host, targets, **options):
        from test_campaign import CalibrationCampaign
        self.options.append(copy.deepcopy(options))
        if self.interrupt and targets == ['plasma']:
            self.calls.append('plasma')
            return 130
        CalibrationCampaign.fake_begin(self, host, targets, **options)
        source = self.layout.base / 'pilots' / options['run_id']
        request = block_request(self.layout, options['campaign_run_id'], options['campaign_block_id'], targets[0])
        state = read_json(source / 'state.json')
        state['campaign_block'] = request
        state['campaign_run_id'] = options['campaign_run_id']
        atomic_json(source / 'state.json', state)
        receipt = dict(request, status='passed')
        if self.corrupt_receipt:
            receipt['block'] = dict(request['block'], repetition=99)
        atomic_json(source / 'block-workload.json', receipt)
        if self.change_context:
            self.context['environment'] = {'kernel': 'changed-between-blocks'}
        return 0

    def campaign(self, **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return execute(SimpleNamespace(pids=lambda uid: self.live_pids), assume_yes=True, layout=self.layout, exercise=True,
                           **dict({'phases': ['motion']}, **options))

    def recoverable_failure(self, host, targets, **options):
        """A real failed pilot state; other blocks use the normal fake runner."""
        if options.get('campaign_block_id') != 'motion-vela-r1':
            return self.fake_begin(host, targets, **options)
        from vela_bench.campaign import freeze_conditions
        self.calls.append(targets[0]); self.options.append(copy.deepcopy(options))
        source = self.layout.base / 'pilots' / options['run_id']
        source.mkdir()
        freeze_conditions(self.layout, options['campaign_run_id'], self.snapshot)
        state = {'stage': 'failed', 'result': 'failed', 'cleanup_errors': [],
                 'conditions_snapshot': copy.deepcopy(self.snapshot), 'conditions_restore': {'status': 'passed'},
                 'error': 'synthetic covered corpus marker',
                 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
        if hasattr(self, 'alter_failed_state'):
            self.alter_failed_state(state, source)
        atomic_json(source / 'state.json', state)
        (source / 'failed-capture.png').write_bytes(b'original diagnostic fixture')
        return 1

    def use_recovered_failure(self):
        self.fail_plasma = False
        self.begin.side_effect = self.recoverable_failure
        def audit(source, *args, **kwargs):
            state = read_json(source / 'state.json')
            return ({'status': 'failed', 'error': state['error'], 'checks': {'restored': True}}
                    if state.get('result') == 'failed' else {'status': 'passed', 'checks': {'restored': True}})
        self.audit.side_effect = audit

    def test_recovered_failure_continues_other_desktop_and_later_phase_with_failed_report(self):
        self.use_recovered_failure()
        self.assertEqual(self.campaign(phases=['motion', 'overhead'], fast=True), 1)
        data = latest(self.layout)
        self.assertEqual(self.calls, ['vela', 'plasma', 'vela', 'plasma'])
        self.assertEqual([row['status'] for row in data['blocks']], ['failed', 'complete', 'complete', 'complete'])
        self.assertEqual(data['stage'], 'completed-with-failures')
        self.assertTrue(data['execution_complete'])
        self.assertFalse(data['selection_complete'])
        self.assertEqual(data['failed_blocks'], 1)
        root = self.layout.base / 'campaigns' / data['run_id']
        attempt = read_json(root / 'blocks/motion-vela-r1/attempt-0001.json')
        self.assertEqual(attempt['continuation']['restoration'], 'verified')
        self.assertTrue(any(row['event'] == 'continue-after-failure' for row in read_events(root / 'events.jsonl')))
        output = export_campaign(self.layout.path('exports'), data, self.layout)
        self.assertEqual(read_export(output), data)
        self.assertEqual((output / 'motion-vela-r1/failed-capture.png').read_bytes(), b'original diagnostic fixture')
        self.assertIn('synthetic covered corpus marker', (output / 'report.html').read_text())
        from vela_bench.discovery import file_hash
        changed = copy.deepcopy(data); changed['failed_blocks'] = 0
        atomic_json(output / 'summary.json', changed)
        manifest = read_json(output / 'manifest.json')
        manifest['files']['summary.json'] = file_hash(output / 'summary.json')
        atomic_json(output / 'manifest.json', manifest)
        with self.assertRaisesRegex(BenchError, 'failed-block count'):
            read_export(output)

    def test_resume_after_recovered_failure_retries_only_failed_block(self):
        self.use_recovered_failure()
        phases = ['motion', 'overhead']
        self.assertEqual(self.campaign(phases=phases, fast=True), 1)
        data = latest(self.layout)
        root = self.layout.base / 'campaigns' / data['run_id']
        paths = list(root.glob('blocks/*/complete.json'))
        before = {path: path.read_bytes() for path in paths}
        self.begin.side_effect = self.fake_begin
        self.assertEqual(self.campaign(phases=phases, fast=True, resume=data['run_id']), 0)
        self.assertEqual(self.calls, ['vela', 'plasma', 'vela', 'plasma', 'vela'])
        self.assertEqual(before, {path: path.read_bytes() for path in paths})
        self.assertTrue(latest(self.layout)['selection_complete'])
        self.assertEqual(latest(self.layout)['failed_blocks'], 0)

    def test_unverified_restoration_or_live_account_stops_before_another_block(self):
        self.use_recovered_failure()
        for case in ('cleanup-error', 'hardware-restore', 'changed-hardware', 'reboot', 'live-pid', 'input-release'):
            self.calls.clear(); self.live_pids = []
            def alter(state, source):
                if case == 'cleanup-error': state['cleanup_errors'] = ['restore failed']
                elif case == 'hardware-restore': state['conditions_restore']['status'] = 'failed'
                elif case == 'changed-hardware': state['conditions_snapshot']['profile']['mode']['refresh_mhz'] = 144000
                elif case == 'reboot': state['boot_id'] = 'previous boot'
                elif case == 'live-pid': self.live_pids = [42]
                elif case == 'input-release':
                    state['plan'] = {'input_policy': {'version': 'fixture'}}
                    atomic_json(source / 'input-isolation.json', {'release_verified_after_worker_exit': False})
            self.alter_failed_state = alter
            with self.subTest(case=case):
                self.assertEqual(self.campaign(phases=['motion', 'overhead'], fast=True), 1)
                data = latest(self.layout)
                self.assertEqual(self.calls, ['vela'])
                self.assertEqual(data['stage'], 'interrupted')
                self.assertEqual(data['blocks'][1]['status'], 'not-run')

    def test_emergency_abort_is_campaign_cancellation_after_successful_recovery(self):
        self.use_recovered_failure()
        self.alter_failed_state = lambda state, source: state.update(operator_cancelled=True)
        self.assertEqual(self.campaign(phases=['motion', 'overhead'], fast=True), 130)
        self.assertEqual(self.calls, ['vela'])
        data = latest(self.layout)
        self.assertEqual(data['stage'], 'interrupted')
        self.assertEqual(data['blocks'][0]['status'], 'interrupted')
        self.assertEqual(data['blocks'][1]['status'], 'not-run')

    def test_recovered_state_waits_for_systemd_worker_deactivation_before_continuing(self):
        self.use_recovered_failure()
        with patch('vela_bench.campaign.require_workers_stopped', side_effect=[None, BenchError('deactivating'), None]) as idle, \
                patch('vela_bench.campaign.time.sleep') as sleep:
            self.assertEqual(self.campaign(phases=['motion', 'overhead'], fast=True), 1)
        self.assertEqual(idle.call_count, 3)
        sleep.assert_called_once_with(.1)
        self.assertEqual(self.calls, ['vela', 'plasma', 'vela', 'plasma'])

    def test_multiple_phases_follow_planned_order_and_run_three_complete_repetitions(self):
        self.fail_plasma = False
        self.assertEqual(self.campaign(phases=['overhead', 'motion']), 0)
        data = latest(self.layout)
        plan = data['plan']
        self.assertEqual(plan['execution']['phases'], ['motion', 'overhead'])
        self.assertEqual(len(data['blocks']), 12)
        self.assertEqual(self.calls[:6], ['vela', 'plasma', 'plasma', 'vela', 'vela', 'plasma'])
        self.assertEqual(len({opt['run_id'] for opt in self.options}), 12)
        for opt in self.options:
            self.assertTrue(opt['calibrate'] and opt['controlled'] and opt['presentation_probe'])
            self.assertFalse(opt['countdown'])
            self.assertEqual(opt['seconds'], 5)
        self.assertTrue(data['selection_complete'])
        self.assertFalse(data['full_suite_complete'])
        self.assertFalse(data['performance_comparison_available'])
        self.assertEqual(data['measurement_blocks_executed'], 0)
        self.assertEqual(data['qualification']['status'], 'not-requested')
        self.assertEqual(data['families']['motion'], 'exercised-not-measured')
        self.assertEqual(data['families']['responsiveness'], 'not-selected')
        journal = read_events(self.layout.base / 'campaigns' / data['run_id'] / 'events.jsonl')
        self.assertEqual([r['phase'] for r in journal if r['event'] == 'phase-start'], ['motion', 'overhead'])
        self.assertEqual([r['phase'] for r in journal if r['event'] == 'phase-complete'], ['motion', 'overhead'])

    def test_fast_runs_once_without_shortening_per_block_work(self):
        self.fail_plasma = False
        self.assertEqual(self.campaign(fast=True), 0)
        fast = copy.deepcopy(self.options)
        self.options.clear()
        self.assertEqual(self.campaign(), 0)
        self.assertEqual(len(fast), 2)
        self.assertEqual(len(self.options), 6)
        for one, normal in zip(fast, self.options):
            for field in ('seconds', 'presentation_probe', 'controlled', 'calibrate', 'fault', 'countdown'):
                self.assertEqual(one[field], normal[field])

    def test_resume_retains_complete_bytes_and_restarts_only_failed_attempt(self):
        self.assertEqual(self.campaign(fast=True), 1)
        data = latest(self.layout)
        root = self.layout.base / 'campaigns' / data['run_id']
        complete = root / 'blocks/motion-vela-r1/complete.json'
        original = complete.read_bytes()
        failed_id = self.options[-1]['run_id']
        self.fail_plasma = False
        self.assertEqual(self.campaign(fast=True, resume=data['run_id']), 0)
        self.assertEqual(complete.read_bytes(), original)
        self.assertEqual(self.calls, ['vela', 'plasma', 'plasma'])
        self.assertNotEqual(failed_id, self.options[-1]['run_id'])
        self.assertTrue(latest(self.layout)['selection_complete'])

    def test_cancel_is_durable_and_cannot_complete_or_start_another_block(self):
        self.fail_plasma = False
        self.interrupt = True
        self.assertEqual(self.campaign(), 130)
        data = latest(self.layout)
        self.assertEqual(data['stage'], 'interrupted')
        self.assertEqual([b['status'] for b in data['blocks'][:3]], ['complete', 'interrupted', 'not-run'])
        failed_id = self.options[-1]['run_id']
        self.interrupt = False
        self.assertEqual(self.campaign(resume=data['run_id']), 0)
        self.assertNotIn(failed_id, [opt['run_id'] for opt in self.options[2:]])

    def test_resume_rejects_different_selection_mode_seed_before_new_session(self):
        self.campaign(fast=True)
        run_id = latest(self.layout)['run_id']
        before = list(self.calls)
        for change in ({'phases': ['overhead']}, {'fast': False}, {'seed': 1}):
            with self.subTest(change=change), self.assertRaisesRegex(BenchError, 'identical'):
                self.campaign(**dict({'fast': True, 'resume': run_id}, **change))
        self.assertEqual(self.calls, before)

    def test_changed_receipt_does_not_publish_completion(self):
        self.fail_plasma = False
        self.corrupt_receipt = True
        self.assertEqual(self.campaign(fast=True), 1)
        data = latest(self.layout)
        self.assertEqual(data['blocks'][0]['status'], 'failed')
        self.assertFalse(data['blocks'][0]['audit']['checks']['fixed_workload_complete'])
        self.assertEqual(self.calls, ['vela'])
        self.assertFalse(data['selection_complete'])

    def test_context_change_stops_before_next_session_and_preserves_accepted_block(self):
        self.fail_plasma = False
        self.change_context = True
        self.assertEqual(self.campaign(fast=True), 1)
        data = latest(self.layout)
        self.assertEqual(data['blocks'][0]['status'], 'complete')
        self.assertIn('environment changed', data['blocks'][1]['error'])
        self.assertEqual(self.calls, ['vela'])

    def test_root_request_refuses_other_target_and_unsafe_block_name(self):
        self.campaign(fast=True)
        run_id = latest(self.layout)['run_id']
        for name, target in [('motion-vela-r1', 'plasma'), ('../../motion-vela-r1', 'vela')]:
            with self.subTest(name=name), self.assertRaises(BenchError):
                block_request(self.layout, run_id, name, target)

    def test_exercise_export_round_trip_and_no_family_success_claim(self):
        self.fail_plasma = False
        self.campaign(fast=True)
        data = latest(self.layout)
        output = export_campaign(self.layout.path('exports'), data, self.layout)
        self.assertEqual(read_export(output), data)
        document = (output / 'report.html').read_text()
        self.assertIn('Zero measurement blocks executed', document)
        self.assertIn('exercised-not-measured', document)
        self.assertEqual(read_json(output / 'manifest.json')['kind'], 'execution-campaign-export')
        self.assertTrue((output / 'campaign-events.jsonl').is_file())
        self.assertTrue((output / 'attempts/motion-vela-r1-complete.json').is_file())
        self.assertTrue((output / 'attempts/motion-plasma-r1-attempt-0001.json').is_file())
        (output / 'motion-vela-r1/block-workload.json').write_text('{}')
        with self.assertRaisesRegex(BenchError, 'evidence changed'):
            read_export(output)

    def worker_fixture(self):
        self.campaign(fast=True)
        data = latest(self.layout)
        source = self.layout.base / 'pilots' / data['blocks'][0]['pilot_run_id']
        state = read_json(source / 'state.json')
        state.update(stage='queued', result='pending', target='vela', target_vt=3,
                     display_manager_was_active=True, restore_required=False,
                     plan={'targets': {'vela': {}}, 'seconds_per_target': 0,
                           'controlled_conditions': True, 'instrument_calibration': True,
                           'presentation_probe': True})
        atomic_json(source / 'state.json', state)
        atomic_json(self.layout.base / 'active.json', {'run_id': data['blocks'][0]['pilot_run_id']})
        (source / 'block-workload.json').unlink()
        self.layout.home.mkdir()
        return source

    def test_worker_receipt_is_published_only_after_full_fixed_controls(self):
        from vela_bench.pilot import worker
        from unittest.mock import Mock
        source = self.worker_fixture()
        record = {'uid': 1001, 'gid': 1001}
        input_call = Mock(side_effect=[
            {'status': 'passed', 'session_accounting': {'status': 'passed'},
             'metrics': {'injection_to_presentation_upper_ms': {'p50': value}}}
            for value in (5, 6, 45)])
        controls = {
            'load_setup': {'return_value': record}, 'create_fixture': {},
            'logind_sessions': {'return_value': []}, 'snapshot_conditions': {'return_value': self.snapshot},
            'reserve_vt': {}, 'session_command': {'return_value': ['fixed-fixture-session']},
            'resolve_targets': {'return_value': {'vela': {}}},
            'stop_user': {'return_value': None}, 'reset_profile': {}, 'wait_session': {'return_value': {}},
            'verify_session': {}, 'apply_conditions': {'return_value': {'status': 'passed'}},
            'verify_conditions': {}, 'run_probe': {'return_value': {'status': 'complete'}},
            'observe_session': {},
            'preserve_session_log': {}, 'os.chown': {}, 'trusted': {},
        }
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch('vela_bench.host.trusted'))
            guard = stack.enter_context(patch('vela_bench.pilot.InputIsolation')).return_value
            guard.error = None
            guard.evidence.return_value = {'status': 'released'}
            for name, opts in controls.items():
                stack.enter_context(patch('vela_bench.pilot.' + name, **opts))
            stack.enter_context(patch('vela_bench.pilot.run_input', input_call))
            worker(SimpleNamespace(run=Mock(return_value=SimpleNamespace(stdout=''))), self.layout)
            guard.start.assert_called_once()
            guard.close.assert_called_once()
        receipt = read_json(source / 'block-workload.json')
        self.assertEqual(receipt['status'], 'passed')
        self.assertEqual(receipt['block']['id'], 'motion-vela-r1')
        self.assertFalse(receipt['measurement_executed'])
        self.assertEqual([call.kwargs['delay'] for call in input_call.call_args_list], [0, 0, 40])
        self.assertEqual(input_call.call_args_list[1].kwargs['capture_profile'], self.snapshot['profile'])
        self.assertEqual(read_json(source / 'state.json')['result'], 'passed')

    def test_worker_rejects_changed_binding_before_touching_hardware(self):
        from vela_bench.pilot import worker
        source = self.worker_fixture()
        state = read_json(source / 'state.json')
        state['campaign_block']['block']['repetition'] = 99
        atomic_json(source / 'state.json', state)
        with patch('vela_bench.pilot.load_setup', return_value={}), patch('vela_bench.host.trusted'), patch('vela_bench.pilot.trusted'), \
                patch('vela_bench.pilot.snapshot_conditions') as snapshot, \
                patch('vela_bench.pilot.create_fixture') as fixture:
            with self.assertRaisesRegex(BenchError, 'request changed'):
                worker(SimpleNamespace(), self.layout)
        snapshot.assert_not_called()
        fixture.assert_not_called()
        self.assertFalse((source / 'block-workload.json').exists())
        self.assertFalse(read_json(source / 'state.json')['restore_required'])


class ExerciseCLI(unittest.TestCase):
    def inventory(self):
        from test_core import host
        return host()

    def test_plan_clearly_distinguishes_exercise_from_future_measurements(self):
        config, inventory = load_campaign(), self.inventory()
        normal = make_plan(config, inventory, phases=['motion'], exercise=True)
        fast = make_plan(config, inventory, phases=['motion'], exercise=True, fast=True)
        self.assertEqual(fast['blocks'], [b for b in normal['blocks'] if b['repetition'] == 1])
        self.assertEqual(normal['scenario_revision'], 'p1-instrument-controls-v1')
        self.assertTrue(all(not b['measurement_executed'] and b['measurement_budget_seconds'] == 0 for b in normal['blocks']))
        for phase in ('check', 'calibrate', 'report'):
            with self.assertRaises(BenchError):
                make_plan(config, inventory, phases=[phase], exercise=True)

    def test_cli_passes_only_fixed_phase_selections_to_installed_launcher(self):
        with patch('vela_bench.cli.discover', return_value=self.inventory()), \
                patch('vela_bench.cli.require_root'), patch('vela_bench.cli.trusted'), \
                patch('vela_bench.cli.require_current_launcher'), \
                patch('vela_bench.cli.subprocess.call', return_value=0) as call, \
                patch('vela_bench.cli.export_latest_campaign') as export, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(['run', '--exercise', '--phase', 'overhead', '--phase', 'motion',
                                   '--target', 'vela', '--fast', '--yes']), 0)
        command = call.call_args.args[0]
        self.assertIn('--exercise', command)
        self.assertIn('--fast', command)
        self.assertEqual([command[i+1] for i, x in enumerate(command) if x == '--phase'], ['motion', 'overhead'])
        self.assertEqual([command[i+1] for i, x in enumerate(command) if x == '--target'], ['vela'])
        export.assert_called_once()

    def test_exercise_does_not_bypass_missing_target_check(self):
        with patch('vela_bench.cli.discover', return_value=self.inventory()), \
                patch('vela_bench.cli.subprocess.call') as call, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(['run', '--exercise', '--target', 'gnome']), 2)
        call.assert_not_called()


if __name__ == '__main__':
    unittest.main()
