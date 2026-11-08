#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""P3 v2 production controller on a private compositor; fixture input only."""
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'benchmarks'),str(ROOT/'tests/functional')]
from harness import Session,tool,BUILD
from vela_bench import desktop
from vela_bench.host import Layout
from vela_bench.archive import atomic_json

KEYBOARD=r'''import json,subprocess,sys,time
p=subprocess.Popen([sys.argv[1],'-'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
def send(command):
 p.stdin.write(command+'\n');p.stdin.flush()
 if p.stdout.readline().strip()!='OK':raise RuntimeError('virtual input fixture failed: '+command)
try:
 time.sleep(.3)
 print(json.dumps({'event':'uinput-ready','method':'linux-uinput-fixed-desktop-v1','clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'}),flush=True)
 mapping={'launcher':'keycode\t125','escape':'keycode\t1','pulse':'keycode\t194','enter':'keycode\t28',
   'search':'type\tvelabenchscene','close':'key\talt+F4','desktop':'key\tsuper+d',
   'down':'down\tleft','up':'up\tleft','alt-tab-begin':'keydown\tAlt_L','alt-tab-end':'keyup\tAlt_L',
   'window-menu':'key\talt+space','maximize':'key\tsuper+Up','restore':'key\tsuper+Down',
   'snap-left':'key\tsuper+Left','snap-right':'key\tsuper+Right',
   'workspace-next':'key\tctrl+super+Right','workspace-previous':'key\tctrl+super+Left'}
 for index,line in enumerate(sys.stdin,1):
  c=line.strip()
  if c=='quit':break
  before=time.monotonic_ns()
  if c.startswith('point '):
   _,x,y=c.split();send('move\t'+str(round(int(x)*1919/65535))+'\t'+str(round(int(y)*1079/65535)))
  else:
   send(mapping[c])
   if c=='alt-tab-begin':send('keycode\t15')
  print(json.dumps({'event':'injected','action_id':index,'before_monotonic_ns':before,'after_monotonic_ns':time.monotonic_ns()}),flush=True)
finally:p.stdin.close();p.wait(timeout=3)
'''
CAPTURE=r'''import subprocess,sys,time,json
screen,adapter,path=sys.argv[1:]
for i in range(12):
 begin=time.monotonic_ns();subprocess.run([screen,path],check=True,stdout=subprocess.DEVNULL)
 end=time.monotonic_ns()
 row=json.loads(subprocess.check_output([adapter,'--analyze',path],text=True))
 row.update(event='capture',backend='ext-image-copy-capture-v1',index=i,begin_monotonic_ns=str(begin),end_monotonic_ns=str(end))
 print(json.dumps(row),flush=True)
 time.sleep(.1)
'''


