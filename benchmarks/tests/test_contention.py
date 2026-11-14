# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import unittest

from vela_bench import BenchError,contention


def load_rows(kind='cpu',uid=1001):
    meta={'event':kind+'-load','schema':1,'method':contention.SPEC[kind]['method'],'uid':uid,'pid':40,
          'clock':'CLOCK_MONOTONIC','sample_period_ns':500_000_000}
    if kind=='cpu':meta.update(allowed_cpus=2,workers=[{'worker':i,'pid':41+i,'cpu':i} for i in range(2)],worker_cap=32,iterations_per_chunk=65536)
    else:meta.update(vendor_id=4098,device_id=1,drm_major=226,drm_minor=128,queue_family=0,graphics_queue=True,
                     workgroups=256,local_size=64,target_job_ms=5,timestamp_valid_bits=64,timestamp_period_ns=1)
    rows=[meta]
    if kind=='gpu':rows.append({'event':'calibrated','iterations':1000,'dispatches':1,'wall_duration_ms':5.})
    for index in range(1,61):
        stamp=index*500_000_000
        if kind=='cpu':
            for i in range(2):rows.append({'event':'cpu-load-sample','worker':i,'pid':41+i,'cpu':i,'sequence':index,
                'chunks':index*100,'checksum':'0123456789abcdef','cpu_time_ns':index*450_000_000,'monotonic_ns':stamp})
        else:rows.append({'event':'gpu-load-sample','sequence':index,'jobs':index*100,'gpu_time_ns':index*450_000_000,
                          'last_submit_ns':stamp-1_000_000,'monotonic_ns':stamp})
    summary={'event':kind+'-load-summary','complete':True,'monotonic_ns':30_100_000_000}
    if kind=='cpu':summary.update(workers_reaped=2,end_request_monotonic_ns=30_050_000_000)
    else:summary.update(submitted=6000,completed=6000)
    rows.append(summary)
    return rows


class ContentionMetrics(unittest.TestCase):
    def test_real_work_window_and_counters_are_separate_from_global_saturation(self):
        for kind in ('cpu','gpu'):
            metric=contention.metrics(load_rows(kind),kind,[1_100_000_000,12_200_000_000],1001)
            self.assertTrue(metric['window_covered']);self.assertFalse(metric['saturation_claimed'])
            if kind=='cpu':self.assertAlmostEqual(metric['mean_worker_cpu_fraction'],.9)
            else:
                self.assertAlmostEqual(metric['timestamp_span_fraction'],.9)
                self.assertIsNone(metric['global_gpu_utilization'])

    def test_missing_wrong_uid_clock_or_incomplete_load_cannot_pass(self):
        for kind in ('cpu','gpu'):
            for field,value in [('uid',1000),('clock','other'),('method','other')]:
                rows=load_rows(kind);rows[0][field]=value
                with self.subTest(kind=kind,field=field),self.assertRaises(BenchError):contention.metrics(rows,kind,[1_000_000_000,2_000_000_000],1001)
            rows=load_rows(kind);rows[-1]['complete']=False
            with self.assertRaises(BenchError):contention.metrics(rows,kind,[1_000_000_000,2_000_000_000],1001)
            with self.assertRaises(BenchError):contention.metrics(load_rows(kind),kind,[0,31_000_000_000],1001)

    def test_cpu_requires_all_pinned_workers_progress_cpu_time_and_cleanup(self):
        for failure in ('worker','affinity','lost','idle','cleanup','checksum','gap'):
            rows=load_rows()
            if failure=='worker':rows[2]['pid']=999
            if failure=='affinity':rows[0]['workers'][1]['cpu']=0
            if failure=='lost':rows.pop(3)
            if failure=='idle':
                for r in rows:
                    if r.get('event')=='cpu-load-sample':r['cpu_time_ns']=1
            if failure=='cleanup':rows[-1]['workers_reaped']=1
            if failure=='checksum':rows[1]['checksum']='not-work'
            if failure=='gap':rows[3]['monotonic_ns']=8_000_000_000
            with self.subTest(failure=failure),self.assertRaises(BenchError):contention.metrics(rows,'cpu',[1_100_000_000,12_200_000_000],1001)

    def test_gpu_requires_same_workload_graphics_queue_timestamps_and_continued_jobs(self):
        for failure in ('queue','geometry','time','idle','lost','finish','submit'):
            rows=load_rows('gpu')
            if failure=='queue':rows[0]['graphics_queue']=False
            if failure=='geometry':rows[0]['workgroups']=1
            if failure=='time':rows[0]['timestamp_period_ns']=float('nan')
            if failure=='idle':
                for r in rows:
                    if r.get('event')=='gpu-load-sample':r['gpu_time_ns']=1
            if failure=='lost':rows.pop(3)
            if failure=='finish':rows[-1]['completed']-=1
            if failure=='submit':rows[3]['last_submit_ns']=99_000_000_000
            with self.subTest(failure=failure),self.assertRaises(BenchError):contention.metrics(rows,'gpu',[1_100_000_000,12_200_000_000],1001)

    def test_readiness_requires_every_cpu_worker_or_completed_gpu_work(self):
        rows=load_rows();self.assertFalse(contention.ready(rows[:2],'cpu',1001));self.assertTrue(contention.ready(rows[:3],'cpu',1001))
        rows=load_rows('gpu');self.assertFalse(contention.ready(rows[:2],'gpu',1001));self.assertTrue(contention.ready(rows[:3],'gpu',1001))


