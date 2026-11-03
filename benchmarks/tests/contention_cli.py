#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native worker lifecycle with actual CPU counters on one allowed core."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vela_bench import contention
from vela_bench.calibration import Lines,stop_process
binary=str(Path(sys.argv[1]).resolve())
for args in ([],['--workers','1'],['--json'],['--until-stdin-eof']):
 assert subprocess.run([binary,*args],capture_output=True,timeout=3).returncode==2
if os.getuid()==0:sys.exit(77)
def start():
 cpu=min(os.sched_getaffinity(0))
 return subprocess.Popen(['taskset','-c',str(cpu),binary,'--json','--until-stdin-eof'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,start_new_session=True)
p=start()
try:
 lines=Lines(p)
 while not lines.records or not contention.ready(lines.records,'cpu',os.getuid()):lines.next(time.monotonic()+4)
 begin=time.monotonic_ns()
 while time.monotonic_ns()<begin+1_100_000_000:lines.next(time.monotonic()+3)
 end=time.monotonic_ns();p.stdin.close();p.stdin=None
 while lines.next(time.monotonic()+4)['event']!='cpu-load-summary':pass
 assert p.wait(timeout=3)==0
 assert contention.metrics(lines.records,'cpu',[begin,end],os.getuid())['worker_count']==1
 for worker in lines.records[0]['workers']:
  assert not Path('/proc',str(worker['pid'])).exists(),'owned CPU worker survived EOF'
finally:stop_process(p)
p=start()
try:
 lines=Lines(p);meta=lines.next(time.monotonic()+4)
 while not contention.ready(lines.records,'cpu',os.getuid()):lines.next(time.monotonic()+4)
 p.stdin.write(b'not-an-eof\n');p.stdin.flush()
 while lines.next(time.monotonic()+4)['event']!='cpu-load-summary':pass
 assert p.wait(timeout=3)!=0 and lines.records[-1]['complete'] is False
 for worker in meta['workers']:assert not Path('/proc',str(worker['pid'])).exists()
finally:stop_process(p)
print('CPU worker counters, EOF cleanup and command refusal passed')
