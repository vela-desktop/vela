#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""P4 controller on private KWin, with D-Bus action fixtures, not kernel input."""
import json
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'benchmarks'))


def shortcut_bus():
    from gi.repository import Gio
    return Gio.bus_get_sync(Gio.BusType.SESSION,None)


def shortcut(bus,method,parameters=None):
    return bus.call_sync('org.kde.kglobalaccel','/component/kwin',
        'org.kde.kglobalaccel.Component',method,parameters,None,0,5000,None)


def invoke(bus,name):
    from gi.repository import GLib
    shortcut(bus,'invokeShortcut',GLib.Variant('(s)',(name,)))


def keyboard():
    bus=shortcut_bus()
    print(json.dumps({'event':'uinput-ready','method':'linux-uinput-fixed-desktop-v1',
        'clock':'CLOCK_MONOTONIC','sysfs':'/sys/devices/virtual/input/input999999999'}),flush=True)
    for index,line in enumerate(sys.stdin,1):
        command=line.strip();began=time.monotonic_ns()
        if command=='quit':break
        if command!='point 65535 65535':
            invoke(bus,{'snap-left':'Window Quick Tile Left','snap-right':'Window Quick Tile Right',
                        'maximize-toggle':'Window Maximize'}[command])
        # Pointer movement is a fixture/no-op: native capture excludes it.
        print(json.dumps({'event':'injected','action_id':index,'before_monotonic_ns':began,
                          'after_monotonic_ns':time.monotonic_ns()}),flush=True)


def child(adapter,directory):
    from types import SimpleNamespace
    from vela_bench.calibration import Lines,stop_process
    from vela_bench.conditions import normalized_outputs,desired_profile
    from vela_bench.host import Layout
    from vela_bench.platform_workloads import run_workload,audit_measurements
    env=dict(os.environ)
    def native(*args):
        return subprocess.run([str(adapter),*args],env=env,capture_output=True,text=True,check=True,timeout=20)
    bus=shortcut_bus()
    infos={row[0]:row for row in shortcut(bus,'allShortcutInfos').unpack()[0]}
    # Both effective and default sequences must match an untouched KWin profile.
    keys={'Window Maximize':0x11000016,'Window Quick Tile Top':0x11000013,
          'Window Quick Tile Left':0x11000012,'Window Quick Tile Right':0x11000014}
    for name,code in keys.items():
        assert infos[name][-2:]==([code],[code]),infos[name]
    initial=normalized_outputs(native('--outputs').stdout,'plasma')
    mode=next(m for m in initial[0]['modes'] if m['id']==initial[0]['current_mode_id'])
    profile=desired_profile(initial,initial[0]['name'],{k:mode[k] for k in ('width','height','refresh_mhz')})
    # Virtual outputs have no controllable hardware VRR policy. Preserve their
    # reported policy; this fixture qualifies actions/capture, not hardware.
    profile['outputs'][0]['vrr']=initial[0]['vrr']
    # Reproduce the original stimulus: Meta+Up is top tiling, not maximize.
    process=subprocess.Popen([str(adapter.parent/'vela-pattern'),'--benchmark','401','301'],
        env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        lines=Lines(process);meta=lines.next(time.monotonic()+8);time.sleep(.6)
        invoke(bus,'Window Quick Tile Top');time.sleep(.7)
        while lines.pending or select.select([process.stdout],[],[],.15)[0]:
            meta=lines.next(time.monotonic()+3)
        assert meta['maximized'] is False and meta['buffer_height']<.65*profile['mode']['height'],meta
        original_failure_reproduced=True
    finally:stop_process(process)
    layout=Layout(directory)
    class Host:
        def spawn(self,command,errors):
            if command[0].endswith('vela-bench-input'):
                argv=[sys.executable,str(Path(__file__).resolve()),'--keyboard']
            else:
                at=next(i for i,v in enumerate(command) if v.endswith('/vela-pattern'))
                argv=[str(adapter.parent/'vela-pattern'),*command[at+1:]]
            return subprocess.Popen(argv,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,bufsize=0)
        def run(self,command,**options):
            if '_capture' in command:
                return native('--capture',profile['primary'],'1')
            if '--outputs' in command:return native('--outputs')
            if '--apply' in command:return native('--apply',command[command.index('--apply')+1])
            return SimpleNamespace(stdout='',returncode=0)
    result=run_workload(Host(),{'uid':os.getuid(),'gid':os.getgid()},directory,'plasma',layout,
                        profile,'scaling',lambda:None)
    assert result['status']=='passed',result['failures']
    assert len(result['scenarios'])==25
    assert audit_measurements(directory,'plasma',profile,'scaling')==result
    restored=normalized_outputs(native('--outputs').stdout,'plasma')
    assert next(o for o in restored if o['enabled'])['scale']==1.
    print('PLASMA_SCALING_EVIDENCE '+json.dumps({'scope':'private-kwin-production-P4-controller',
        'physical_acceptance':False,'kernel_uinput_qualified':False,'default_shortcuts_verified':True,
        'wrong_top_tile_action_reproduced':original_failure_reproduced,'cases':len(result['scenarios']),
        'offline_pixel_audit':True,'scale1_restored':True}),flush=True)


def main(adapter):
    try:import gi
    except ImportError:
        print('SKIP: private KWin shortcut check needs PyGObject');return 77
    if os.getuid()==0:raise RuntimeError('Run the private KWin check unprivileged')
    with tempfile.TemporaryDirectory(prefix='vela-plasma-scaling-') as temporary:
        base=Path(temporary);runtime=base/'runtime';runtime.mkdir(mode=0o700)
        home=base/'home';home.mkdir(mode=0o700)
        env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','HOME':str(home),'XDG_RUNTIME_DIR':str(runtime),
             'XDG_CONFIG_HOME':str(home/'config'),'XDG_DATA_HOME':str(home/'data'),
             'XDG_STATE_HOME':str(home/'state'),'XDG_CACHE_HOME':str(home/'cache'),
             'QT_QPA_PLATFORM':'wayland','DBUS_SYSTEM_BUS_ADDRESS':'unix:path='+str(base/'no-system-bus')}
        command=['dbus-run-session','--','kwin_wayland','--virtual','--width','1920','--height','1080',
            '--socket','wayland-vela-scaling-check','--no-lockscreen','--no-kactivities','--exit-with-session',
            shlex.join([sys.executable,str(Path(__file__).resolve()),'--child',str(Path(adapter).resolve()),str(base)])]
        process=subprocess.Popen(command,env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            log,_=process.communicate(timeout=160)
            records=[line.removeprefix('PLASMA_SCALING_EVIDENCE ') for line in log.splitlines()
                     if line.startswith('PLASMA_SCALING_EVIDENCE ')]
            if len(records)!=1:raise RuntimeError('Private KWin scaling failed: '+log[-8000:])
            print(json.dumps(json.loads(records[0]),indent=2))
        finally:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            try:process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=3)
    return 0


if __name__=='__main__':
    if sys.argv[1]=='--keyboard':keyboard()
    elif sys.argv[1]=='--child':child(Path(sys.argv[2]),Path(sys.argv[3]))
    else:sys.exit(main(sys.argv[1]))
