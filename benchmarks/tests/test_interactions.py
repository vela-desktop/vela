# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from vela_bench import BenchError, interactions


def transcript(target='vela',profile=None,begin=12_000_000_000):
    profile=profile or {'mode':{'width':1920,'height':1080}}
    def pattern(sequence,stamp,maximized):
        w,h=(profile['mode']['width'],int(profile['mode']['height']*.9)) if maximized else (401,301)
        return {'event':'pattern','method':'coordinate-pattern-v1','protocol':'wayland','viewport':True,
            'sequence':sequence,'monotonic_ns':stamp,'scale120':120,'logical_width':w,'logical_height':h,
            'buffer_width':w,'buffer_height':h,'maximized':maximized}
    initial=pattern(1,begin,False)
    row={'event':'window-interaction-complete','method':interactions.METHOD,'target':target,'spec':interactions.SPEC,
        'begin_monotonic_ns':begin,'keyboard_ready':{'event':'uinput-ready','method':'linux-uinput-fixed-desktop-v1',
        'clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'},
        'initial':initial,'pattern_events':[initial],'actions':[]}
    for i in range(24):
        before=begin+(i+1)*1_000_000_000
        state=pattern(i+2,before+100,i%2==0)
        operation='maximize' if i%2==0 else 'restore'
        row['pattern_events'].append(state)
        row['actions'].append({'operation':operation,'pattern':state,'verified_monotonic_ns':before+200,
            'injection':{'event':'injected','action_id':i+1,'command':operation if target=='vela' else 'maximize-toggle',
                'before_monotonic_ns':before,'after_monotonic_ns':before+50}})
    row['end_monotonic_ns']=begin+25_000_000_000
    return row


class WindowInteractions(unittest.TestCase):
    profile={'mode':{'width':1920,'height':1080}}

    def test_native_receipts_establish_all_actions_without_capture_or_latency_claims(self):
        for target in ('vela','plasma'):
            result=interactions.audit(json.dumps(transcript(target)),target,self.profile)
            self.assertEqual(result['verified_actions'],24)
            self.assertFalse(result['capture_enabled']);self.assertFalse(result['latency_measurement'])

    def test_incomplete_wrong_backend_scale_state_receipt_or_cadence_is_rejected(self):
        for case in ('count','order','backend','scale','state','receipt','source','cadence','clock','input','geometry',
                     'shape','device','start'):
            row=copy.deepcopy(transcript('plasma'))
            action=row['actions'][0]
            if case=='count':row['actions'].pop()
            if case=='order':row['actions'].reverse()
            if case=='backend':action['pattern']['protocol']='x11'
            if case=='scale':action['pattern']['scale120']=150
            if case=='state':action['pattern']['maximized']=False
            if case=='receipt':action['injection']['command']='maximize'
            if case=='source':row['pattern_events'].pop(1)
            if case=='cadence':row['actions'][1]['injection']['before_monotonic_ns']=action['injection']['before_monotonic_ns']+100
            if case=='clock':action['pattern']['monotonic_ns']=1
            if case=='input':row['keyboard_ready']['method']='unsupported'
            if case=='geometry':action['pattern']['buffer_width']=200
            if case=='shape':row['actions'][0]=None
            if case=='device':row['keyboard_ready']['sysfs']='unknown'
            if case=='start':row['begin_monotonic_ns']-=100
            with self.subTest(case=case),self.assertRaises(BenchError):
                interactions.audit(json.dumps(row),'plasma',self.profile)

    def test_resource_window_must_cover_the_entire_interaction_transcript(self):
        from test_platform_workloads import sample
        from vela_bench.platform_workloads import resource_metrics
        # Intermediate samples maintain the fixed inventory gap bound.
        rows=[sample(i*1_000_000_000,100000+i) for i in range(11,39)]
        raw=json.dumps(transcript())
        result=resource_metrics(rows,1001,'window-interaction',raw,target='vela',profile=self.profile)
        self.assertEqual(result['workload']['cycles'],12)
        row=transcript();row['end_monotonic_ns']=39_000_000_000
        with self.assertRaisesRegex(BenchError,'cover the completed workload'):
            resource_metrics(rows,1001,'window-interaction',json.dumps(row),target='vela',profile=self.profile)

    def test_partial_native_failure_evidence_is_kept_without_a_completion(self):
        from vela_bench.archive import read_json
        with tempfile.TemporaryDirectory() as temporary:
            failure=Path(temporary)/'failure.json'
            class Broken:
                records=[]
                pending=b''
                def next(self,deadline):raise BenchError('fixture early client exit')
            with patch('vela_bench.interactions.Lines',return_value=Broken()):
                with self.assertRaisesRegex(BenchError,'early client exit'):
                    interactions.run(SimpleNamespace(),None,'vela',self.profile,{},None,failure)
            saved=read_json(failure)
            self.assertEqual(saved['status'],'failed')
            self.assertEqual(saved['completed_actions'],[])
            self.assertEqual(saved['pattern_events'],[])

    def test_failed_input_start_keeps_the_original_error_and_unverified_cleanup(self):
        from unittest.mock import Mock
        from vela_bench.archive import read_json
        from vela_bench.host import Layout
        from vela_bench.platform_workloads import resource_condition
        from test_platform_workloads import sample
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);host=Mock()
            host.run.return_value=SimpleNamespace(returncode=0,stdout='inactive\n',stderr='')
            with patch('vela_bench.resources.snapshot',return_value=sample()), \
                 patch('vela_bench.platform_workloads.stop_process'), \
                 patch('vela_bench.platform_workloads.Lines') as lines:
                lines.return_value.next.side_effect=BenchError('fixture uinput creation failed')
                with self.assertRaisesRegex(BenchError,'fixture uinput creation failed'):
                    resource_condition(host,{'uid':1001,'gid':1001},root,'vela',Layout(root),
                                       'window-interaction',lambda:None,None,self.profile)
            source=read_json(root/'vela-resources-window-interaction.json')
            self.assertEqual(source['interaction_input_cleanup'],'unverified')
            self.assertFalse((root/'vela-resources-window-interaction.work.jsonl').exists())
