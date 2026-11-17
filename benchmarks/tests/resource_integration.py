#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Actual waiting launch client and separate worker; private harmless user unit.

No root/PAM/TTY/hardware operations. The root-controller cgroup exception is
covered by CPU fixtures; this check uses the caller's real user-manager cgroups.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vela_bench import resources
from vela_bench.host import SAFE_ENV


def main():
    uid=os.getuid()
    if uid==0:raise RuntimeError('Run the private resource check unprivileged')
    env=dict(SAFE_ENV,XDG_RUNTIME_DIR=f'/run/user/{uid}',DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{uid}/bus')
    unit='vela-benchmark-resource-check-'+uuid.uuid4().hex+'.service'
    program="import json,os,time;from pathlib import Path;print(json.dumps({'pid':os.getpid(),'cgroup':Path('/proc/self/cgroup').read_text().strip()}),flush=True);time.sleep(3);print('complete',flush=True)"
    command=['/usr/bin/systemd-run','--user','--wait','--pipe','--collect','--quiet',
             '--service-type=exec','--unit='+unit,'--property=RuntimeMaxSec=8',sys.executable,'-I','-c',program]
    process=subprocess.Popen(command,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        worker=json.loads(process.stdout.readline())
        identity=resources.transport_identity(process,command)
        assert identity and identity['cgroup']==Path('/proc/self/cgroup').read_text().strip(),identity
        assert worker['cgroup']!=identity['cgroup'] and unit in worker['cgroup'],worker
        with tempfile.TemporaryDirectory(prefix='vela-resource-check-') as temporary:
            # Enumerate only our two live fixture PIDs, reading their actual proc
            # records and the real UID counter. No other process is inventoried.
            proc=Path(temporary)
            for pid in (process.pid,worker['pid']):(proc/str(pid)).symlink_to(Path('/proc')/str(pid))
            rows=[resources.snapshot(uid,proc=proc,transport=identity)]
            time.sleep(.1)
            rows.append(resources.snapshot(uid,proc=proc,transport=identity))
            for row in rows:
                assert [p['pid'] for p in row['processes']]==[worker['pid']],row
                assert [p['pid'] for p in row['controller_transports']]==[process.pid],row
            result=resources.metrics(rows,uid,identity)
            assert result['collector_transport_samples']==2,result
            assert result['collector_transport_in_uid_cpu_totals'] is True,result
        output,errors=process.communicate(timeout=6)
        assert process.returncode==0 and output.strip()==b'complete',(output,errors)
        print(json.dumps({'scope':'real-user-manager-private-sleep-unit-no-root-pam-tty-hardware',
            'client_stays_in_controller_cgroup':True,'workload_in_separate_user_service':True,
            'live_pid_start_parent_command_identity_verified':True,'transport_pss_recorded_separately':True,
            'offline_metrics_recomputed':True,'root_controller_cgroup_case':'CPU-fixture-only'}))
    finally:
        subprocess.run(['systemctl','--user','stop',unit],env=env,stdin=subprocess.DEVNULL,capture_output=True,timeout=8)
        if process.poll() is None:process.terminate()
        try:process.communicate(timeout=3)
        except subprocess.TimeoutExpired:process.kill();process.communicate(timeout=3)


if __name__=='__main__':main()
