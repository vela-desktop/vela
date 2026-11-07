#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Production P4 scaling controller on a private headless Vela, fixture input only."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
import time
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'benchmarks'),str(ROOT/'tests/functional')]
from harness import Session,tool
from vela_bench.conditions import normalized_outputs,desired_profile
from vela_bench.host import Layout
from vela_bench.platform_workloads import run_workload,audit_measurements

KEYBOARD='''import json,subprocess,sys,time
device=subprocess.Popen([sys.argv[1],'-'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
print(json.dumps({'event':'uinput-ready','method':'linux-uinput-fixed-desktop-v1','clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'}),flush=True)
mapping={'snap-left':'super+Left','snap-right':'super+Right','maximize':'super+Up','restore':'super+Down'}
try:
    for index,line in enumerate(sys.stdin,1):
        began=time.monotonic_ns()
        command='move\\t1919\\t1079' if line.strip()=='point 65535 65535' else 'key\\t'+mapping[line.strip()]
        device.stdin.write(command+'\\n');device.stdin.flush()
        if device.stdout.readline().strip()!='OK':raise RuntimeError('fixture input failed')
        print(json.dumps({'event':'injected','action_id':index,'before_monotonic_ns':began,'after_monotonic_ns':time.monotonic_ns()}),flush=True)
finally:
    device.stdin.close();device.wait(timeout=3)
'''


class ScalingController(unittest.TestCase):
    def test_scaling_matrix_real_fractional_buffers_and_offline_pixel_audit(self):
        from types import SimpleNamespace
        with Session(size='1920x1080@60') as session,tempfile.TemporaryDirectory() as temporary:
            layout=Layout(Path(temporary));directory=Path(temporary)
            env=dict(session.client_env)
            if os.environ.get('VELA_TEST_CORPUS'):
                env['DISPLAY']=session.wait_for_log('Xwayland on DISPLAY=',lambda line:line.split('DISPLAY=')[1].split()[0])
            initial=normalized_outputs(subprocess.check_output(['wlr-randr','--json'],env=env,text=True),'vela')
            mode=initial[0]['modes'][0]
            profile=desired_profile(initial,initial[0]['name'],{k:mode[k] for k in ('width','height','refresh_mhz')})
            class Host:
                def spawn(self,command,errors):
                    if command[0].endswith('vela-bench-input'):
                        argv=[sys.executable,'-c',KEYBOARD,tool('vela-input')]
                    else:
                        at=next(i for i,v in enumerate(command) if Path(v).name in ('vela-pattern','vela-bench-corpus-qt','vela-bench-corpus-gtk','vela-bench-corpus-web'))
                        argv=[tool(Path(command[at]).name),*command[at+1:]]
                    return subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,env=env,bufsize=0)
                def run(self,command,**options):
                    if 'wlr-randr' in command or '/usr/bin/wlr-randr' in command:
                        at=next(i for i,v in enumerate(command) if v.endswith('wlr-randr'))
                        return subprocess.run(command[at:],env=env,capture_output=True,text=True,check=True,timeout=15)
                    if '_capture' in command:
                        path=str(directory/'transient.png')
                        began=time.monotonic_ns();session.screenshot(path);end=time.monotonic_ns()
                        capture=json.loads(subprocess.check_output([tool('vela-bench-desktop'),'--analyze',path],env=env,text=True))
                        capture.update(event='capture',backend='ext-image-copy-capture-v1',index=0,
                                       begin_monotonic_ns=str(began),end_monotonic_ns=str(end))
                        return SimpleNamespace(stdout=json.dumps(capture),returncode=0)
                    return SimpleNamespace(stdout='',returncode=0)
            from vela_bench import corpus
            inventory={case:{'tool':binary,'status':'unsupported','reason':'not-installed'} for case,binary in corpus.CASES.items()}
            if os.environ.get('VELA_TEST_CORPUS'):
                for case,binary in corpus.CASES.items():
                    inventory[case]={'tool':binary,'status':'available','binary_sha256':hashlib.sha256(Path(tool(binary)).read_bytes()).hexdigest(),
                        'description':json.loads(subprocess.check_output([tool(binary),'--describe'],env=env,text=True))}
            record={'uid':os.getuid(),'gid':os.getgid(),'corpus_inventory':inventory}
            if os.environ.get('VELA_TEST_CORPUS')=='1':
                from vela_bench.calibration import Lines,stop_process
                from vela_bench.platform_workloads import apply_outputs
                from vela_bench.archive import atomic_json
                outcome={'scenarios':{}}
                with (directory/'corpus-errors').open('wb') as errors:
                    keyboard=Host().spawn([str(layout.helper.parent/'vela-bench-input')],errors)
                    try:
                        keys=Lines(keyboard);keys.next(time.monotonic()+5);time.sleep(.5)
                        corpus.run(Host(),record,directory,'vela',layout,profile,lambda:None,
                            lambda label:print(label,flush=True),outcome,keyboard,keys,errors)
                        self.assertEqual(len(corpus.audit(outcome,directory,profile,'vela')),sum(v.get('cases',0) for v in outcome['corpus_availability'].values()))
                        self.assertTrue(all(outcome['corpus_availability'][case]['status']=='supported' for case in ('qt-wayland','gtk-wayland','qt-xwayland')))
                    finally:
                        stop_process(keyboard);apply_outputs(Host(),record,layout,'vela',profile['outputs'])
                        atomic_json(directory/'corpus-result.json',outcome)
                        if os.environ.get('VELA_P4_EVIDENCE'):shutil.copytree(directory,os.environ['VELA_P4_EVIDENCE'])
                print('CORPUS PASSED '+json.dumps(outcome['corpus_availability']),flush=True)
                return
            outcome=run_workload(Host(),record,directory,'vela',layout,profile,'scaling',lambda:None)
            self.assertEqual(outcome['status'],'passed',outcome['failures'])
            self.assertEqual(len(outcome['scenarios']),25+sum(v.get('cases',0) for v in outcome['corpus_availability'].values()))
            self.assertEqual(audit_measurements(directory,'vela',profile,'scaling'),outcome)
            actual=normalized_outputs(subprocess.check_output(['wlr-randr','--json'],env=env,text=True),'vela')
            self.assertEqual(next(o for o in actual if o['enabled'])['scale'],1.)
            if os.environ.get('VELA_P4_EVIDENCE'):
                output=Path(os.environ['VELA_P4_EVIDENCE'])
                if not output.is_absolute():output=ROOT/output
                if output.exists():raise RuntimeError('Development evidence destination already exists')
                shutil.copytree(directory,output)
                (output/'development-scope.json').write_text(json.dumps({'scope':'isolated-headless-production-P4-controller',
                    'physical_acceptance':False,'kernel_uinput_qualified':False,'capture_and_pixel_audit':True,
                    'profile':profile},indent=2)+'\n')
                import html
                text='<html><meta charset="utf-8"><title>P4 development evidence</title><h1>P4 isolated scaling evidence</h1><p>Headless Vela; fixture input. This is not physical TTY qualification.</p><table><tr><th>Case</th><th>Different pattern pixels / corpus span error (px)</th><th>Offscreen pixels</th><th>Original PNG</th></tr>'
                for name,row in outcome['scenarios'].items():
                    text+=f"<tr><td>{html.escape(name)}</td><td>{row['metrics'].get('different_pixels',row['metrics'].get('span_error_pixels'))}</td><td>{row['metrics']['offscreen_pixels']}</td><td><a href='{html.escape(row['image_file'])}'>capture</a></td></tr>"
                (output/'report.html').write_text(text+'</table></html>')

if __name__=='__main__':unittest.main()
