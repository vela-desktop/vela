# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.archive import atomic_json
from vela_bench.desktop import SCENE, SPEC, action_metrics, changed, validate_frames, audit_observation
from vela_bench.campaign import execution_selection
from vela_bench.cli import main


def frames(kind='launcher-open'):
    rows=[]
    for i in range(12):
        tiles=[0]*576
        markers=[{'marker':'vela-frame-id-v2','frame':SCENE,'x':32,'y':32}]
        if i>=2:
            if kind=='launcher-open': tiles[:20]=[0xffffff]*20
            if kind=='desktop-hide': markers=[]
            if kind=='drag': markers[0]['x']+=20*(i-1)
        if kind=='desktop-restore':
            if i<2: markers=[]
        rows.append({'event':'capture','index':i,'backend':'ext-image-copy-capture-v1','width':1280,'height':720,
                     'begin_monotonic_ns':(100+i*100)*1_000_000,'end_monotonic_ns':(110+i*100)*1_000_000,
                     'tiles':tiles,'markers':markers})
    return rows


def actions(kind='launcher-open'):
    commands={'launcher-open':['launcher'],'desktop-hide':['desktop'],'desktop-restore':['desktop'],
              'drag':['point 100 100','down',*['point 200 100']*9,'up']}[kind]
    return [{'event':'injected','command':c,'before_monotonic_ns':211_000_000+i*10_000_000,
             'after_monotonic_ns':212_000_000+i*10_000_000} for i,c in enumerate(commands)]


