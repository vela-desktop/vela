# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""P2 sample budgets, raw-data rejection, GPU load evidence and independent blocks."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json
from vela_bench.campaign import execution_selection
from vela_bench.measurements import (SPEC, VERSION, aggregate, input_metrics, motion_metrics,
    producer_metrics, paired_metrics, audit_measurements, INPUT_WARMUP, INPUT_SAMPLES,
    MOTION_WARMUP, MOTION_SAMPLES)


def native(rows):
    next(row for row in rows if row['event']=='probe').update(
        display_protocol='wayland', window_protocol='xdg-shell', buffer_backend='wl_shm',buffer_width=640,buffer_height=360)
    return rows


def input_rows():
    from test_calibration import samples
    rows, injections = samples(count=INPUT_WARMUP + INPUT_SAMPLES)
    return native(rows), injections


def motion_rows():
    from test_diagnostics import stream
    rows = native(stream(frames=MOTION_WARMUP + MOTION_SAMPLES))
    return ''.join(json.dumps(row)+'\n' for row in rows), rows


def gpu_rows(mode='explicit', frames=10):
    rows = [{'event':'producer', 'schema':1, 'method':'delayed-gpu-acquire-v1', 'sync':mode,
             'requested_delay_ms':150, 'contention_excluded':False, 'vendor_id':4098, 'device_id':1,
             'drm_major':226, 'drm_minor':128, 'queue_family':0, 'timestamp_valid_bits':64, 'timestamp_period_ns':1, 'buffer_width':1024, 'buffer_height':640},
            {'event':'calibrated', 'iterations':1000, 'wall_duration_ms':150}]
    for frame in range(1,frames+1):
        stamp=frame*150_000_000
        rows.extend([{'event':'producer-commit', 'frame':frame, 'monotonic_ns':stamp, 'fence_pending':True},
                     {'event':'gpu-complete', 'frame':frame, 'gpu_duration_ms':150.,
                      'submitted_monotonic_ns':stamp-100, 'observed_monotonic_ns':stamp+150_000_000},
                     {'event':'producer-presented','frame':frame,'received_monotonic_ns':stamp+160_000_000,'flags':1}])
    rows.append({'event':'producer-summary','complete':True,'submitted':frames,'completed':frames,'presented':frames,'discarded':0,'monotonic_ns':frames*150_000_000+200_000_000})
    return rows


