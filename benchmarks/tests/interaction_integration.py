#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real native window sequence/controller; root transport/accounting are fixtures."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'benchmarks'),str(ROOT/'tests/functional')]


def run_condition(env,target,profile,directory,pattern,keyboard):
    from vela_bench.host import Layout
    from vela_bench.platform_workloads import resource_condition,resource_metrics
    from vela_bench.archive import read_json
    from test_platform_workloads import sample
    processes=[];counter=0
    class Host:
        def spawn(self,command,errors):
            if command[0].endswith('vela-bench-input'):argv=keyboard
            else:
                at=next(i for i,v in enumerate(command) if v.endswith('/vela-pattern'))
                argv=[str(pattern),*command[at+1:]]
            process=subprocess.Popen(argv,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,bufsize=0)
            processes.append(process);return process
        def run(self,command,**options):
            if '--property=ActiveState' in command:return SimpleNamespace(stdout='inactive\n',returncode=0)
            assert command[-3:]==['--user','stop','vela-benchmark-measurement.service'],command
            return SimpleNamespace(stdout='',returncode=0)
    def snapshot(uid,**options):
        nonlocal counter
        assert uid==1001
        counter+=1
        return sample(time.monotonic_ns(),100000+counter*1000)
    with patch('vela_bench.resources.snapshot',side_effect=snapshot),patch('vela_bench.resources.transport_identity',return_value=None):
        result=resource_condition(Host(),{'uid':1001,'gid':1001},directory,target,Layout(directory),
                                  'window-interaction',lambda:None,None,profile)
    source=read_json(directory/f'{target}-resources-window-interaction.json')
    raw=(directory/f'{target}-resources-window-interaction.work.jsonl').read_text()
    assert resource_metrics(source['samples'],1001,'window-interaction',raw,target=target,profile=profile)==result
    assert source['interaction_input_cleanup']=='passed'
    assert all(p.poll() is not None for p in processes)
    assert result['workload']['verified_actions']==24
    assert result['samples']>=18 and result['capture_enabled'] is False
    return {'target':target,'verified_actions':24,'native_state_and_buffer_records':True,
            'production_async_resource_controller':True,'raw_audit_and_owned_process_cleanup':True,
            'capture_enabled':False,'kernel_uinput_qualified':False,'resource_counters':'CPU-fixture-only',
            'root_transport':'fixture-only','latency_measurement':False,'physical_acceptance':False}


def main():
    from harness import Session,tool
    from p4_integration import KEYBOARD
    from vela_bench.conditions import normalized_outputs,desired_profile
    with Session(size='1920x1080@60') as session,tempfile.TemporaryDirectory(prefix='vela-interaction-') as temporary:
        initial=normalized_outputs(subprocess.check_output(['wlr-randr','--json'],env=session.client_env,text=True),'vela')
        mode=initial[0]['modes'][0]
        profile=desired_profile(initial,initial[0]['name'],{k:mode[k] for k in ('width','height','refresh_mhz')})
        result=run_condition(session.client_env,'vela',profile,Path(temporary),tool('vela-pattern'),
                             [sys.executable,'-c',KEYBOARD,tool('vela-input')])
        print(json.dumps(result,indent=2))


if __name__=='__main__':main()
