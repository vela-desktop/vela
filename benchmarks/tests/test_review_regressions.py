# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Review regressions at CLI, trust, capture and lifecycle boundaries."""
import ast
import contextlib
import copy
import hashlib
import importlib.abc
import importlib.machinery
import importlib.util
import io
import json
import marshal
import os
from pathlib import Path
import signal
import struct
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench import BenchError, cli, host, measurements, pilot, resources
from vela_bench.archive import atomic_json, read_json
from vela_bench.desktop_pixels import evidence
from vela_bench.diagnostics import parse_probe, stop_instrument
from vela_bench.discovery import sessions
from vela_bench.pixels import image_hash, pattern_metrics, png_bytes, png_rgb


class ReviewCLI(unittest.TestCase):
    def test_non_object_manifests_have_a_diagnostic_and_exit_two(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            for manifest in ([], None, 'invalid'):
                atomic_json(directory/'manifest.json',manifest)
                for command in ('status','report'):
                    with self.subTest(command=command,manifest=manifest), contextlib.redirect_stderr(io.StringIO()) as errors:
                        self.assertEqual(cli.main([command,str(directory)]),2)
                        self.assertIn('object',errors.getvalue())
                        self.assertNotIn('Traceback',errors.getvalue())

    def test_live_observe_uses_the_same_discovered_target_as_dry_run(self):
        with patch.object(cli,'discover',return_value={'targets':{'vela':{},'gnome':{}}}), \
                patch.object(cli,'run_calibration_campaign',return_value=0) as run:
            self.assertEqual(cli.main(['observe','--yes']),0)
        self.assertEqual(run.call_args.args[1],['vela'])

    def test_live_json_redirects_worker_output_and_emits_an_archive_object(self):
        args=SimpleNamespace(resume=None,yes=True,fast=True,json=True,output=Path('/unused'))
        with patch.object(cli,'require_root'),patch.object(cli,'trusted'),patch.object(cli,'require_current_launcher'), \
                patch.object(cli.subprocess,'call',return_value=0) as worker, \
                patch.object(cli,'export_latest_campaign') as export:
            self.assertEqual(cli.run_calibration_campaign(args,['vela'],'calibrate'),0)
        self.assertIs(worker.call_args.kwargs['stdout'],cli.sys.stderr)
        self.assertTrue(export.call_args.kwargs['as_json'])
        with tempfile.TemporaryDirectory() as temporary,contextlib.redirect_stdout(io.StringIO()) as output, \
                patch.object(cli.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=json.dumps(
                    {'stage':'completed-with-failures','selection_complete':False}))), \
                patch.object(cli,'export_campaign',return_value=Path(temporary)),patch.dict(os.environ,{'SUDO_UID':'','SUDO_GID':''}):
            cli.export_latest_campaign(Path('/fixed-helper'),Path(temporary),as_json=True)
            result=json.loads(output.getvalue())
        self.assertEqual(result['archive'],temporary)
        self.assertFalse(result['selection_complete'])

    def test_eof_rejects_authorization_before_creating_a_new_pilot(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout=host.Layout(Path(temporary));transport=Mock()
            transport.pids.return_value=[]
            with patch.object(pilot,'load_setup',return_value={'uid':1001,'private_homes':[]}), \
                    patch.object(pilot,'resolve_targets',return_value={'vela':{}}), \
                    patch.object(pilot,'tty_preconditions',return_value={'origin_vt':2,'target_vt':4}), \
                    patch.object(pilot,'check_account',return_value=SimpleNamespace(pw_uid=1001)), \
                    patch.object(pilot,'autostart_inventory',return_value=[]), \
                    patch('builtins.input',side_effect=EOFError),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(BenchError,'not authorized'):
                    pilot.begin(transport,['vela'],layout=layout,countdown=False)
            self.assertFalse((layout.base/'active.json').exists())
            transport.run.assert_not_called()

    def test_measurement_defaults_skip_gnome_but_explicit_gnome_is_rejected(self):
        from vela_bench.model import load_campaign,make_plan
        discovery={'targets':{'vela':{},'gnome':{}},'limitations':[],'suite':{},'environment':{}}
        plan=make_plan(load_campaign(None),discovery,phases=['motion'],fast=True)
        self.assertEqual(list(plan['targets']),['vela'])
        with self.assertRaisesRegex(BenchError,'gnome'):
            make_plan(load_campaign(None),discovery,phases=['motion'],targets=['gnome'])

    def test_shell_entry_can_hash_a_readable_nonexecutable_script_argument(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);script=directory/'session script.sh'
            script.write_text('exec /bin/true\n');script.chmod(0o644)
            (directory/'vela.desktop').write_text('[Desktop Entry]\nExec=/bin/sh "'+str(script)+'"\n')
            found,rejected=sessions(directory)
            self.assertEqual(rejected,[])
            self.assertEqual(found['vela']['files'],[{'path':str(script),'sha256':hashlib.sha256(script.read_bytes()).hexdigest()}])
            self.assertEqual(len(found['vela']['executables']),1)


class ReviewEvidence(unittest.TestCase):
    def test_animation_deadline_covers_1800_frames_at_30_hz_without_shortening_work(self):
        from vela_bench.platform_workloads import resource_animation_timeout,SPEC
        self.assertEqual(SPEC['active_animation_frames'],1800)
        self.assertEqual(resource_animation_timeout({'mode':{'refresh_mhz':30000}}),80)
        self.assertEqual(resource_animation_timeout({'mode':{'refresh_mhz':60000}}),50)
        for refresh in (0,True,10000):
            with self.subTest(refresh=refresh),self.assertRaises(BenchError):
                resource_animation_timeout({'mode':{'refresh_mhz':refresh}})

    def test_p0_capture_markers_are_recomputed_from_the_hashed_original_png(self):
        from test_calibration import samples
        from test_desktop_v2 import picture
        from vela_bench.calibration import parse_input
        from vela_bench.qualification import calibration_evidence
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name,count,delay in (('input',20,0),('input-capture',20,0),('input-delay',5,40)):
                rows,injections=samples(count=count,delay=delay)
                prefix=root/('vela-'+name);metrics=parse_input(rows,injections,count=count,delay=delay)
                if name=='input-capture':
                    frames=[]
                    for i in range(12):
                        path=root/f'vela-capture-{i:02d}.png';path.write_bytes(picture([80,80,640,360],identity=i%3+1))
                        frames.append(dict(evidence(path),image_file=path.name,image_sha256=image_hash(path)))
                    metrics['capture']={'status':'passed','scope':'coarse-output-state-observation','frames':frames}
                atomic_json(prefix.with_suffix('.json'),{'status':'passed','uinput_cleanup':'passed',
                    'session_accounting':{'status':'passed'},'metrics':metrics})
                atomic_json(prefix.with_suffix('.injections.json'),injections)
                prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
            calibration_evidence(root,'vela')
            path=root/'vela-input-capture.json';saved=read_json(path)
            saved['metrics']['capture']['frames'][0]['markers'][0]['frame']=20
            atomic_json(path,saved)
            with self.assertRaisesRegex(BenchError,'original PNG'):calibration_evidence(root,'vela')

    def test_monitor_failure_persists_evidence_before_requesting_supervised_recovery(self):
        from vela_bench import input_isolation
        events=[]
        guard=input_isolation.InputIsolation(host.Layout(),on_failure=lambda row:events.append(row))
        def killed(pid,signum):
            self.assertEqual(pid,os.getpid());self.assertEqual(signum,signal.SIGUSR2)
            self.assertEqual(events[0]['status'],'failed')
            self.assertIn('device failed',events[0]['error'])
        with patch.object(input_isolation.select,'select',side_effect=OSError('device failed')), \
                patch.object(input_isolation.os,'kill',side_effect=killed) as kill:
            guard.monitor()
        kill.assert_called_once()

    def test_future_presentation_is_rejected_when_clocks_are_identical(self):
        from test_diagnostics import stream,encode
        rows=stream();rows[3]['presentation_ns']=rows[3]['received_monotonic_ns']+1
        with self.assertRaisesRegex(BenchError,'receipt'):
            parse_probe(encode(rows),frames=3)

    def test_trailing_discard_extends_new_motion_window_and_preserves_v2(self):
        from test_measurements import motion_rows
        _,rows=motion_rows();last=next(r for r in reversed(rows) if r['event']=='presented')
        last.update(event='discarded',received_monotonic_ns=last['received_monotonic_ns']+3_000_000_000)
        rows[-1].update(presented=629,discarded=1)
        raw=''.join(json.dumps(r)+'\n' for r in rows);profile={'mode':{'refresh_mhz':60000}}
        current=measurements.motion_metrics(raw,rows,profile)
        historical=measurements.motion_metrics(raw,rows,profile,method=measurements.SPEC_V2['version'])
        self.assertEqual(current['window_monotonic_ns'][1],last['received_monotonic_ns'])
        self.assertLess(historical['window_monotonic_ns'][1],current['window_monotonic_ns'][1])

    def test_producer_receipt_cannot_precede_its_commit_or_use_another_uid(self):
        from test_measurements import gpu_rows
        rows=gpu_rows();window=[400_000_000,1_400_000_000]
        measurements.producer_metrics(rows,'explicit',window,uid=1001)
        with self.assertRaisesRegex(BenchError,'UID'):
            measurements.producer_metrics(rows,'explicit',window,uid=1002)
        rows[-2]['received_monotonic_ns']=rows[-4]['monotonic_ns']-1
        with self.assertRaisesRegex(BenchError,'visibility'):
            measurements.producer_metrics(rows,'explicit',window)

    def test_paired_metrics_reject_each_reversed_or_overlapping_window(self):
        from test_measurements import input_rows,motion_rows
        rows,injections=input_rows();raw,motion=motion_rows()
        control={'input':measurements.input_metrics(rows,injections),
                 'motion':measurements.motion_metrics(raw,motion,{'mode':{'refresh_mhz':60000}})}
        loaded=copy.deepcopy(control)
        for i,(sample,scope) in enumerate(((control,'input'),(control,'motion'),(loaded,'input'),(loaded,'motion'))):
            sample[scope]['window_monotonic_ns']=[i*10+1,i*10+5]
        measurements.paired_metrics(control,loaded)
        for sample,scope in ((control,'input'),(control,'motion'),(loaded,'input'),(loaded,'motion')):
            original=sample[scope]['window_monotonic_ns']
            for changed in ([9,8],[1,40]):
                sample[scope]['window_monotonic_ns']=changed
                with self.assertRaisesRegex(BenchError,'windows'):
                    measurements.paired_metrics(control,loaded)
            sample[scope]['window_monotonic_ns']=original

    def test_rgb_marker_scan_survives_an_unaligned_preceding_candidate(self):
        from test_desktop_v2 import picture
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'capture.png';path.write_bytes(picture([80,80,640,360]))
            width,height,rgb=png_rgb(path);at=(96*width+95)*3;rgb[at:at+3]=bytes((0,208,32))
            path.write_bytes(png_bytes(width,height,rgb))
            self.assertEqual(len(evidence(path)['markers']),1)

    def test_compact_marker_keeps_all_32_identity_bits_in_a_half_width_scene(self):
        from vela_bench.desktop import SCENE
        width,height=512,360;rgb=bytearray(bytes((32,40,48))*width*height)
        def fill(x,y,w,h,color):
            for line in range(y,y+h):rgb[(line*width+x)*3:(line*width+x+w)*3]=bytes(color)*w
        for x,color in ((16,(208,32,208)),(32,(32,208,32)),(304,(32,64,224)),(320,(224,128,32))):fill(x,16,16,32,color)
        for bit in range(32):fill(48+bit*8,16,8,32,(255,255,255) if SCENE>>bit&1 else (32,40,48))
        fill(width-24,height-24,16,16,(200,80,120))
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'capture.png';path.write_bytes(png_bytes(width,height,rgb));result=evidence(path)
        self.assertEqual(result['rectangles'],{str(SCENE):[0,0,512,360]})

    def test_pattern_cannot_pass_when_exclusions_remove_all_pixels(self):
        from test_platform_workloads import pattern
        meta={'event':'pattern','method':'coordinate-pattern-v1','protocol':'wayland','viewport':True,
              'logical_width':16,'logical_height':16,'buffer_width':16,'buffer_height':16,'scale120':120,'sequence':1}
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'image.png';path.write_bytes(png_bytes(16,16,pattern(16,16)))
            with self.assertRaisesRegex(BenchError,'pixels'):
                pattern_metrics(path,meta,[16,16],1.)

    def test_png_hash_and_decode_bound_the_read_before_allocation(self):
        stream=Mock();stream.read.return_value=b'x'*(64*1024*1024+1)
        with patch('pathlib.Path.open') as opened:
            opened.return_value.__enter__.return_value=stream
            for operation in (image_hash,png_rgb):
                with self.assertRaisesRegex(BenchError,'size limit'):operation(Path('/fixture.png'))
        self.assertTrue(all(call.args==(64*1024*1024+1,) for call in stream.read.call_args_list))

    def test_resource_classification_must_match_the_cgroup(self):
        from test_platform_workloads import sample
        rows=[sample(),sample(2_000_000_000,200_000)]
        resources.metrics(rows,1001)
        rows[0]['processes'][0]['classification']='benchmark-workload'
        with self.assertRaisesRegex(BenchError,'classification'):resources.metrics(rows,1001)

    def test_instrument_stop_requires_success_and_inactive_readback(self):
        for result,active in ((1,'inactive'),(0,'active'),(0,'activating')):
            transport=Mock()
            def run(command,**kwargs):
                if 'stop' in command:
                    if result:raise BenchError('stop failed')
                    return SimpleNamespace(returncode=0,stdout='')
                return SimpleNamespace(returncode=0,stdout=active)
            transport.run.side_effect=run
            with self.subTest(result=result,active=active),self.assertRaises(BenchError):
                stop_instrument(transport,{'uid':1001,'gid':1001},host.Layout(),'calibration')


class ReviewTrust(unittest.TestCase):
    def test_installed_source_loader_ignores_a_writable_timestamp_valid_cache(self):
        launcher=Path(__file__).resolve().parents[1]/'launcher.py'
        tree=ast.parse(launcher.read_text())
        selected=[node for node in tree.body if isinstance(node,ast.ClassDef) and node.name in ('SourceOnly','InstalledModules')]
        scope={'importlib':importlib,'Path':Path}
        exec(compile(ast.Module(body=selected,type_ignores=[]),str(launcher),'exec'),scope)
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);package=root/'vela_bench';package.mkdir()
            source=package/'__init__.py';source.write_text('VALUE="source"\n')
            cache=Path(importlib.util.cache_from_source(str(source)));cache.parent.mkdir()
            header=importlib.util.MAGIC_NUMBER+struct.pack('<III',0,int(source.stat().st_mtime),source.stat().st_size)
            cache.write_bytes(header+marshal.dumps(compile('VALUE="cache"',str(source),'exec')));cache.chmod(0o666)
            control=SimpleNamespace()
            exec(importlib.machinery.SourceFileLoader('control',str(source)).get_code('control'),control.__dict__)
            self.assertEqual(control.VALUE,'cache')
            scope.update(directory=root,MODULES=host.MODULES)
            spec=scope['InstalledModules']().find_spec('vela_bench');module=importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            self.assertEqual(module.VALUE,'source')
            with self.assertRaises(ImportError):scope['InstalledModules']().find_spec('vela_bench.untracked')
        inventory=next(node.value for node in tree.body if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='MODULES' for t in node.targets))
        self.assertEqual(ast.literal_eval(inventory),host.MODULES)

    def test_bootstrap_lock_serializes_install_before_ownership_reads(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout=host.Layout(Path(temporary));first_entered=threading.Event();release_first=threading.Event();second_entered=threading.Event()
            def install_body(transport,*args):
                if transport=='first':
                    first_entered.set()
                    if not release_first.wait(5):raise RuntimeError('test release deadline')
                else:second_entered.set()
            with patch.object(host,'trusted'),patch.object(host,'_install',side_effect=install_body):
                first=threading.Thread(target=host.install,args=('first',layout));first.start()
                try:
                    self.assertTrue(first_entered.wait(3))
                    second=threading.Thread(target=host.install,args=('second',layout));second.start()
                    self.assertFalse(second_entered.wait(.1))
                finally:
                    release_first.set();first.join(3)
                    if 'second' in locals():second.join(3)
                self.assertTrue(second_entered.is_set())

    def test_edp_backlight_takes_precedence_over_a_non_vcp_ddc_link(self):
        from vela_bench.conditions import brightness_snapshot,set_brightness
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);drm=root/'class/drm/card0-eDP-1';drm.mkdir(parents=True)
            (drm/'status').write_text('connected\n');(drm/'edid').write_bytes(b'A'*128)
            bus=root/'devices/i2c-7';bus.mkdir(parents=True);(drm/'ddc').symlink_to(bus)
            light=root/'class/backlight/intel_backlight';light.mkdir(parents=True)
            (light/'brightness').write_text('25');(light/'max_brightness').write_text('100')
            transport=Mock();transport.run.side_effect=AssertionError('internal panel must not use DDC')
            snapshot=brightness_snapshot(transport,'eDP-1',root)
            self.assertEqual(snapshot['backend'],'sysfs-backlight')
            self.assertEqual(set_brightness(transport,snapshot,100,root)['value'],100)
            self.assertEqual(set_brightness(transport,snapshot,25,root)['value'],25)