class Metrics(unittest.TestCase):
    def test_input_warmup_is_excluded_but_all_105_actions_are_validated(self):
        rows, injections=input_rows()
        result=input_metrics(rows,injections)
        self.assertEqual(result['actions'],100)
        self.assertEqual(result['injection_to_presentation_upper_ms']['samples'],100)
        self.assertEqual(result['client_receipt_to_presentation_ms']['p50'],4)
        self.assertFalse(result['p99_available'])
        self.assertEqual(result['window_monotonic_ns'][0],injections[5]['before_monotonic_ns'])
        broken=copy.deepcopy(rows)
        broken.remove(next(r for r in broken if r.get('event')=='input' and r['action_id']==1))
        with self.assertRaises(BenchError): input_metrics(broken,injections)

    def test_bad_native_backend_and_warmup_counts_cannot_pass(self):
        rows,injections=input_rows()
        next(r for r in rows if r['event']=='probe')['display_protocol']='x11'
        with self.assertRaises(BenchError): input_metrics(rows,injections)
        rows,injections=input_rows()
        with self.assertRaises(BenchError): input_metrics(rows,injections[:-1])

    def test_motion_excludes_warmup_and_keeps_discard_counts_and_refresh_gaps(self):
        raw,rows=motion_rows()
        profile={'mode':{'refresh_mhz':60000}}
        result=motion_metrics(raw,rows,profile)
        self.assertEqual(result['submitted'],600)
        self.assertEqual(result['presented'],600)
        self.assertEqual(result['presentation_intervals_ms']['samples'],599)
        self.assertEqual(result['gaps_over_1_5_refresh_periods'],0)
        frame=next(i for i,r in enumerate(rows) if r['event']=='presented' and r['frame']==100)
        rows[frame]={'event':'discarded','frame':100,'received_monotonic_ns':rows[frame]['received_monotonic_ns']}
        rows[-1].update(presented=629,discarded=1)
        result=motion_metrics(''.join(json.dumps(r)+'\n' for r in rows),rows,profile)
        self.assertEqual(result['discarded'],1)
        self.assertEqual(result['presented'],599)
        self.assertEqual(result['gaps_over_1_5_refresh_periods'],1)
        self.assertIsNone(result['missed_kernel_vblanks'])

    def test_motion_rejects_wrong_refresh_and_truncated_or_unordered_feedback(self):
        raw,rows=motion_rows()
        for profile in ({'mode':{'refresh_mhz':30000}},):
            with self.assertRaises(BenchError):motion_metrics(raw,rows,profile)
        with self.assertRaises(BenchError):motion_metrics(raw[:-1],rows,{'mode':{'refresh_mhz':60000}})
        shown=next(r for r in rows if r['event']=='presented' and r['frame']==50)
        shown['presentation_ns']=0
        with self.assertRaises(BenchError):motion_metrics(''.join(json.dumps(r)+'\n' for r in rows),rows,{'mode':{'refresh_mhz':60000}})

    def test_gpu_requires_real_job_timestamps_fences_and_window_coverage(self):
        result=producer_metrics(gpu_rows(),'explicit',[400_000_000,1_500_000_000])
        self.assertEqual(result['completed'],10)
        self.assertEqual(result['gpu_duration_ms']['p50'],150)
        self.assertFalse(result['contention_excluded'])
        for case in ('ready-fence','missing-completion','no-gpu-delay','wrong-sync','window','clock','wrong-device','partial'):
            rows=gpu_rows();window=[400_000_000,1_500_000_000]
            if case=='ready-fence':rows[2]['fence_pending']=False
            if case=='missing-completion':rows.pop(3)
            if case=='no-gpu-delay':
                for r in rows:
                    if r['event']=='gpu-complete':r['gpu_duration_ms']=1
            if case=='wrong-sync':rows[0]['sync']='implicit'
            if case=='window':window=[200_000_000,5_000_000_000]
            if case=='clock':rows[3]['observed_monotonic_ns']=0
            if case=='wrong-device':rows[0].pop('device_id')
            if case=='partial':rows[-1]['complete']=False
            with self.subTest(case=case),self.assertRaises(BenchError):producer_metrics(rows,'explicit',window)

    def test_pairing_reports_absolute_deltas_without_claiming_no_contention(self):
        rows,inj=input_rows();raw,motion=motion_rows()
        control={'input':input_metrics(rows,inj),'motion':motion_metrics(raw,motion,{'mode':{'refresh_mhz':60000}})}
        loaded=copy.deepcopy(control)
        loaded['input']['injection_to_presentation_upper_ms']['p50']+=40
        paired=paired_metrics(control,loaded)
        self.assertEqual(paired['input_p50_upper_delta_ms'],40)
        self.assertFalse(paired['causal_isolation_from_gpu_contention'])

    def test_block_statistics_do_not_pool_actions_or_claim_significance(self):
        blocks=[]
        for repetition,value in enumerate((1,2,90),1):
            blocks.append({'phase':'responsiveness','target':'vela','repetition':repetition,'status':'complete',
                'audit':{'measurements':{'scenarios':{'neutral':{'input':{'injection_to_presentation_upper_ms':{'p50':value,'p95':value+1}}}}}}})
        group=aggregate(blocks)[0]
        self.assertEqual(group['independent_blocks'],3)
        self.assertEqual(group['block_statistics']['input/p50'],{'median':2,'min':1,'max':90,'unit':'ms'})
        self.assertIn('no pooled quantiles',group['aggregation'])
        blocks[2]['status']='failed'
        self.assertEqual(aggregate(blocks)[0]['independent_blocks'],2)

    def test_raw_audit_rejects_changed_summary_and_family_completion_claim(self):
        rows,inj=input_rows();metrics=input_metrics(rows,inj)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);prefix=root/'vela-measure-input'
            prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            atomic_json(prefix.with_suffix('.injections.json'),inj)
            atomic_json(prefix.with_suffix('.json'),{'status':'passed','uinput_cleanup':'passed'})
            summary={'schema':1,'method':VERSION,'phase':'responsiveness','spec':SPEC,'status':'passed',
                     'scenarios':{'neutral':{'input':metrics}},'failures':[],'family_complete':False}
            atomic_json(root/'vela-measurements.json',summary)
            self.assertEqual(audit_measurements(root,'vela',{},'responsiveness'),summary)
            summary['family_complete']=True
            atomic_json(root/'vela-measurements.json',summary)
            with self.assertRaises(BenchError):audit_measurements(root,'vela',{},'responsiveness')

    def test_unsupported_phase_cannot_be_dispatched_by_privileged_executor(self):
        for phase in ('report','check'):
            with self.subTest(phase=phase),self.assertRaises(BenchError):
                execution_selection({'targets':{'vela':{}}},['vela'],case='calibrate',fast=True,seed=0,phases=[phase],exercise=False)