def check(env,target,directory,phase,*,session=None,input_command=None,audit=True):
    layout=Layout(directory);layout.home.mkdir(parents=True,exist_ok=True);layout.helper.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(tool('vela-bench-scene'),layout.helper.parent/'vela-bench-scene');(layout.helper.parent/'vela-bench-scene').chmod(0o755)
    if not (layout.home/'.local/share/applications').exists():desktop.prepare_scene(layout,{'uid':os.getuid(),'gid':os.getgid()})
    processes={};cpus=','.join(map(str,sorted(os.sched_getaffinity(0))[:2]))
    class Host:
        def spawn(self,command,errors):
            role=next((v.removeprefix('--unit=') for v in command if v.startswith('--unit=')),'fixture-input')
            if command[0].endswith('vela-bench-input'):argv=input_command or [sys.executable,'-c',KEYBOARD,tool('vela-input')]
            elif '_capture' in command:
                argv=([sys.executable,'-c',CAPTURE,tool('vela-screen-dump'),tool('vela-bench-desktop'),str(directory/'transient.png')] if target=='vela' else
                      [tool('vela-bench-desktop'),'--capture',profile['primary'],'12'])
            else:
                at=next(i for i,v in enumerate(command) if Path(v).name in ('vela-bench-scene','vela-bench-desktop','vela-slowgpu','vela-bench-cpuload'))
                argv=[tool(Path(command[at]).name),*command[at+1:]]
                if argv[0].endswith('vela-bench-cpuload'):argv=['taskset','-c',cpus,*argv]
            p=subprocess.Popen(argv,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,bufsize=0,start_new_session=True)
            processes[role]=p;return p
        def run(self,command,**options):
            if 'show' in command and '--property=ActiveState' in command:
                return SimpleNamespace(stdout='inactive\n',returncode=0)
            if command[-1].endswith('.service') and 'stop' in command:
                p=processes.get(command[-1])
                if p and p.poll() is None:p.terminate();p.wait(timeout=3)
                return SimpleNamespace(stdout='',returncode=0)
            at=next(i for i,v in enumerate(command) if Path(v).name in ('vela-bench-scene','vela-bench-desktop'))
            return subprocess.run([tool(Path(command[at]).name),*command[at+1:]],env=env,capture_output=True,text=True,timeout=options.get('timeout',30),check=True)
    profile={'primary':'HEADLESS-1' if target=='vela' else 'Virtual-1','mode':{'width':1920,'height':1080,'refresh_mhz':60000}}
    if target=='plasma':
        from vela_bench.conditions import normalized_outputs
        outputs=normalized_outputs(subprocess.check_output([tool('vela-bench-desktop'),'--outputs'],env=env,text=True),'plasma')
        profile['primary']=next(o['name'] for o in outputs if o['enabled'])
    original=desktop.producer_metrics
    def producer(rows,mode,window,**kwargs):return original(rows,mode,window,physical_output_required=False,**kwargs)
    try:
        with patch.object(desktop,'producer_metrics',producer):
            result=desktop.run_observation(Host(),{'uid':os.getuid(),'gid':os.getgid()},directory,target,layout,profile,phase,
                lambda:None,progress=lambda name:print(name,flush=True))
            assert result['status']=='passed',result['failures']
            if audit:assert desktop.audit_observation(directory,target,profile,phase)==result
            print('P3_V2_PASSED '+json.dumps({'target':target,'phase':phase,'actions':len(result['actions']),'raw_audit':audit,'physical_acceptance':False}),flush=True)
            return result
    finally:
        for p in processes.values():
            try:os.killpg(p.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=3)
        atomic_json(directory/'development-scope.json',{'physical_acceptance':False,'input':'fixture','profile':profile})


def main(phase):
    with tempfile.TemporaryDirectory(prefix='vela-p3-private-') as temp:
        root=Path(temp);runtime=root/'runtime';runtime.mkdir(mode=0o700);directory=root/'evidence';directory.mkdir()
        layout=Layout(directory);layout.home.mkdir(parents=True)
        layout.helper.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(tool('vela-bench-scene'),layout.helper.parent/'vela-bench-scene');(layout.helper.parent/'vela-bench-scene').chmod(0o755)
        desktop.prepare_scene(layout,{'uid':os.getuid(),'gid':os.getgid()})
        env={'HOME':str(layout.home),'XDG_RUNTIME_DIR':str(runtime),'XDG_STATE_HOME':str(layout.home/'.local/state'),
            'XDG_DATA_HOME':str(layout.home/'.local/share'),'DBUS_SYSTEM_BUS_ADDRESS':'unix:path='+str(root/'no-system-bus')}
        try:
            with Session(size='1920x1080@60',env=env,startup=shlex.quote(str(Path(BUILD)/'shell/vela-shell')) if phase!='motion' else None) as session:
                if phase!='motion':time.sleep(2)
                check(session.client_env,'vela',directory,phase,session=session,audit=os.environ.get('VELA_P3_DEFER_AUDIT')!='1')
        finally:
            if os.environ.get('VELA_P3_EVIDENCE'):shutil.copytree(directory,os.environ['VELA_P3_EVIDENCE'])

def vela_private(phase):
    # D-Bus-activated services can inherit test stdout after the controller
    # exits. Own their process group and log file as well as the compositor.
    with tempfile.TemporaryDirectory(prefix='vela-p3-bus-') as temporary:
        path=Path(temporary)/'session.log'
        with path.open('w') as log:
            process=subprocess.Popen(['dbus-run-session','--',sys.executable,str(Path(__file__).resolve()),'--vela-child',phase],
                stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                status=process.wait(timeout=850)
                print(path.read_text()[-8000:])
                if status:raise RuntimeError('Private Vela P3 failed: '+str(status))
            finally:
                try:os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=3)


