# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed runtime mode, sanitized entry and immutable export profile boundaries."""
import copy
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError, campaign, cli
from vela_bench.archive import atomic_json
from vela_bench.discovery import file_hash
from vela_bench.host import ACCOUNT, Layout
from vela_bench.model import load_campaign, make_plan, vela_environment
from vela_bench.pilot import desktop_environment, runtime_environment, session_command
from test_core import host


class SyncProfile(unittest.TestCase):
    def test_profile_changes_fingerprint_and_not_workloads_or_plasma(self):
        inventory = host()
        original = copy.deepcopy(inventory)
        for fast in (False, True):
            standard = make_plan(load_campaign(), inventory, fast=fast)
            sync = make_plan(load_campaign(), inventory, fast=fast, vela_sync_test=True)
            self.assertNotEqual(standard['fingerprint'], sync['fingerprint'])
            for key in ('blocks', 'targets', 'display_policy', 'mode', 'repetitions'):
                self.assertEqual(standard[key], sync[key])
            self.assertEqual(sync['vela_runtime_environment'], vela_environment(True))
        self.assertEqual(inventory, original)
        for case in ('calibrate', 'observe'):
            selection = campaign.execution_selection(inventory, ['vela','plasma'], case=case,
                fast=False, seed=0, phases=['motion'], exercise=False, vela_sync_test=True)
            self.assertTrue(selection['vela_sync_test'])
            self.assertEqual(len(selection['blocks']), 6)

    def test_invalid_selection_and_nonboolean_modes_refuse(self):
        with self.assertRaisesRegex(BenchError, 'requires the Vela target'):
            make_plan(load_campaign(), host(), targets=['plasma'], vela_sync_test=True)
        for value in (1, 'yes', None, {}):
            with self.subTest(value=value), self.assertRaises(BenchError):
                vela_environment(value)

    def test_entry_uses_only_explicit_flags_and_drops_inherited_operator_values(self):
        user = SimpleNamespace(pw_uid=os.getuid(), pw_name=ACCOUNT, pw_dir='/reserved', pw_shell='/bin/sh')
        inherited = {'XDG_SESSION_ID':'test', 'XDG_SEAT':'seat0', 'XDG_VTNR':'4',
                     'VELA_SCANOUT':'0', 'VELA_READY_WAIT':'0', 'LD_PRELOAD':'untrusted'}
        for target in ('vela', 'plasma'):
            normal = desktop_environment(target, user, inherited)
            self.assertFalse(set(vela_environment(True)) & normal.keys())
        active = desktop_environment('vela', user, dict(inherited, VELA_SCANOUT='1'), vela_sync_test=True)
        self.assertTrue(all(active.get(key) == value for key,value in vela_environment(True).items()))
        self.assertNotIn('LD_PRELOAD', active)
        with self.assertRaisesRegex(BenchError, 'only be applied to Vela'):
            desktop_environment('plasma', user, inherited, vela_sync_test=True)

    def test_session_command_passes_fixed_entry_argument_and_refuses_plasma_flags(self):
        record = {'account_tag':'test', 'uid':1240, 'private_homes':[]}
        normal = session_command('vela', 4, record)
        sync = session_command('vela', 4, record, vela_sync_test=True)
        self.assertEqual(sync, normal + ['--vela-sync-test'])
        with self.assertRaises(BenchError):
            session_command('plasma', 4, record, vela_sync_test=True)

    def test_runtime_receipt_rejects_missing_changed_or_extra_flags(self):
        plan = {'vela_sync_test':True, 'vela_runtime_environment':vela_environment(True)}
        self.assertEqual(runtime_environment(plan, 'vela'), vela_environment(True))
        self.assertEqual(runtime_environment(plan, 'plasma'), {})
        for flags in ({}, {'VELA_SCANOUT':'0'}, dict(vela_environment(True), LD_PRELOAD='x'),
                      dict(vela_environment(True), VELA_READY_WAIT='1')):
            with self.subTest(flags=flags), self.assertRaises(BenchError):
                runtime_environment(dict(plan, vela_runtime_environment=flags), 'vela')

    def export(self, root, target='vela', enabled=True):
        flags = vela_environment(True)
        plan = {'case':'exercise', **({'vela_sync_test':True, 'execution':{'vela_runtime_environment':flags}} if enabled else {})}
        active = enabled and target == 'vela'
        block = {'id':'motion-' + target + '-r1', 'target':target, 'status':'complete',
                 'audit':{'status':'passed', 'checks':{'vela_runtime_profile':True} if active else {}, 'calibration':None}}
        directory = root / block['id']; directory.mkdir()
        state = {'plan':{'vela_sync_test':True, 'vela_runtime_environment':flags} if active else {},
                 'campaign_block':{'vela_runtime_environment':flags} if active else {}}
        session = {'vela_runtime_environment':flags if active else {}}
        data = {'schema':1, 'kind':'execution-campaign', 'run_id':'a'*32, 'full_suite_complete':False,
                'selection_complete':True, 'plan':plan, 'blocks':[block]}
        atomic_json(root / 'summary.json', data)
        atomic_json(root / 'campaign-plan.json', plan)
        atomic_json(directory / 'state.json', state)
        atomic_json(directory / (target+'-session.json'), session)
        return block

    def reopen(self, root, block):
        files = {str(p.relative_to(root)):file_hash(p) for p in root.glob('**/*.json') if p.name!='manifest.json'}
        atomic_json(root / 'manifest.json', {'schema':1, 'kind':'execution-campaign-export', 'run_id':'a'*32, 'files':files})
        with patch.object(campaign, 'audit_pilot', return_value=block['audit']):
            return campaign.read_export(root)

    def test_reopened_export_accepts_normal_and_each_correct_profile_role(self):
        for enabled,target in ((False,'vela'), (True,'vela'), (True,'plasma')):
            with self.subTest(enabled=enabled,target=target), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                block = self.export(root,target,enabled)
                self.assertTrue(self.reopen(root,block)['selection_complete'])

    def test_resigned_exports_cannot_hide_profile_or_lose_or_leak_flags(self):
        for case in ('summary-label', 'both-labels', 'pilot-label', 'live-flags', 'block-request', 'plasma-leak'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); target = 'plasma' if case=='plasma-leak' else 'vela'
                block = self.export(root,target)
                if case in ('summary-label','both-labels'):
                    path = root / 'summary.json'; data=json.loads(path.read_text());data['plan'].pop('vela_sync_test')
                    atomic_json(path,data)
                    if case=='both-labels':atomic_json(root/'campaign-plan.json',data['plan'])
                elif case in ('pilot-label','block-request'):
                    path=root/block['id']/'state.json';data=json.loads(path.read_text())
                    data['plan' if case=='pilot-label' else 'campaign_block']={}
                    atomic_json(path,data)
                else:
                    path=root/block['id']/(target+'-session.json');data=json.loads(path.read_text())
                    data['vela_runtime_environment']=vela_environment(True) if case=='plasma-leak' else {}
                    atomic_json(path,data)
                with self.assertRaises(BenchError):self.reopen(root,block)