class MeasurementCampaign(unittest.TestCase):
    """Exercise the common executor with real raw P2 audits and mocked hardware."""
    def setUp(self):
        from test_campaign import CalibrationCampaign
        CalibrationCampaign.setUp(self)
        self.fail_plasma=False
        self.options=[]
        patch('vela_bench.campaign.audit_pilot',return_value={'status':'passed','checks':{'restored':True}}).start()

    def fake_begin(self,host,targets,**options):
        from test_campaign import CalibrationCampaign
        from vela_bench.archive import read_json
        from vela_bench.campaign import block_request
        self.options.append(options)
        CalibrationCampaign.fake_begin(self,host,targets,**options)
        source=self.layout.base/'pilots'/options['run_id']
        target=targets[0]
        request=block_request(self.layout,options['campaign_run_id'],options['campaign_block_id'],target)
        state=read_json(source/'state.json')
        state['campaign_block']=request
        atomic_json(source/'state.json',state)
        phase=request['block']['phase']
        sample={}
        if phase=='responsiveness':
            rows,inj=input_rows();prefix=source/(target+'-measure-input')
            prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            atomic_json(prefix.with_suffix('.injections.json'),inj)
            atomic_json(prefix.with_suffix('.json'),{'status':'passed','uinput_cleanup':'passed'})
            sample['input']=input_metrics(rows,inj)
        elif phase=='motion':
            raw,rows=motion_rows();prefix=source/(target+'-measure-neutral-motion')
            prefix.with_suffix('.jsonl').write_text(raw)
            sample['motion']=motion_metrics(raw,rows,self.snapshot['profile'])
            atomic_json(prefix.with_suffix('.json'),{'status':'passed','metrics':sample['motion']})
        summary={'schema':1,'method':VERSION,'phase':phase,'spec':SPEC,'status':'passed',
                 'scenarios':{'neutral':sample},'failures':[],'family_complete':False}
        atomic_json(source/(target+'-measurements.json'),summary)
        atomic_json(source/'block-workload.json',dict(request,status='passed'))
        return 0

    def run_campaign(self,**options):
        import contextlib,io
        from types import SimpleNamespace
        from vela_bench.campaign import execute
        with contextlib.redirect_stdout(io.StringIO()):
            return execute(SimpleNamespace(),assume_yes=True,layout=self.layout,
                **dict({'phases':['responsiveness','motion'],'fast':True},**options))

    def test_two_phases_export_real_metrics_with_partial_family_coverage(self):
        from vela_bench.campaign import latest,export_campaign,read_export
        self.assertEqual(self.run_campaign(),0)
        data=latest(self.layout)
        self.assertEqual(data['execution_mode'],'measure')
        self.assertEqual(data['measurement_blocks_executed'],4)
        self.assertEqual(data['families']['motion'],'partial-client-only')
        self.assertFalse(data['full_suite_complete'])
        self.assertEqual(len(data['measurements']),4)
        self.assertTrue(all(opt['calibrate'] for opt in self.options))
        output=export_campaign(self.layout.path('exports'),data,self.layout)
        self.assertEqual(read_export(output),data)
        text=(output/'report.html').read_text()
        self.assertIn('Native benchmark measurements',text)
        self.assertIn('No p99',text)
        self.assertNotIn('Zero measurement blocks executed',text)

    def test_fast_and_normal_use_identical_scenario_specifications(self):
        from vela_bench.campaign import latest
        self.run_campaign(fast=True,phases=['motion'])
        fast=latest(self.layout)
        self.assertEqual(self.run_campaign(fast=False,phases=['motion']),0)
        normal=latest(self.layout)
        self.assertEqual(len(normal['blocks']),6)
        self.assertEqual(len(fast['blocks']),2)
        for block in normal['blocks']:
            self.assertEqual(block['measurement_spec'],SPEC)
            self.assertEqual(block['audit']['measurements']['scenarios']['neutral']['motion']['submitted'],600)

    def test_missing_gpu_instrument_is_rejected_before_any_session(self):
        with self.assertRaisesRegex(BenchError,'requires the installed'):
            self.run_campaign(phases=['slow-apps'])
        self.assertEqual(self.calls,[])