class DesktopObservation(unittest.TestCase):
    def test_response_is_only_a_capture_upper_bound_and_static_or_unsettled_ui_is_refused(self):
        rows=frames(); events=actions()
        result=action_metrics(rows,events,'launcher-open')
        self.assertEqual(result['first_observed_response_lower_ms'],0)
        self.assertEqual(result['first_observed_response_upper_ms'],99)
        self.assertFalse(result['fine_grained_motion'])
        for broken in ('unchanged','tiny','unsettled','no-baseline','wrong-action'):
            with self.subTest(broken=broken):
                data=copy.deepcopy(rows); injected=copy.deepcopy(events)
                if broken=='unchanged':
                    for row in data: row['tiles']=[0]*576
                elif broken=='tiny':
                    for row in data[2:]: row['tiles']=[0xffffff]*2+[0]*574
                elif broken=='unsettled': data[-1]['tiles']=[0]*576
                elif broken=='no-baseline': injected[0]['before_monotonic_ns']=1
                else: injected[0]['command']='pulse'
                with self.assertRaises(BenchError): action_metrics(data,injected,'launcher-open')

    def test_drag_requires_identified_visible_window_movement_and_fixed_input(self):
        rows=frames('drag'); events=actions('drag')
        result=action_metrics(rows,events,'drag')
        self.assertGreater(result['distance_pixels'],80)
        self.assertIsNone(result['missed_refreshes'])
        for broken in ('stationary','wrong-window','short-path','invalid-input'):
            with self.subTest(broken=broken):
                data=copy.deepcopy(rows); injected=copy.deepcopy(events)
                if broken=='stationary':
                    for row in data: row['markers'][0]['x']=32
                elif broken=='wrong-window': data[-1]['markers'][0]['frame']=123
                elif broken=='short-path': data[-1]['markers'][0]['x']=33
                else: injected[-1]['command']='down'
                with self.assertRaises(BenchError): action_metrics(data,injected,'drag')

    def test_failed_marker_analysis_keeps_all_captures_and_injections_in_the_report(self):
        import subprocess
        import sys
        from types import SimpleNamespace
        from vela_bench.desktop import run_observation
        from vela_bench.host import Layout
        from vela_bench.archive import read_json
        # Pipe/controller fixture: no uinput, GUI, PAM or host service calls.
        # Reproduce the real failure only after the twelve captures complete.
        script = '''import base64,json,struct,sys,time
kind=sys.argv[1]
def emit(row): print(json.dumps(row),flush=True)
if kind=='scene':
    emit({'event':'scene-ready','protocol':'wayland','width':640,'height':360})
    emit({'event':'move-request','accepted':True})
    time.sleep(30)
elif kind=='input':
    emit({'event':'uinput-ready','method':'linux-uinput-fixed-desktop-v1','clock':'CLOCK_MONOTONIC',
          'sysfs':'/sys/devices/virtual/input/input999999999'})
    for i,line in enumerate(sys.stdin,1):
        if line=='quit\\n': break
        stamp=time.monotonic_ns()
        emit({'event':'injected','action_id':i,'before_monotonic_ns':stamp,'after_monotonic_ns':stamp+1})
else:
    png=base64.b64encode(b'\\x89PNG\\r\\n\\x1a\\n'+b'\\0'*8+struct.pack('!II',1280,720)).decode()
    for i in range(12):
        stamp=time.monotonic_ns()
        emit({'event':'capture','index':i,'backend':'ext-image-copy-capture-v1','width':1280,'height':720,
              'begin_monotonic_ns':str(stamp),'end_monotonic_ns':str(stamp+1),'tiles':[0]*576,
              'markers':[{'marker':'vela-frame-id-v2','frame':0x715000 if i<2 else 0x715800,
                          'x':32+18*max(0,i-2),'y':32}],'png_base64':png})
        time.sleep(.03)
'''
        class Host:
            def spawn(self, command, errors):
                kind='input' if command[0].endswith('vela-bench-input') else 'capture' if '_capture' in command else 'scene'
                return subprocess.Popen([sys.executable,'-c',script,kind],stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE,stderr=errors,bufsize=0)
            def run(self, command, **kwargs):
                return SimpleNamespace(returncode=0,stdout='',stderr='')
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            result=run_observation(Host(),{'uid':1001,'gid':1001},directory,'vela',Layout(directory),
                                   {'primary':'HEADLESS-1','mode':{'width':1280,'height':720}},'motion',lambda:None)
            self.assertEqual(result['status'],'failed')
            self.assertEqual(result['uinput_cleanup'],'passed')
            self.assertIn('0x715000',result['failures'][0])
            self.assertEqual(len(result['actions']),1)
            evidence=result['actions'][0]
            self.assertEqual(len(evidence['captures']),12)
            self.assertEqual(len(evidence['injections']),12)
            self.assertIn('observed IDs',evidence['error'])
            self.assertNotIn('metrics',evidence)
            self.assertEqual(read_json(directory/'vela-desktop.json'),result)
            self.assertEqual(len(list(directory.glob('vela-desktop-window-drag-*.png'))),12)

    def test_show_desktop_and_restore_require_opposite_verified_states(self):
        for kind in ('desktop-hide','desktop-restore'):
            rows=frames(kind)
            action_metrics(rows,actions(kind),kind)
            rows[-1]['markers']=frames()[0]['markers'] if kind=='desktop-hide' else []
            with self.assertRaises(BenchError): action_metrics(rows,actions(kind),kind)

    def test_capture_route_geometry_hash_and_marker_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp); rows=frames()
            for i,row in enumerate(rows):
                import hashlib
                name=f'vela-desktop-control-{i:02d}.png'; image=b'fixture png '+bytes([i])
                (directory/name).write_bytes(image)
                row.update(image_file=name,image_sha256=hashlib.sha256(image).hexdigest())
            profile={'mode':{'width':1280,'height':720}}
            validate_frames(rows,'vela',profile,directory,'control')
            for broken in ('route','geometry','hash','clock','marker'):
                data=copy.deepcopy(rows)
                if broken=='route': data[2]['backend']='portal-fallback'
                elif broken=='geometry': data[2]['width']=1920
                elif broken=='hash': data[2]['image_sha256']='wrong'
                elif broken=='clock': data[2]['begin_monotonic_ns']=0
                else: data[2]['markers'][0]['x']=-1
                with self.subTest(broken=broken), self.assertRaises(BenchError): validate_frames(data,'vela',profile,directory,'control')

    def test_observation_plan_preserves_blocks_and_budgets_between_modes(self):
        current={'targets':{'vela':{},'plasma':{}}}
        fast=execution_selection(current,['vela','plasma'],case='observe',fast=True,seed=0,phases=None,exercise=False)
        normal=execution_selection(current,['vela','plasma'],case='observe',fast=False,seed=0,phases=None,exercise=False)
        self.assertEqual((len(fast['blocks']),len(normal['blocks'])),(4,12))
        for block in fast['blocks']+normal['blocks']:
            self.assertEqual(block['measurement_spec'],SPEC)
            self.assertTrue(block['measurement_executed'])
        for phase in ('slow-apps','scaling','overhead','check'):
            with self.subTest(phase=phase),self.assertRaises(BenchError):
                execution_selection(current,['vela'],case='observe',fast=True,seed=0,phases=[phase],exercise=False)

    def test_observe_cli_dispatches_only_fixed_phases_without_exercise(self):
        with patch('vela_bench.cli.run_calibration_campaign',return_value=0) as run:
            self.assertEqual(main(['observe','--target','vela','--fast']),0)
        self.assertEqual(run.call_args.args[2],'observe')
        self.assertEqual(run.call_args.kwargs['phases'],['responsiveness','motion'])
        self.assertNotIn('exercise',run.call_args.kwargs)

    def test_complete_launcher_audit_requires_launch_receipt_content_and_return_to_baseline(self):
        import hashlib
        from vela_bench.desktop import LAUNCHED, VERSION
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            sample={'schema':1,'method':VERSION,'spec':SPEC,'phase':'responsiveness','status':'passed','failures':[],
                    'uinput_cleanup':'passed','family_complete':False,'launcher_launch_verified':True,
                    'launch_receipt':{'schema':1,'event':'launcher-app-started','uid':1001,'pid':42,'marker':LAUNCHED},'actions':[]}
            cases=[(f'{kind}-{i}','launcher-open' if kind=='launcher' else 'baseline') for i in range(6) for kind in ('launcher','close')]
            cases += [('launched','baseline'),('desktop-hide','desktop-hide'),('desktop-restore','desktop-restore')]
            for sequence,(name,kind) in enumerate(cases):
                rows=frames(kind)
                if kind=='baseline':
                    for row in rows:
                        if name=='launched': row['markers'][0]['frame']=LAUNCHED
                        elif row['index']<2: row['tiles'][:20]=[0xffffff]*20
                    injected=actions(); injected[0]['command']='pulse' if name=='launched' else 'escape'
                else: injected=actions(kind)
                for injection in injected:
                    for key in ('before_monotonic_ns','after_monotonic_ns'): injection[key]+=sequence*2_000_000_000
                for i,row in enumerate(rows):
                    for key in ('begin_monotonic_ns','end_monotonic_ns'): row[key]+=sequence*2_000_000_000
                    image=b'fixture png '+bytes([i]); filename=f'vela-desktop-{name}-{i:02d}.png'
                    (directory/filename).write_bytes(image)
                    row.update(image_file=filename,image_sha256=hashlib.sha256(image).hexdigest())
                sample['actions'].append({'id':name,'kind':kind,'captures':rows,'injections':injected,
                                          'metrics':action_metrics(rows,injected,kind)})
            sample['launch_actions']=[{'event':'injected','command':command,'before_monotonic_ns':stamp,
                                      'after_monotonic_ns':stamp+1_000_000} for command,stamp in
                                     zip(('launcher','search','enter','close'),(23_500_000_000,23_600_000_000,23_700_000_000,25_400_000_000))]
            profile={'mode':{'width':1280,'height':720}}
            path=directory/'vela-desktop.json'; atomic_json(path,sample)
            self.assertEqual(audit_observation(directory,'vela',profile,'responsiveness'),sample)
            for broken in ('receipt','search','visible-content','clock'):
                data=copy.deepcopy(sample)
                if broken=='receipt': data.pop('launch_receipt')
                elif broken=='search': data['launch_actions'][1]['command']='pulse'
                elif broken=='visible-content': data['actions'][12]['captures'][1]['markers'][0]['frame']=SCENE
                else: data['actions'][3]['captures'][0]['begin_monotonic_ns']=1
                atomic_json(path,data)
                with self.subTest(broken=broken),self.assertRaises(BenchError): audit_observation(directory,'vela',profile,'responsiveness')


