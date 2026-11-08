# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Combined CLI sequencing; root/TTY/process boundaries are fixtures only."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError, cli
from vela_bench.host import Layout
from test_core import host


def complete(failed=False, case='measure', run_id='a'*32):
    return {'stage': 'completed-with-failures' if failed else 'done',
            'execution_complete': True, 'selection_complete': not failed,
            'plan': {'case':case}, 'run_id':run_id}


class RunAll(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.layout = Layout(Path(self.folder.name))
        self.layout.base.mkdir(parents=True)
        (self.layout.base / 'active.json').write_text('{}')
        (self.layout.base / 'campaign-active.json').write_text(json.dumps({'run_id':'0'*32}))
        self.commands = []
        self.statuses, self.results = [0, 0], [complete(), complete(case='observe',run_id='b'*32)]
        self.recovery_status = 0
        self.native = SimpleNamespace(run=lambda *args, **kwargs: SimpleNamespace(stdout='2\n'))
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        def patched(name, **options):
            return self.stack.enter_context(patch(name, **options))
        patched('vela_bench.cli.discover', return_value=host())
        patched('vela_bench.cli.require_root')
        patched('vela_bench.cli.Layout', return_value=self.layout)
        patched('vela_bench.cli.Host', return_value=self.native)
        self.tty = patched('vela_bench.cli.tty_preconditions', return_value={'origin_vt':2, 'origin_session':'test'})
        self.update = patched('vela_bench.cli.update_launcher')
        patched('vela_bench.cli.trusted')
        patched('vela_bench.cli.require_current_launcher')
        patched('vela_bench.cli.load_setup', return_value={})
        patched('vela_bench.campaign.context', return_value=host())
        self.preflight = patched('vela_bench.campaign.require_selection_instruments')
        patched('vela_bench.cli.subprocess.call', side_effect=self.call)
        patched('vela_bench.cli.subprocess.run', side_effect=self.result)
        self.export = patched('vela_bench.cli.export_campaign', return_value=Path(self.folder.name))
        self.stack.enter_context(patch.dict('os.environ', {'SUDO_UID':'', 'SUDO_GID':''}))

    def call(self, command, **options):
        self.commands.append(command)
        if command[-1] == 'recover':return self.recovery_status
        status=self.statuses.pop(0)
        if status in (0,1) and self.results and isinstance(self.results[0],dict):
            (self.layout.base / 'campaign-active.json').write_text(json.dumps({'run_id':self.results[0]['run_id']}))
        return status

    def result(self, command, **options):
        self.assertEqual(command[-1], 'campaign-results')
        return SimpleNamespace(returncode=0, stdout=json.dumps(self.results.pop(0)))

    def invoke(self, *args):
        self.output = io.StringIO()
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(io.StringIO()):
            return cli.main(['run-all', *args])

    def test_default_preview_schedules_all_48_blocks_without_mutations(self):
        self.assertEqual(self.invoke('--dry-run'), 0)
        plan = json.loads(self.output.getvalue())
        self.assertEqual((plan['repetitions'], plan['measure']['blocks'], plan['observe']['blocks']), (3,30,18))
        self.assertEqual(plan['total_blocks'], 48)
        self.assertEqual(self.invoke('--dry-run', '--fast'), 0)
        self.assertEqual(json.loads(self.output.getvalue())['total_blocks'], 16)
        self.update.assert_not_called()
        self.assertFalse(self.commands)

    def test_one_start_updates_recovers_preflights_both_and_runs_in_order(self):
        with patch('builtins.input', return_value='START') as authorize:
            self.assertEqual(self.invoke(), 0)
        authorize.assert_called_once()
        self.update.assert_called_once()
        self.assertEqual(self.commands[0][-1], 'recover')
        self.assertEqual(len(self.commands), 3)
        self.assertEqual([cmd[cmd.index('--case')+1] for cmd in self.commands[1:]], ['calibrate','observe'])
        self.assertTrue(all('--yes' in cmd and '--fast' not in cmd for cmd in self.commands[1:]))
        self.assertEqual([call.args[-1]['case'] for call in self.preflight.call_args_list], ['measure','observe'])
        self.assertEqual(self.export.call_count, 2)

    def test_eof_never_updates_recovers_or_starts_campaigns(self):
        with patch('builtins.input', side_effect=EOFError):
            self.assertEqual(self.invoke(), 2)
        self.update.assert_not_called()
        self.assertFalse(self.commands)

    def test_completed_recovered_failures_do_not_skip_p3_or_become_success(self):
        self.statuses = [1,0]
        self.results = [complete(True), complete(case='observe',run_id='b'*32)]
        self.assertEqual(self.invoke('--yes'), 1)
        self.assertEqual(len(self.commands), 3)

    def test_interruption_incomplete_restore_or_missing_result_never_starts_p3(self):
        for status, result in ((130,None), (1,dict(complete(True), stage='interrupted')),
                               (1,dict(complete(True), execution_complete=False)), (0,None),
                               (1,complete())):
            with self.subTest(status=status, result=result):
                self.commands.clear()
                self.statuses, self.results = [status], [result]
                self.assertNotEqual(self.invoke('--yes'), 0)
                self.assertEqual(len(self.commands), 2)

    def test_previous_campaign_export_cannot_authorize_p3_after_early_failure(self):
        self.statuses=[1]
        self.results=[complete(True,run_id='0'*32)]
        self.assertEqual(self.invoke('--yes'),2)
        self.assertEqual(len(self.commands),2)

    def test_failed_recovery_or_missing_p3_instruments_stop_before_measurements(self):
        self.recovery_status = 2
        self.assertEqual(self.invoke('--yes'), 2)
        self.assertEqual(len(self.commands), 1)
        self.commands.clear()
        self.recovery_status = 0
        def inspect(*args):
            if args[-1]['case'] == 'observe':raise BenchError('missing P3 scene')
        self.preflight.side_effect = inspect
        self.assertEqual(self.invoke('--yes'), 2)
        self.assertEqual(len(self.commands), 1)

    def test_changed_caller_session_prevents_second_campaign(self):
        original = {'origin_vt':2, 'origin_session':'test'}
        self.tty.side_effect = [original,original,dict(original,origin_session='replacement')]
        self.assertEqual(self.invoke('--yes'), 2)
        self.assertEqual(len(self.commands), 2)

    def test_first_setup_without_a_previous_pilot_skips_recovery(self):
        (self.layout.base / 'active.json').unlink()
        self.assertEqual(self.invoke('--yes'), 0)
        self.assertEqual(len(self.commands), 2)
        self.assertTrue(all('campaign' in cmd for cmd in self.commands))