class SlowWorkload(unittest.TestCase):
    def test_production_pairing_and_raw_audit_for_both_sync_modes(self):
        from types import SimpleNamespace
        import io
        from vela_bench.measurements import run_workload
        from vela_bench.host import Layout
        profile={'mode':{'refresh_mhz':60000}}
        calls=[]
        def offset(rows):
            for row in rows:
                for key in ('monotonic_ns','commit_clock_ns','presentation_ns','received_monotonic_ns','input_received_monotonic_ns'):
                    if key in row:row[key]+=1_000_000_000
                for key in ('delivered_ms','input_delivered_ms'):
                    if key in row:row[key]+=1000
            return rows
        def inject(host,record,directory,target,layout,health,*,measurement_name):
            calls.append(measurement_name)
            rows,inj=input_rows();rows=offset(rows)
            for row in inj:
                row['before_monotonic_ns']+=1_000_000_000
                row['after_monotonic_ns']+=1_000_000_000
            prefix=directory/(target+'-'+measurement_name)
            prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            atomic_json(prefix.with_suffix('.injections.json'),inj)
            atomic_json(prefix.with_suffix('.json'),{'status':'passed','uinput_cleanup':'passed'})
        def animate(host,record,directory,target,layout,profile,label):
            calls.append('motion-'+label)
            _,rows=motion_rows();rows=offset(rows);raw=''.join(json.dumps(r)+'\n' for r in rows)
            metrics=motion_metrics(raw,rows,profile)
            prefix=directory/(target+'-measure-'+label+'-motion')
            prefix.with_suffix('.jsonl').write_text(raw)
            outcome={'status':'passed','metrics':metrics}
            atomic_json(prefix.with_suffix('.json'),outcome)
            return outcome
        class FakeLines:
            def __init__(self,process):
                self.records=[];self.rows=iter(gpu_rows(process.mode,100))
            def next(self,deadline):
                row=next(self.rows);self.records.append(row);return row
        class Host:
            def run(self,*args,**kwargs):return SimpleNamespace(returncode=0)
            def spawn(self,command,errors):
                return SimpleNamespace(mode='implicit' if '--implicit' in command else 'explicit',
                    stdin=io.BytesIO(),stdout=None,stderr=None,poll=lambda:0,wait=lambda timeout:0)
        with tempfile.TemporaryDirectory() as tmp, patch('vela_bench.measurements.run_input',side_effect=inject), \
                patch('vela_bench.measurements.run_motion',side_effect=animate), \
                patch('vela_bench.measurements.Lines',FakeLines):
            directory=Path(tmp)
            result=run_workload(Host(),{'uid':1001,'gid':1001},directory,'vela',Layout(),profile,'slow-apps',lambda:None)
            self.assertEqual(result['status'],'passed',result['failures'])
            self.assertEqual(calls,['measure-explicit-control-input','motion-explicit-control',
                'measure-explicit-input','motion-explicit','measure-implicit-control-input','motion-implicit-control',
                'measure-implicit-input','motion-implicit'])
            self.assertEqual(audit_measurements(directory,'vela',profile,'slow-apps'),result)


if __name__=='__main__':unittest.main()