def plasma(phase):
    with tempfile.TemporaryDirectory(prefix='vela-p3-kwin-') as temporary:
        base=Path(temporary);runtime=base/'runtime';runtime.mkdir(mode=0o700);directory=base/'evidence';directory.mkdir()
        layout=Layout(directory);layout.home.mkdir(parents=True);layout.helper.parent.mkdir(parents=True)
        shutil.copyfile(tool('vela-bench-scene'),layout.helper.parent/'vela-bench-scene');(layout.helper.parent/'vela-bench-scene').chmod(0o755)
        desktop.prepare_scene(layout,{'uid':os.getuid(),'gid':os.getgid()})
        env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','HOME':str(layout.home),'XDG_RUNTIME_DIR':str(runtime),
             'XDG_CONFIG_HOME':str(layout.home/'.config'),'XDG_DATA_HOME':str(layout.home/'.local/share'),
             'XDG_STATE_HOME':str(layout.home/'.local/state'),'XDG_CACHE_HOME':str(layout.home/'.cache'),
             'QT_QPA_PLATFORM':'wayland','XDG_CURRENT_DESKTOP':'KDE','XDG_SESSION_TYPE':'wayland','KDE_FULL_SESSION':'true','QT_FORCE_STDERR_LOGGING':'1','QT_LOGGING_TO_CONSOLE':'1','DBUS_SYSTEM_BUS_ADDRESS':'unix:path='+str(base/'no-system-bus'),
             'VELA_BUILD':str(BUILD),'VELA_P3_DEFER_AUDIT':os.environ.get('VELA_P3_DEFER_AUDIT','')}
        command=['dbus-run-session','--','kwin_wayland','--virtual','--width','1920','--height','1080',
            '--socket','wayland-private-p3','--no-lockscreen','--no-kactivities','--exit-with-session',
            shlex.join([sys.executable,str(Path(__file__).resolve()),'--plasma-child',str(directory),phase])]
        with (base/'kwin.log').open('w') as log:
            process=subprocess.Popen(command,env=env,start_new_session=True,stdout=log,stderr=subprocess.STDOUT)
            try:
                status=process.wait(timeout=360)
                print((base/'kwin.log').read_text()[-8000:])
                if status:raise RuntimeError('Private KWin P3 failed: '+str(status))
            finally:
                try:os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=3)
                if os.environ.get('VELA_P3_EVIDENCE'):shutil.copytree(directory,os.environ['VELA_P3_EVIDENCE'])


def plasma_child(directory,phase):
    env=dict(os.environ);shell=activities=None
    try:
        if phase!='motion':
            activities=subprocess.Popen(['/usr/lib/kactivitymanagerd'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            time.sleep(1)
            subprocess.run(['kbuildsycoca6','--noincremental'],env=env,check=True,timeout=30)
            with (Path(directory)/'plasmashell.log').open('wb') as errors:
                shell=subprocess.Popen(['plasmashell','--no-respawn'],env=env,stdout=errors,stderr=errors)
            time.sleep(5)
            if shell.poll() is not None:raise RuntimeError('Private plasmashell exited: '+(Path(directory)/'plasmashell.log').read_text()[-3000:])
        check(env,'plasma',Path(directory),phase,input_command=[tool('vela-bench-kwin-input-fixture')],audit=os.environ.get('VELA_P3_DEFER_AUDIT')!='1')
    finally:
        for process in (shell,activities):
            if process:process.terminate();process.wait(timeout=5)


if __name__=='__main__':
    if os.getuid()==0:sys.exit(77)
    if len(sys.argv)==1:sys.argv.append('motion')
    if sys.argv[1]=='--vela-child':main(sys.argv[2])
    elif sys.argv[1]=='--plasma-child':plasma_child(sys.argv[2],sys.argv[3])
    elif sys.argv[1]=='--plasma':plasma(sys.argv[2])
    else:vela_private(sys.argv[1] if len(sys.argv)>1 else 'motion')