class P2CampaignBinding(unittest.TestCase):
    """Mock hardware/raw auditor here; test the immutable live/export boundary."""
    def setUp(self):
        from test_campaign import CalibrationCampaign
        from unittest.mock import patch
        from vela_bench.archive import read_json
        CalibrationCampaign.setUp(self)
        self.context['uid']=1001;self.fail_plasma=False;self.bad_uid=False;self.downgrade=False
        for tool in ('vela-slowgpu','vela-bench-cpuload'):
            self.setup['files'][str(self.layout.helper.parent/tool)]='fixture'
        patch('vela_bench.campaign.audit_pilot',return_value={'status':'passed','checks':{'restored':True}}).start()
        patch('vela_bench.workloads.audit_measurements',side_effect=lambda directory,target,*args:read_json(directory/f'{target}-measurements.json')).start()

    def fake_begin(self,host,targets,**options):
        from test_campaign import CalibrationCampaign
        from vela_bench.archive import atomic_json,read_json
        from vela_bench.campaign import block_request
        from vela_bench.measurements import VERSION,SPEC,SPEC_V1
        CalibrationCampaign.fake_begin(self,host,targets,**options)
        source=self.layout.base/'pilots'/options['run_id'];target=targets[0]
        request=block_request(self.layout,options['campaign_run_id'],options['campaign_block_id'],target)
        state=read_json(source/'state.json');state['campaign_block']=request;atomic_json(source/'state.json',state)
        atomic_json(source/'block-workload.json',dict(request,status='passed'))
        atomic_json(source/f'{target}-measurements.json',{'schema':1,'method':SPEC_V1['version'] if self.downgrade else VERSION,
            'spec':SPEC_V1 if self.downgrade else SPEC,'phase':'slow-apps','status':'passed','family_complete':False,
            'scenarios':{},'contention_uid':1002 if self.bad_uid else 1001})
        return 0

    def run_campaign(self,**options):
        import contextlib,io
        from types import SimpleNamespace
        from vela_bench.campaign import execute
        with contextlib.redirect_stdout(io.StringIO()):
            return execute(SimpleNamespace(run=lambda *args,**kwargs:SimpleNamespace(stdout='--contention --timeout 1..240')),
                assume_yes=True,layout=self.layout,phases=['slow-apps'],fast=True,**options)

    def test_reserved_uid_and_method_binding_survive_export(self):
        from vela_bench.campaign import latest,export_campaign,read_export
        self.assertEqual(self.run_campaign(),0)
        data=latest(self.layout)
        self.assertTrue(all(b['audit']['checks']['contention_uid_binding'] for b in data['blocks']))
        output=export_campaign(self.layout.path('exports'),data,self.layout)
        self.assertEqual(read_export(output),data)

    def test_foreign_uid_is_rejected_before_acceptance(self):
        from vela_bench.campaign import latest
        self.bad_uid=True;self.assertEqual(self.run_campaign(),1)
        self.assertFalse(latest(self.layout)['blocks'][0]['audit']['checks']['contention_uid_binding'])

    def test_historical_smaller_workload_cannot_satisfy_new_block(self):
        from vela_bench.campaign import latest
        self.downgrade=True;self.assertEqual(self.run_campaign(),1)
        self.assertFalse(latest(self.layout)['blocks'][0]['audit']['checks']['measurement_method_binding'])

if __name__=='__main__':unittest.main()