class ReviewAuthentication(unittest.TestCase):
    def test_opening_sessions_are_refused_before_their_type_is_known(self):
        with self.assertRaisesRegex(BenchError,'session'):
            pilot.require_no_graphical_sessions([{'State':'opening','Type':'unspecified','Class':'user'}])

    def test_login_during_quiescence_never_reaches_kill_and_recovery_thaws(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout=host.Layout(Path(temporary));directory=layout.path('evidence');directory.mkdir()
            state={'stage':'takeover'};calls=[];frozen=False
            def run(command,**kwargs):
                nonlocal frozen
                calls.append(command);output=''
                if '--property=Id' in command:output='sddm.service'
                elif '--property=Restart' in command:output='no'
                elif '--property=FreezerState' in command:output='frozen' if frozen else 'running'
                elif '--property=ActiveState' in command:output='active'
                elif command[1]=='freeze':frozen=True
                elif command[1]=='thaw':frozen=False
                return SimpleNamespace(returncode=0,stdout=output)
            transport=SimpleNamespace(run=run)
            with patch.object(pilot,'trusted'),patch.object(pilot,'logind_sessions',return_value=[{'State':'opening','Class':'user'}]):
                with self.assertRaisesRegex(BenchError,'after authorization'):
                    pilot.quiesce_display_manager(transport,directory,state,layout)
                self.assertTrue(frozen)
                self.assertTrue((layout.path('run/systemd/system')/'sddm.service').is_symlink())
                self.assertFalse(any(c[1] in ('kill','stop') for c in calls))
                pilot.release_display_manager_quiescence(transport,directory,state,layout)
            self.assertFalse(frozen)
            self.assertFalse((layout.path('run/systemd/system')/'sddm.service').is_symlink())
            self.assertEqual(state['display_manager_quiescence']['status'],'released')