class ObservationCampaign(unittest.TestCase):
    """Production schedule/checkpoint/export and raw audits; hardware is a fixture."""
    def setUp(self):
        from test_campaign import CalibrationCampaign
        CalibrationCampaign.setUp(self)
        self.fail_plasma=False
        scene=str(self.layout.helper.parent/'vela-bench-scene')
        self.setup['files'][scene]='fixture-hash'
        self.options=[]
        patch('vela_bench.campaign.audit_pilot',return_value={'status':'passed','checks':{'restored':True}}).start()

    def fake_begin(self, host, targets, **options):
        from test_campaign import CalibrationCampaign
        from vela_bench.archive import read_json
        from vela_bench.campaign import block_request
        if getattr(self,'interrupt_plasma',False) and targets==['plasma']: return 130
        CalibrationCampaign.fake_begin(self,host,targets,**options)
        self.options.append(options)
        directory=self.layout.base/'pilots'/options['run_id']; target=targets[0]
        request=block_request(self.layout,options['campaign_run_id'],options['campaign_block_id'],target)
        state=read_json(directory/'state.json'); state['campaign_block']=request
        atomic_json(directory/'state.json',state)
        rows=frames('drag')
        for i,row in enumerate(rows):
            import hashlib
            row.update(width=1920,height=1080,backend='ext-image-copy-capture-v1' if target=='vela' else 'kwin-screenshot2-v1')
            image=b'fixture png '+bytes([i]); name=f'{target}-desktop-window-drag-{i:02d}.png'
            (directory/name).write_bytes(image)
            row.update(image_file=name,image_sha256=hashlib.sha256(image).hexdigest())
        from vela_bench.desktop import VERSION
        result={'schema':1,'method':VERSION,'spec':SPEC,'phase':'motion','status':'passed','failures':[],
                'uinput_cleanup':'passed','family_complete':False,'actions':[{
                    'id':'window-drag','kind':'drag','captures':rows,'injections':actions('drag'),
                    'metrics':action_metrics(rows,actions('drag'),'drag')}]}
        atomic_json(directory/f'{target}-desktop.json',result)
        atomic_json(directory/'block-workload.json',dict(request,status='passed'))
        return 0

    def run_campaign(self,**options):
        import contextlib,io
        from types import SimpleNamespace
        from vela_bench.campaign import execute
        with contextlib.redirect_stdout(io.StringIO()):
            return execute(SimpleNamespace(),assume_yes=True,layout=self.layout,case='observe',
                           **dict({'phases':['motion'],'fast':True},**options))

    def test_export_reaudits_observed_positions_and_keeps_coverage_partial(self):
        from vela_bench.campaign import latest,export_campaign,read_export
        self.assertEqual(self.run_campaign(),0)
        data=latest(self.layout)
        self.assertEqual(data['execution_mode'],'observe')
        self.assertEqual(data['measurement_blocks_executed'],2)
        self.assertEqual(data['families']['motion'],'partial-desktop-observation')
        self.assertFalse(data['performance_comparison_available'])
        output=export_campaign(self.layout.path('exports'),data,self.layout)
        self.assertEqual(read_export(output),data)
        self.assertIn('Identified movement:',(output/'report.html').read_text())
        self.assertTrue(all(o['calibrate'] and o['controlled'] for o in self.options))
        # Rehash the edited derived file: a fresh hash cannot substitute for
        # recomputing geometry from the recorded captures.
        from vela_bench.archive import read_json
        from vela_bench.discovery import file_hash
        name='motion-vela-r1/vela-desktop.json'
        sample=read_json(output/name); sample['actions'][0]['metrics']['distance_pixels']=99999
        atomic_json(output/name,sample)
        manifest=read_json(output/'manifest.json'); manifest['files'][name]=file_hash(output/name)
        atomic_json(output/'manifest.json',manifest)
        with self.assertRaisesRegex(BenchError,'metrics differ'): read_export(output)

    def test_normal_schedule_has_six_independent_motion_blocks_and_same_spec(self):
        from vela_bench.campaign import latest
        self.assertEqual(self.run_campaign(fast=False),0)
        data=latest(self.layout)
        self.assertEqual(data['measurement_blocks_executed'],6)
        self.assertTrue(all(b['measurement_spec']==SPEC for b in data['blocks']))

    def test_missing_scene_is_refused_before_takeover(self):
        self.setup['files'].clear()
        with self.assertRaisesRegex(BenchError,'requires the installed'): self.run_campaign()
        self.assertEqual(self.calls,[])

    def test_resume_keeps_completed_observation_bytes_and_reruns_only_interrupted_block(self):
        from vela_bench.campaign import latest
        self.interrupt_plasma=True
        self.assertEqual(self.run_campaign(),130)
        first=latest(self.layout)
        marker=self.layout.base/'campaigns'/first['run_id']/'blocks/motion-vela-r1/complete.json'
        before=marker.read_bytes()
        self.interrupt_plasma=False
        self.assertEqual(self.run_campaign(resume=first['run_id']),0)
        self.assertEqual(marker.read_bytes(),before)
        self.assertEqual(latest(self.layout)['measurement_blocks_executed'],2)

if __name__=='__main__': unittest.main()
