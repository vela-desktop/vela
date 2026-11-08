#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Full P2 production controller on private Vela; virtual input, two CPU cores.

Headless feedback explicitly bypasses physical-output parsing only in this
unprivileged test. No primary runner flag can enable that path.
"""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'benchmarks'),str(ROOT/'tests/functional')]
from harness import Session,tool
from vela_bench import measurements
from vela_bench.archive import atomic_json
from vela_bench.host import Layout

KEYBOARD='''import json,sys,subprocess,time
p=subprocess.Popen([sys.argv[1],'-'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
try:
    time.sleep(.3)
    print(json.dumps({'event':'uinput-ready','method':'linux-uinput-f24-v1','clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'}),flush=True)
    for index,line in enumerate(sys.stdin,1):
        before=time.monotonic_ns();p.stdin.write('keycode\\t194\\n');p.stdin.flush()
        if p.stdout.readline().strip()!='OK':raise RuntimeError('virtual input failed')
        print(json.dumps({'event':'injected','action_id':index,'before_monotonic_ns':before,'after_monotonic_ns':time.monotonic_ns()}),flush=True)
finally:
    p.stdin.close();p.wait(timeout=3)
'''


def main():
    with Session(size='1280x720@60') as session,tempfile.TemporaryDirectory() as temporary:
        directory=Path(temporary);layout=Layout(directory);groups={}
        cpus=','.join(map(str,sorted(os.sched_getaffinity(0))[:2]))
        class Host:
            def spawn(self,command,errors):
                role=next((v.removeprefix('--unit=') for v in command if v.startswith('--unit=')),'fixture-input')
                if command[0].endswith('vela-bench-input'):argv=[sys.executable,'-c',KEYBOARD,tool('vela-input')]
                else:
                    at=next(i for i,v in enumerate(command) if Path(v).name in ('vela-bench-probe','vela-slowgpu','vela-bench-cpuload'))
                    argv=[tool(Path(command[at]).name),*command[at+1:]]
                    if argv[0].endswith('vela-bench-cpuload'):argv=['taskset','-c',cpus,*argv]
                p=subprocess.Popen(argv,env=session.client_env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                    stderr=errors,bufsize=0,start_new_session=True)
                groups[role]=p;return p
            def run(self,command,**options):
                if '--property=ActiveState' in command:return SimpleNamespace(stdout='inactive\n',returncode=0)
                if str(layout.probe) in command:
                    at=command.index(str(layout.probe))
                    return subprocess.run([tool('vela-bench-probe'),*command[at+1:]],env=session.client_env,
                        capture_output=True,text=True,timeout=options.get('timeout',65))
                if 'stop' in command:
                    p=groups.get(command[-1])
                    if p and p.poll() is None:
                        os.killpg(p.pid,signal.SIGTERM)
                        try:p.wait(timeout=3)
                        except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=3)
                return SimpleNamespace(stdout='',stderr='',returncode=0)
        original_motion,original_producer=measurements.motion_metrics,measurements.producer_metrics
        def motion(raw,rows,profile,**options):return original_motion(raw,rows,profile,**dict(options,physical_output_required=False))
        def producer(rows,mode,window,**options):return original_producer(rows,mode,window,**dict(options,physical_output_required=False))
        profile={'mode':{'refresh_mhz':60000}}
        atomic_json(directory/'vela-session.json',{'session':{'User':str(os.getuid())}})
        try:
            with patch.object(measurements,'motion_metrics',motion),patch.object(measurements,'producer_metrics',producer):
                result=measurements.run_workload(Host(),{'uid':os.getuid(),'gid':os.getgid()},directory,'vela',layout,profile,'slow-apps',lambda:None,
                    progress=lambda label:print(label,flush=True))
                assert result['status']=='passed',result['failures']
                assert len(result['scenarios'])==8
                assert measurements.audit_measurements(directory,'vela',profile,'slow-apps')==result
                for kind in ('cpu','gpu'):
                    sample=result['scenarios'][kind]
                    assert sample['input']['actions']==100 and sample['motion']['submitted']==600
                    assert sample['contention']['window_covered']
                print('P2_FULL_PASSED '+json.dumps({kind:result['scenarios'][kind]['contention'] for kind in ('cpu','gpu')}),flush=True)
        finally:
            for p in groups.values():
                try:os.killpg(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:p.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=3)
            if os.environ.get('VELA_P2_EVIDENCE'):
                destination=Path(os.environ['VELA_P2_EVIDENCE']);shutil.copytree(directory,destination)
                atomic_json(destination/'development-scope.json',{'physical_acceptance':False,'kernel_uinput':False,
                    'scope':'private-headless-production-P2; virtual-input; CPU-affinity-limited-to-two-cores','profile':profile})

def motion_only(env,target,directory,tools):
    """Cross-compositor load + real foreground motion; no input timing claim."""
    from concurrent.futures import ThreadPoolExecutor
    from vela_bench.calibration import Lines,stop_process
    from vela_bench.contention import ready,metrics
    result={}
    cpus=','.join(map(str,sorted(os.sched_getaffinity(0))[:2]))
    def animate(label):
        raw=subprocess.run([str(tools/'vela-bench-probe'),'--frames','630','--timeout','55'],
            env=env,capture_output=True,text=True,timeout=60)
        assert raw.returncode==0,raw.stderr
        (directory/(target+'-'+label+'.jsonl')).write_text(raw.stdout)
        rows=[json.loads(line) for line in raw.stdout.splitlines()]
        return measurements.motion_metrics(raw.stdout,rows,{'mode':{'refresh_mhz':60000}},physical_output_required=False)
    for kind in ('cpu','gpu'):
        control=animate(kind+'-control')
        command=([str(tools/'vela-bench-cpuload'),'--json','--until-stdin-eof'] if kind=='cpu' else
                 [str(tools/'vela-slowgpu'),'--contention','--json','--until-stdin-eof'])
        if kind=='cpu':command=['taskset','-c',cpus,*command]
        with (directory/(target+'-'+kind+'.stderr')).open('wb') as errors,ThreadPoolExecutor(max_workers=1) as pool:
            process=subprocess.Popen(command,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,bufsize=0,start_new_session=True)
            lines=Lines(process)
            try:
                while not lines.records or not ready(lines.records,kind,os.getuid()):lines.next(time.monotonic()+15)
                def drain():
                    while lines.next(time.monotonic()+5).get('event')!=kind+'-load-summary':pass
                future=pool.submit(drain)
                loaded=animate(kind)
                process.stdin.close();process.stdin=None;future.result(timeout=6)
                assert process.wait(timeout=3)==0
                load=metrics(lines.records,kind,loaded['window_monotonic_ns'],os.getuid())
                result[kind]={'control':control,'motion':loaded,'contention':load,'input_measured':False}
                # Reopen the exact stream rather than trusting the live summary.
                file=directory/(target+'-'+kind+'-load.jsonl')
                file.write_text(''.join(json.dumps(row)+'\n' for row in lines.records))
                assert metrics([json.loads(line) for line in file.read_text().splitlines()],kind,loaded['window_monotonic_ns'],os.getuid())==load
            finally:
                stop_process(process)
                try:os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError:pass
    atomic_json(directory/(target+'-contention-motion.json'),result)
    return {'conditions':list(result),'scope':'private-compositor; real CPU/GPU work and foreground motion; no kernel input or physical timing qualification',
            'measured_frames_per_condition':600,'raw_audit':True}

if __name__=='__main__':
    if os.getuid()==0 or not any(Path('/dev/dri').glob('renderD*')):
        print('SKIP: private P2 integration requires an unprivileged user and GPU render node');sys.exit(77)
    main()
