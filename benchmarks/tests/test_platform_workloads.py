# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import hashlib
import struct
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import zlib

from vela_bench import BenchError
from vela_bench.archive import atomic_json
from vela_bench.campaign import execution_selection
from vela_bench.pixels import png_rgb, pattern_metrics
from vela_bench.platform_workloads import SPEC, VERSION, scaling_metrics, audit_measurements
from vela_bench.resources import metrics, snapshot, process_stat
from vela_bench import resources


def png(width,height,pixels,channels=3,filter_mode=0):
    def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data)&0xffffffff)
    stride=width*channels
    rows=bytearray()
    for y in range(height):
        original=pixels[y*stride:(y+1)*stride]
        previous=pixels[(y-1)*stride:y*stride] if y else bytes(stride)
        filtered=bytearray(original)
        for i,value in enumerate(original):
            left=original[i-channels] if i>=channels else 0
            up=previous[i]
            corner=previous[i-channels] if i>=channels else 0
            guess=left+up-corner
            ds=[abs(guess-left),abs(guess-up),abs(guess-corner)]
            adjustment=(0,left,up,(left+up)//2,(left,up,corner)[ds.index(min(ds))])[filter_mode]
            filtered[i]=(value-adjustment)&255
        rows.extend(bytes([filter_mode])+filtered)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',width,height,8,2 if channels==3 else 6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def pattern(width=64,height=48):
    return bytes(v for y in range(height) for x in range(width) for v in (x&255,y&255,((x>>8)&15)*16+((y>>8)&15)))


def sample(stamp=1_000_000_000,cpu=100_000,pid=22,start=42):
    return {'event':'resource-sample','uid':1001,'begin_monotonic_ns':stamp,'end_monotonic_ns':stamp+100,
        'cpu_begin_monotonic_ns':stamp+40,'cpu_end_monotonic_ns':stamp+60,'cpu_usage_usec':cpu,
        'collector_cpu_seconds':stamp/1e11,'clock_ticks_per_second':100,'vanished_during_inventory':[],
        'processes':[{'pid':pid,'start_ticks':start,'cpu_ticks':100,'pss_kib':1024,
        'name':'fixture','cgroup':'0::/user.slice/user-1001.slice/user@1001.service/app.slice/app.service','classification':'session'}]}


class Pixels(unittest.TestCase):
    def test_all_png_filters_and_rgba_decode_without_resampling(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'image.png'
            for channels in (3,4):
                rgb=pattern()
                data=rgb if channels==3 else bytes(v for i in range(64*48) for v in (*rgb[i*3:i*3+3],255))
                for mode in range(5):
                    path.write_bytes(png(64,48,data,channels,mode))
                    self.assertEqual(png_rgb(path),(64,48,bytearray(rgb)))

    def test_pattern_verifies_full_expected_rectangle_not_surviving_good_crop(self):
        meta={'event':'pattern','method':'coordinate-pattern-v1','protocol':'wayland','viewport':True,
              'logical_width':64,'logical_height':48,'buffer_width':64,'buffer_height':48,'scale120':120,'sequence':1}
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'image.png';pixels=bytearray(pattern())
            path.write_bytes(png(64,48,pixels))
            self.assertTrue(pattern_metrics(path,meta,[64,48],1.)['pixel_equality'])
            pixels[(20*64+30)*3]^=1
            path.write_bytes(png(64,48,pixels))
            result=pattern_metrics(path,meta,[64,48],1.)
            self.assertEqual(result['different_pixels'],1)
            self.assertFalse(result['pixel_equality'])
            with self.assertRaises(BenchError):pattern_metrics(path,dict(meta,buffer_width=65),[64,48],1.)
            with self.assertRaises(BenchError):pattern_metrics(path,meta,[32,24],1.)
            with self.assertRaises(BenchError):pattern_metrics(path,meta,[64,48],1.25)
            damaged=bytearray(path.read_bytes());damaged[-1]^=1;path.write_bytes(damaged)
            with self.assertRaises(BenchError):png_rgb(path)


class ScalingState(unittest.TestCase):
    def test_plasma_toggle_receipts_cannot_accept_top_tiling_or_vela_shortcuts(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            profile={'outputs':[],'mode':{'width':1920,'height':1080}}
            def fixture(state,commands):
                name=f'plasma-scale-100-{state}.png';(directory/name).write_bytes(b'fixture')
                return {'scale':1.,'state':state,'image_file':name,'image_sha256':hashlib.sha256(b'fixture').hexdigest(),
                    'outputs':[],'capture':{'event':'capture','index':0,'backend':'kwin-screenshot2-v1',
                        'width':1920,'height':1080,'begin_monotonic_ns':30,'end_monotonic_ns':31},
                    'pattern':{'monotonic_ns':20,'logical_width':1920,'logical_height':1000,
                        'buffer_width':1920,'buffer_height':1000,'maximized':True},
                    'injections':[{'event':'injected','command':c,'before_monotonic_ns':10+i,
                                   'after_monotonic_ns':11+i} for i,c in enumerate(commands)]}
            with patch('vela_bench.platform_workloads.pattern_metrics',return_value={'fixture':True}):
                row=fixture('maximized',['maximize-toggle'])
                self.assertEqual(scaling_metrics(row,directory,profile,'plasma'),{'fixture':True})
                row['pattern'].update(maximized=False,logical_height=517,buffer_height=517)
                with self.assertRaisesRegex(BenchError,'acknowledge maximization'):
                    scaling_metrics(row,directory,profile,'plasma')
                row=fixture('maximized',['maximize'])
                with self.assertRaisesRegex(BenchError,'actions were not verified'):
                    scaling_metrics(row,directory,profile,'plasma')
                row=fixture('restored',['maximize-toggle','maximize-toggle'])
                row['pattern'].update(maximized=False,logical_width=401,logical_height=301,buffer_width=401,buffer_height=301)
                self.assertEqual(scaling_metrics(row,directory,profile,'plasma'),{'fixture':True})
                row['injections'][1]['command']='restore'
                with self.assertRaisesRegex(BenchError,'actions were not verified'):
                    scaling_metrics(row,directory,profile,'plasma')


class ResourceAccounting(unittest.TestCase):
    def test_cpu_comes_from_whole_cgroup_even_when_pid_exits(self):
        rows=[sample(),sample(3_000_000_000,600_000,pid=33)]
        data=metrics(rows,1001)
        self.assertEqual(data['cpu_seconds'],.5)
        self.assertEqual(data['cpu_percent_one_logical_cpu'],25)
        self.assertEqual(data['pss_mib_median'],1)
        self.assertEqual(data['inventory_process_identities'],2)
        self.assertFalse(data['capture_enabled'])
        self.assertEqual(data['energy'],'unsupported')

    def test_rejects_counter_reset_reused_pid_duplicate_and_sampling_gap(self):
        for kind in ('reset','uid','duplicate','boundary','gap','pid-counter'):
            rows=[sample(),sample(3_000_000_000,600_000)]
            if kind=='reset':rows[-1]['cpu_usage_usec']=0
            if kind=='uid':rows[-1]['uid']=0
            if kind=='duplicate':rows[-1]['processes']*=2
            if kind=='boundary':rows[-1]['processes'][0]['cgroup']='0::/other'
            if kind=='pid-counter':rows[-1]['processes'][0]['cpu_ticks']=0
            if kind=='gap':rows[-1]['begin_monotonic_ns']=10_000_000_000;rows[-1]['end_monotonic_ns']=10_000_000_100
            with self.subTest(kind=kind),self.assertRaises(BenchError):metrics(rows,1001)

    def test_real_proc_parser_handles_spaces_and_closing_parentheses(self):
        fields=['S']+['0']*19
        fields[11],fields[12],fields[19]='4','6','123'
        self.assertEqual(process_stat('22 (some ) name) '+' '.join(fields)),(10,123))

    def test_snapshot_includes_services_outside_workload_tree_and_refuses_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);proc=root/'proc';proc.mkdir();cg=root/'cg/user.slice/user-1001.slice';cg.mkdir(parents=True)
            (cg/'cpu.stat').write_text('usage_usec 500000\n')
            for pid,uid in ((22,1001),(33,1001),(44,1000)):
                path=proc/str(pid);path.mkdir()
                (path/'status').write_text(f'Name:\tdaemon\nUid:\t{uid}\t{uid}\t{uid}\t{uid}\n')
                fields=['S']+['0']*19;fields[19]=str(pid)
                (path/'stat').write_text(f'{pid} (daemon) '+' '.join(fields))
                (path/'smaps_rollup').write_text('Pss: 1024 kB\n')
                (path/'cgroup').write_text(f'0::/user.slice/user-{uid}.slice/user@{uid}.service/app.slice/daemon.service')
            data=snapshot(1001,proc=proc,cgroup=root/'cg')
            self.assertEqual({p['pid'] for p in data['processes']},{22,33})
            (proc/'33/cgroup').write_text('0::/system.slice/escaped.service')
            with self.assertRaises(BenchError):snapshot(1001,proc=proc,cgroup=root/'cg')


class P4Planning(unittest.TestCase):
    def test_phases_have_fixed_specs_and_fast_only_changes_repetitions(self):
        plans=[execution_selection({'targets':{'vela':{},'plasma':{}}},['vela','plasma'],case='calibrate',
            fast=fast,seed=0,phases=['scaling','overhead'],exercise=False) for fast in (True,False)]
        self.assertEqual([len(p['blocks']) for p in plans],[4,12])
        for plan in plans:
            self.assertTrue(all(b['measurement_spec']==SPEC and b['workload']==VERSION for b in plan['blocks']))
            self.assertTrue(all('capture' not in b['requirements'] for b in plan['blocks'] if b['phase']=='overhead'))

    def test_mixed_p2_p4_schedule_keeps_distinct_immutable_workloads(self):
        plan=execution_selection({'targets':{'vela':{}}},['vela'],case='calibrate',fast=True,seed=0,
            phases=['motion','overhead'],exercise=False)
        self.assertEqual([b['workload'] for b in plan['blocks']],['p2-neutral-client-v1',VERSION])


class ResourceCampaign(unittest.TestCase):
    def setUp(self):
        import test_measurements
        test_measurements.MeasurementCampaign.setUp(self)
        self.setup['files'][str(self.layout.probe)]='fixture-probe-hash'

    def fake_begin(self,host,targets,**options):
        import json
        from test_campaign import CalibrationCampaign
        from test_diagnostics import stream
        from vela_bench.campaign import block_request
        from vela_bench.archive import read_json
        from vela_bench.platform_workloads import resource_metrics
        self.options.append(options)
        CalibrationCampaign.fake_begin(self,host,targets,**options)
        source=self.layout.base/'pilots'/options['run_id'];target=targets[0]
        request=block_request(self.layout,options['campaign_run_id'],options['campaign_block_id'],target)
        state=read_json(source/'state.json');state['campaign_block']=request
        atomic_json(source/'state.json',state)
        scenarios={}
        for index,label in enumerate(('idle','active-animation','local-hash')):
            offset=index*50_000_000_000
            rows=[sample((i+11)*1_000_000_000+offset,100_000+i*10_000) for i in range(34)]
            raw=''
            if label=='active-animation':
                work=stream(frames=1800)
                for row in work:
                    for key in ('monotonic_ns','received_monotonic_ns','presentation_ns','commit_clock_ns'):
                        if key in row:row[key]+=12_000_000_000+offset
                raw=''.join(json.dumps(r)+'\n' for r in work)
            if label=='local-hash':raw=json.dumps({'event':'local-work-complete','method':'fixed-sha256-corpus-v1',
                'chunks':resources.HASH_CHUNKS,'bytes':len(resources.HASH_DATA)*resources.HASH_CHUNKS,
                'digest':hashlib.sha256(resources.HASH_DATA).hexdigest(),
                'begin_monotonic_ns':12_000_000_000+offset,'end_monotonic_ns':16_000_000_000+offset})
            atomic_json(source/f'{target}-resources-{label}.json',{'uid':1001,'samples':rows})
            if raw:(source/f'{target}-resources-{label}.work.jsonl').write_text(raw)
            scenarios[label]={'resources':resource_metrics(rows,1001,label,raw)}
        atomic_json(source/f'{target}-session.json',{'session':{'User':'1001'}})
        atomic_json(source/f'{target}-measurements.json',{'schema':1,'method':VERSION,'phase':'overhead','spec':SPEC,
            'settling_begin_monotonic_ns':0,'settling_end_monotonic_ns':10_000_000_000,
            'status':'passed','scenarios':scenarios,'failures':[],'family_complete':False})
        atomic_json(source/'block-workload.json',dict(request,status='passed'))
        return 0

    def test_executor_disables_input_and_capture_then_exports_and_reaudits_resources(self):
        import test_measurements
        from vela_bench.campaign import latest,export_campaign,read_export
        result=test_measurements.MeasurementCampaign.run_campaign(self,phases=['overhead'])
        self.assertEqual(result,0)
        self.assertTrue(all(opt['calibrate'] is False and opt['presentation_probe'] is False for opt in self.options))
        data=latest(self.layout)
        self.assertEqual(data['families']['overhead'],'partial-session-resources')
        output=export_campaign(self.layout.path('exports'),data,self.layout)
        self.assertEqual(read_export(output),data)
        self.assertFalse(data['full_suite_complete'])
        first=next(output.glob('overhead-*/vela-resources-idle.json'))
        first.write_text('{}')
        with self.assertRaises(BenchError):read_export(output)

    def test_raw_resource_audit_rejects_foreign_uid_overlap_and_counter_reset(self):
        import test_measurements
        from vela_bench.campaign import latest
        from vela_bench.archive import read_json
        self.assertEqual(test_measurements.MeasurementCampaign.run_campaign(self,phases=['overhead']),0)
        data=latest(self.layout);block=data['blocks'][0]
        source=self.layout.base/'pilots'/block['pilot_run_id'];target=block['target']
        path=source/f'{target}-resources-active-animation.json'
        original=read_json(path)
        for case in ('uid','overlap','reset'):
            broken=copy.deepcopy(original)
            if case=='uid':broken['uid']=1002
            if case=='overlap':broken['samples'][0]['begin_monotonic_ns']=1
            if case=='reset':broken['samples'][-1]['cpu_usage_usec']=0
            atomic_json(path,broken)
            with self.subTest(case=case),self.assertRaises(BenchError):audit_measurements(source,target,block['profile'],'overhead')
        atomic_json(path,original)

    def test_missing_native_instruments_fail_before_starting_any_worker(self):
        import test_measurements
        self.setup['files'].clear()
        with self.assertRaisesRegex(BenchError,'probe before takeover'):
            test_measurements.MeasurementCampaign.run_campaign(self,phases=['overhead'])
        with self.assertRaisesRegex(BenchError,'vela-pattern'):
            test_measurements.MeasurementCampaign.run_campaign(self,phases=['scaling'])
        self.assertEqual(self.options,[])


class ResourceLifecycle(unittest.TestCase):
    def test_resource_acceptance_checks_capture_off_without_claiming_instrument_qualification(self):
        from vela_bench.qualification import audit_pilot
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            state={'stage':'done','result':'passed','cleanup_errors':[], 'conditions_snapshot':{},
                   'conditions_restore':{'status':'passed'},'campaign_block':{'measurement_executed':True,'block':{'phase':'overhead'}},
                   'plan':{'presentation_probe':False,'instrument_calibration':False}}
            atomic_json(root/'state.json',state)
            atomic_json(root/'vela-session.json',{'fixture_hidden_from_both_trees':True,'graphical_session_target':'active'})
            atomic_json(root/'vela-conditions.json',{'status':'passed'})
            self.assertEqual(audit_pilot(root,'vela',instruments=False)['status'],'passed')
            state['plan']['presentation_probe']=True;atomic_json(root/'state.json',state)
            self.assertEqual(audit_pilot(root,'vela',instruments=False)['status'],'failed')
