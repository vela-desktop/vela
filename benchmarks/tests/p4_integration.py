#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Production P4 scaling controller on a private headless Vela, fixture input only."""
import json
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
    def test_full_25_case_matrix_real_fractional_buffers_and_offline_pixel_audit(self):
        from types import SimpleNamespace
        with Session(size='1920x1080@60') as session,tempfile.TemporaryDirectory() as temporary:
            layout=Layout(Path(temporary));directory=Path(temporary)
            initial=normalized_outputs(subprocess.check_output(['wlr-randr','--json'],env=session.client_env,text=True),'vela')
            mode=initial[0]['modes'][0]
            profile=desired_profile(initial,initial[0]['name'],{k:mode[k] for k in ('width','height','refresh_mhz')})
            class Host:
                def spawn(self,command,errors):
                    if command[0].endswith('vela-bench-input'):
                        argv=[sys.executable,'-c',KEYBOARD,tool('vela-input')]
                    else:
                        at=next(i for i,v in enumerate(command) if v.endswith('/vela-pattern'))
                        argv=[tool('vela-pattern'),*command[at+1:]]
                    return subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,env=session.client_env,bufsize=0)
                def run(self,command,**options):
                    if 'wlr-randr' in command or '/usr/bin/wlr-randr' in command:
                        at=next(i for i,v in enumerate(command) if v.endswith('wlr-randr'))
                        return subprocess.run(command[at:],env=session.client_env,capture_output=True,text=True,check=True,timeout=15)
                    if '_capture' in command:
                        path=str(directory/'transient.png')
                        began=time.monotonic_ns();session.screenshot(path);end=time.monotonic_ns()
                        capture=json.loads(subprocess.check_output([tool('vela-bench-desktop'),'--analyze',path],env=session.client_env,text=True))
                        capture.update(event='capture',backend='ext-image-copy-capture-v1',index=0,
                                       begin_monotonic_ns=str(began),end_monotonic_ns=str(end))
                        return SimpleNamespace(stdout=json.dumps(capture),returncode=0)
                    return SimpleNamespace(stdout='',returncode=0)
            outcome=run_workload(Host(),{'uid':os.getuid(),'gid':os.getgid()},directory,'vela',layout,profile,'scaling',lambda:None)
            self.assertEqual(outcome['status'],'passed',outcome['failures'])
            self.assertEqual(len(outcome['scenarios']),25)
            self.assertEqual(audit_measurements(directory,'vela',profile,'scaling'),outcome)
            actual=normalized_outputs(subprocess.check_output(['wlr-randr','--json'],env=session.client_env,text=True),'vela')
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
                text='<html><meta charset="utf-8"><title>P4 development evidence</title><h1>P4 isolated scaling evidence</h1><p>Headless Vela; fixture input. This is not physical TTY qualification.</p><table><tr><th>Case</th><th>Different pixels</th><th>Offscreen pixels</th><th>Original PNG</th></tr>'
                for name,row in outcome['scenarios'].items():
                    text+=f"<tr><td>{html.escape(name)}</td><td>{row['metrics']['different_pixels']}</td><td>{row['metrics']['offscreen_pixels']}</td><td><a href='{html.escape(row['image_file'])}'>capture</a></td></tr>"
                (output/'report.html').write_text(text+'</table></html>')

if __name__=='__main__':unittest.main()
