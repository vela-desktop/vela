# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest

from vela_bench import BenchError
from vela_bench.archive import read_json
from vela_bench.host import Layout,HostOperationError
from vela_bench.pilot import reserve_vt,launch_session


class VTReservation(unittest.TestCase):
    def test_alias_canonical_mask_state_does_not_skip_the_second_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);layout=Layout(root)
            state={'target_vt':4,'masked_units':[]};calls=[]
            class Host:
                def run(self,command,**options):
                    calls.append(command)
                    if command[1]=='mask':
                        # Match the files systemctl actually creates. The
                        # alias query below deliberately repeats canonical state.
                        path=layout.path('run/systemd/system')/command[-1]
                        path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to('/dev/null')
                    masks=list(layout.path('run/systemd/system').glob('*.service'))
                    return SimpleNamespace(stdout='ActiveState=inactive\nUnitFileState='+('masked-runtime' if masks else 'disabled')+'\nLoadState=masked\n')
            reserve_vt(Host(),root,state,layout)
            self.assertEqual(state['masked_units'],['getty@tty4.service','autovt@tty4.service'])
            self.assertEqual([c[-1] for c in calls if c[1]=='mask'],state['masked_units'])
            self.assertEqual(read_json(root/'state.json')['masked_units'],state['masked_units'])

    def test_missing_mask_and_post_mask_activity_abort_before_switching(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);layout=Layout(root)
            class Host:
                def run(self,command,**options):
                    return SimpleNamespace(stdout='ActiveState=inactive\nUnitFileState=disabled\n')
            with self.assertRaisesRegex(BenchError,'mask was not created'):
                reserve_vt(Host(),root,{'target_vt':4,'masked_units':[]},layout)
            class LateHost:
                def run(self,command,**options):
                    if command[1]=='mask':
                        path=layout.path('run/systemd/system')/command[-1]
                        path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to('/dev/null')
                    return SimpleNamespace(stdout=('ActiveState=active\nLoadState=masked\n' if '--property=LoadState' in command
                                                   else 'ActiveState=inactive\nUnitFileState=disabled\n'))
            with self.assertRaisesRegex(BenchError,'reservation did not hold'):
                reserve_vt(LateHost(),root,{'target_vt':4,'masked_units':[]},layout)

    def test_launch_failure_preserves_pre_exec_journal_and_exit_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);layout=Layout(root);directory=layout.base/'pilots'/('a'*32)
            directory.mkdir(parents=True)
            record={'uid':1001,'account_tag':'fixture','private_homes':[]}
            class Host:
                def run(self,command,**options):
                    if command[0]=='systemd-run':raise HostOperationError(command,1)
                    if command[0]=='systemctl':return SimpleNamespace(stdout='ExecMainStatus=208\nResult=exit-code\n',returncode=0)
                    return SimpleNamespace(stdout='Failed to set up standard input: Operation not permitted\n',returncode=0)
            from unittest.mock import patch
            with patch('vela_bench.pilot.trusted'),self.assertRaisesRegex(BenchError,'ExecMainStatus=208.*standard input') as caught:
                launch_session(Host(),'vela',4,record,layout,directory)
            self.assertIsInstance(caught.exception.__cause__,HostOperationError)
            evidence=read_json(directory/'vela-launch-failure.json')
            self.assertEqual(evidence['properties']['ExecMainStatus'],'208')

    def test_failed_pilot_is_not_replaced_by_a_missing_workload_receipt_error(self):
        from unittest.mock import patch
        from vela_bench.archive import atomic_json
        from vela_bench.campaign import audit_block
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)
            atomic_json(source/'state.json',{'error':'session failed: 208/STDIN'})
            with patch('vela_bench.campaign.audit_pilot',return_value={'status':'failed','checks':{'successful_run':False}}), \
                 patch('vela_bench.campaign.block_request',return_value={}):
                audit=audit_block(source,{'id':'scaling-vela-r1','target':'vela','phase':'scaling','fault':None},
                                  {'kind':'execution-campaign','case':'measure','run_id':'a'*32},Layout(source))
            self.assertEqual(audit['status'],'failed')
            self.assertEqual(audit['error'],'session failed: 208/STDIN')
