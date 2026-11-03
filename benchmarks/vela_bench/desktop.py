# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""P3 complete coarse desktop actions, including paired observed load cases."""
import json
import hashlib
from pathlib import Path
import select
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

from . import BenchError,desktop_legacy as legacy,contention
from .archive import atomic_json,read_json,decode_json
from .calibration import Lines,stop_process
from .diagnostics import user_command,stop_instrument
from .desktop_pixels import evidence
from .measurements import producer_metrics
from .platform_workloads import scaling_actions

SCENE,LAUNCHED,DRAG_GRIP=legacy.SCENE,legacy.LAUNCHED,legacy.DRAG_GRIP
PEER=0x715002
prepare_scene=legacy.prepare_scene
changed=legacy.changed
marker=legacy.marker
validate_frames=legacy.validate_frames
# Keep the original raw action functions available for historical callers.
action_metrics=legacy.action_metrics
VERSION='p3-desktop-observation-v2'
PHASES=('responsiveness','motion','slow-apps')
SPEC=dict(legacy.SPEC,version=VERSION,phases=list(PHASES),pixel_audit='native-marker-grid-from-original-png-v1',
          menu_changed_tiles_minimum=4,producer_static_pixels=True,scene_method='native-desktop-scene-v2',alt_tab='held-native-switcher-and-identified-focus-return',
          window_menu={'vela':'Alt+Space','plasma':'Alt+F3'},
          motion=['drag','resize','snap-left','snap-right','maximize','restore','workspace-out','workspace-back'],
          workspaces='two-workspace-fixture; preserve-default-effects; remove-only-owned-addition',
          loads=['explicit','implicit','cpu','gpu'],load_controls='fresh-launcher-and-window-drag-before-each-load',
          contention=dict(contention.SPEC,capture=True,controls='fresh-launcher-and-drag-without-load'),observed_stable_state='coarse-fingerprint-only; not-usable-or-animation-completion')


def integer(row,key):
    value=row.get(key)
    if type(value) is not int or value<0:raise BenchError('Invalid desktop source clock/identity: '+key)
    return value


def sources_audit(stream,identity,uid):
    if type(uid) is not int or uid<=0 or not stream or stream[0].get('event')!='scene-ready':raise BenchError('Missing native scene identity/readiness')
    states=[];sequence=0;previous=-1;pid=None
    for index,row in enumerate(stream):
        if row.get('event')=='scene-ready':
            if row!={'event':'scene-ready','protocol':'wayland','width':640,'height':360} or index!=0:raise BenchError('Invalid scene readiness')
            continue
        stamp=integer(row,'monotonic_ns')
        if stamp<previous or row.get('id')!=identity:raise BenchError('Desktop scene source ordering/identity changed')
        previous=stamp
        if row.get('event') in ('move-request','resize-request'):
            if row.get('accepted') is not True:raise BenchError('Desktop system move/resize rejected')
            continue
        if row.get('event')!='scene-state' or row.get('method')!=SPEC['scene_method'] or row.get('protocol')!='wayland' or row.get('uid')!=uid:
            raise BenchError('Desktop source backend/UID/method changed')
        if pid is None:pid=integer(row,'pid')
        sequence+=1
        if (row.get('sequence')!=sequence or integer(row,'pid')!=pid or not pid or row.get('scale')!=1
                or any(type(row.get(k)) is not bool for k in ('active','maximized'))
                or any(type(row.get(k)) is not int or not 240<=row[k]<=8192 for k in ('width','height'))):
            raise BenchError('Invalid desktop configure/focus source')
        states.append(row)
    if not states:raise BenchError('Missing native desktop states')
    return states


def state_at(states,stamp):
    valid=[s for s in states if s['monotonic_ns']<=stamp]
    if not valid:raise BenchError('No source state before desktop observation')
    return valid[-1]


def visual_timing(rows,receipts,minimum=12):
    if not receipts:raise BenchError('Missing desktop stimulus')
    if len([r for r in rows[2:] if r['begin_monotonic_ns']>=receipts[0].get('after_monotonic_ns',0)])<2:raise BenchError('Insufficient post-action desktop observations')
    previous=rows[1]['end_monotonic_ns']
    for index,row in enumerate(receipts):
        if type(row.get('action_id')) is not int or row['action_id']<=0 or index and row['action_id']!=receipts[index-1]['action_id']+1:raise BenchError('Desktop stimulus sequence changed')
        if row.get('event')!='injected' or not previous<=integer(row,'before_monotonic_ns')<=integer(row,'after_monotonic_ns'):
            raise BenchError('Desktop action interval/order changed')
        previous=row['after_monotonic_ns']
    if previous>rows[-1]['end_monotonic_ns'] and not (receipts[-1].get('command')=='up'
            and rows[-1]['end_monotonic_ns']<=receipts[-1]['before_monotonic_ns']<=previous<=rows[-1]['end_monotonic_ns']+5_000_000_000):
        raise BenchError('Desktop action not covered by capture')
    if changed(rows[0]['tiles'],rows[1]['tiles'])>8:raise BenchError('Desktop pre-action state unsettled')
    before=receipts[0]['before_monotonic_ns']
    post=[r for r in rows[2:] if r['begin_monotonic_ns']>=receipts[0]['after_monotonic_ns']]
    changed_rows=[r for r in post if changed(rows[1]['tiles'],r['tiles'])>=minimum]
    result={'scope':'coarse-output-state-only','fine_grained_motion':False,'optical_latency':False,
            'changed_tiles':[changed(rows[1]['tiles'],r['tiles']) for r in rows],
            'maximum_capture_gap_ms':max(b['end_monotonic_ns']-a['begin_monotonic_ns'] for a,b in zip(rows,rows[1:]))/1e6,
            'first_observed_response_lower_ms':0,'first_observed_response_upper_ms':None,
            'first_observed_stable_state_upper_ms':None}
    if changed_rows:result['first_observed_response_upper_ms']=(changed_rows[0]['end_monotonic_ns']-before)/1e6
    final=rows[-1]['tiles'];stability_limit=min(8,minimum-1)
    for i,row in enumerate(post):
        if (changed_rows and row['begin_monotonic_ns']>=changed_rows[0]['begin_monotonic_ns']
                and len(post[i:])>=2 and all(changed(r['tiles'],final)<=stability_limit for r in post[i:])):
            result['first_observed_stable_state_upper_ms']=(row['end_monotonic_ns']-before)/1e6;break
    return result


def metrics(action,directory,target,profile,streams,uid,*,check_pixels=True,pixel_reader=evidence):
    rows=validate_frames(action['captures'],target,profile,directory,action['id'])
    for row in rows if check_pixels else []:
        pixels=pixel_reader(directory/row['image_file'])
        if any(row.get(k)!=pixels[k] for k in ('width','height','tiles','markers')) or row.get('rectangles')!=pixels['rectangles']:
            raise BenchError('Desktop derived metadata differs from original pixels')
    kind=action['kind'];receipts=action['injections'];commands=[r.get('command') for r in receipts]
    result=visual_timing(rows,receipts,SPEC['menu_changed_tiles_minimum'] if kind=='menu-open' else 12)
    state=sources_audit(streams['scene'],SCENE,uid)
    if any(s['monotonic_ns']>rows[-1]['end_monotonic_ns']+5_000_000_000 for s in state):raise BenchError('Desktop source clock outside captured work')
    final_state=state_at(state,rows[-1]['end_monotonic_ns'])
    rect=rows[-1]['rectangles'].get(str(SCENE))
    if kind in ('launcher-open','menu-open','alt-tab'):
        expected={'launcher-open':['launcher'],'menu-open':['window-menu' if target=='vela' else 'window-menu-plasma'],
                  'alt-tab':['alt-tab-begin','alt-tab-end']}[kind]
        if commands!=expected or result['first_observed_response_upper_ms'] is None:raise BenchError('Native UI did not visibly respond to its declared action')
        if kind=='alt-tab':
            peer=sources_audit(streams['peer'],PEER,uid)
            if (state_at(state,rows[1]['end_monotonic_ns'])['active'] is not False
                    or state_at(peer,rows[1]['end_monotonic_ns'])['active'] is not True
                    or not final_state['active'] or state_at(peer,rows[-1]['end_monotonic_ns'])['active']):
                raise BenchError('Alt+Tab did not exchange identified native focus')
            marker(rows[-1],SCENE)
        elif kind=='menu-open':
            marker(rows[1],SCENE)
            if not state_at(state,rows[1]['end_monotonic_ns'])['active'] or final_state['active']:raise BenchError('Native window menu did not take scene focus')
    elif kind in ('launcher-close','menu-close'):
        if commands!=['escape'] or changed(rows[-2]['tiles'],rows[-1]['tiles'])>8 or not final_state['active']:
            raise BenchError('Native panel/menu did not close and return focus')
        marker(rows[-1],SCENE)
    elif kind in ('drag','resize'):
        if (len(commands)!=12 or commands[1]!='down' or commands[-1]!='up'
                or any(not re.fullmatch(r'point [0-9]{1,5} [0-9]{1,5}',c) or any(int(v)>65535 for v in c.split()[1:])
                       for c in [commands[0],*commands[2:-1]])):
            raise BenchError('Desktop pointer workload changed')
        requests=[r for r in streams['scene'] if r.get('event')==('move-request' if kind=='drag' else 'resize-request')
                  and receipts[1]['before_monotonic_ns']<=r['monotonic_ns']<=rows[-1]['end_monotonic_ns']]
        if len(requests)!=1:raise BenchError('Missing accepted native move/resize request')
        positions=[r['rectangles'].get(str(SCENE)) for r in rows[2:]]
        start=rows[1]['rectangles'].get(str(SCENE))
        if not start or any(p is None for p in positions):raise BenchError('Moving/resizing scene is not fully identified')
        path=pointer_path(start,profile,kind)
        if commands!=[path[0],'down',*path[1:],'up']:raise BenchError('Desktop pointer trace differs from fixed physical path')
        selected=[p[:2] if kind=='drag' else p[2:] for p in positions]
        initial=start[:2] if kind=='drag' else start[2:]
        distance=sum((a-b)**2 for a,b in zip(selected[-1],initial))**.5
        if distance<80 or len({tuple(p) for p in selected})<3:raise BenchError('Insufficient observed native movement/resize')
        if kind=='resize' and any(abs(a-b)>2 for a,b in zip(rect[2:],[final_state['width'],final_state['height']])):
            raise BenchError('Resized pixels differ from native geometry')
        pauses=[];run_start=0
        for i in range(1,len(selected)):
            if selected[i]!=selected[i-1]:run_start=i
            elif i>run_start:pauses.append(rows[i+2]['end_monotonic_ns']-rows[run_start+2]['begin_monotonic_ns'])
        result.update(distance_pixels=distance,observed_geometry=positions,unchanged_adjacent_observations=sum(a==b for a,b in zip(selected,selected[1:])),
                      longest_observed_unchanged_interval_upper_ms=max(pauses,default=0)/1e6,missed_refreshes=None)
    elif kind in ('snap-left','snap-right','maximize','restore'):
        expected=scaling_actions(target,{'maximize':'maximized','restore':'restored'}.get(kind,kind))
        if commands!=expected or not rect or any(abs(a-b)>2 for a,b in zip(rect[2:],[final_state['width'],final_state['height']])):
            raise BenchError('Window transition action/pixel/source geometry changed')
        width=profile['mode']['width']
        if kind.startswith('snap-'):
            if not .35<=rect[2]/width<=.65 or final_state['maximized']:raise BenchError('Window did not tile to half output')
            if (kind=='snap-left' and rect[0]+rect[2]/2>=width/2 or kind=='snap-right' and rect[0]+rect[2]/2<=width/2):raise BenchError('Window snapped on wrong side')
        elif kind=='maximize':
            if not final_state['maximized'] or rect[2]<.9*width or rect[3]<.6*profile['mode']['height']:raise BenchError('Window did not maximize')
        elif (final_state['maximized'] or rect[2:]!=[640,360] or not any(s['maximized'] and receipts[0]['before_monotonic_ns']<=s['monotonic_ns']<=receipts[1]['before_monotonic_ns'] for s in state)):
            raise BenchError('Window did not maximize before restoring original size')
        result['final_geometry']=rect
    elif kind in ('workspace-out','workspace-back'):
        expected=('workspace-next' if kind=='workspace-out' else 'workspace-previous') if target=='vela' else ('workspace-second-plasma' if kind=='workspace-out' else 'workspace-first-plasma')
        if commands!=[expected]:raise BenchError('Workspace shortcut changed')
        workspace=action['workspace_after'];workspace_audit(workspace,target,uid)
        stamp=int(workspace['begin_monotonic_ns'])
        if not rows[-1]['end_monotonic_ns']<=stamp<=rows[-1]['end_monotonic_ns']+10_000_000_000:raise BenchError('Workspace receipt outside action interval')
        if workspace.get('uid')!=uid or workspace.get('target')!=target or workspace.get('count',0)<2 or workspace.get('current_index')!=(1 if kind=='workspace-out' else 0):
            raise BenchError('Workspace state did not acknowledge native switching')
        if kind=='workspace-out':
            marker(rows[1],SCENE)
            if any(m['frame']==SCENE for m in rows[-1]['markers']):raise BenchError('Workspace switch did not hide scene')
        else:marker(rows[-1],SCENE)
    else:raise BenchError('Unknown extended desktop action')
    return result


class Controller:
    def __init__(self,host,record,directory,target,layout,profile,health,progress,result):
        self.host,self.record,self.directory,self.target,self.layout,self.profile=host,record,directory,target,layout,profile
        self.health,self.progress,self.result=health,progress,result
        self.processes={};self.lines={};self.streams={};self.keyboard=None;self.input_lines=None;self.sysfs=None
        self.errors=(directory/f'{target}-desktop-v2.stderr').open('wb')
    def collect(self):
        for role,lines in self.lines.items():
            while lines.pending or select.select([lines.process.stdout],[],[],0)[0]:
                if lines.process.poll() is not None:raise BenchError('Desktop scene exited during observation')
                row=lines.next(time.monotonic()+2)
                if 'monotonic_ns' in row:row['monotonic_ns']=int(row['monotonic_ns'])
                if len(lines.records)>4096:raise BenchError('Desktop source stream exceeds limit')
            self.streams[role]=lines.records
    def start(self,peer=False):
        role='peer' if peer else 'scene';unit='observation' if peer else 'scene'
        if role in self.processes:self.stop(role)
        process=self.host.spawn(user_command(self.record,self.layout,unit,[str(self.layout.helper.parent/'vela-bench-scene'),'--peer' if peer else '--observe'],runtime_seconds=180),self.errors)
        self.processes[role]=process;self.lines[role]=Lines(process)
        ready=self.lines[role].next(time.monotonic()+10)
        if ready!={'event':'scene-ready','protocol':'wayland','width':640,'height':360}:raise BenchError('Native v2 scene readiness failed')
        time.sleep(.6);self.collect()
    def stop(self,role):
        try:self.collect()
        finally:
            try:stop_instrument(self.host,self.record,self.layout,'observation' if role=='peer' else 'scene')
            finally:stop_process(self.processes.pop(role,None));self.lines.pop(role,None)
    def input(self):
        self.keyboard=self.host.spawn([str(self.layout.helper.parent/'vela-bench-input'),'--desktop'],self.errors)
        self.input_lines=Lines(self.keyboard);row=self.input_lines.next(time.monotonic()+5);self.sysfs=row.get('sysfs','')
        if row.get('event')!='uinput-ready' or row.get('method')!=SPEC['input'] or row.get('clock')!='CLOCK_MONOTONIC' or not re.fullmatch(r'/sys/devices/virtual/input/input[0-9]+',self.sysfs):raise BenchError('Invalid desktop input device')
        time.sleep(.5)
    def pulse(self,command):
        self.health();self.keyboard.stdin.write((command+'\n').encode());self.keyboard.stdin.flush()
        receipt=self.input_lines.next(time.monotonic()+5);receipt['command']=command;return receipt
    def workspace(self,operation='state'):
        return decode_json(self.host.run(user_command(self.record,self.layout,'display',
            [str(self.layout.helper.parent/'vela-bench-desktop'),'--workspace',self.target,operation]),timeout=10).stdout)
    def capture(self,name,kind,callback):
        if self.progress:self.progress(name)
        command=user_command(self.record,self.layout,'capture',['/usr/bin/python3','-I',str(self.layout.helper),'_capture',self.target,self.profile['primary']],runtime_seconds=60)
        process=None;rows=[];injections=[];action={'id':name,'kind':kind,'captures':rows,'injections':injections}
        self.result['actions'].append(action)
        with (self.directory/f'{self.target}-desktop-{name}.stderr').open('wb') as errors:
            try:
                process=self.host.spawn(command,errors);lines=Lines(process,max_record_bytes=16*1024*1024)
                for index in range(12):
                    self.health();row=lines.next(time.monotonic()+30)
                    validate_frames([row],self.target,self.profile,self.directory,name,partial=True,offset=index,previous_end=rows[-1]['end_monotonic_ns'] if rows else -1)
                    if not isinstance(row.get('rectangles'),dict):raise BenchError('Capture lacks declared native scene geometry')
                    rows.append(row);self.collect()
                    if index>=1:injections.extend(callback(index,rows) or [])
                if process.wait(timeout=5):raise BenchError('Desktop capture process failed')
                self.collect()
                if kind.startswith('workspace-'):action['workspace_after']=self.workspace()
                action['source_streams']={role:list(rows) for role,rows in self.streams.items()}
                action['metrics']=metrics(action,self.directory,self.target,self.profile,action['source_streams'],self.record['uid'],check_pixels=False)
                return action
            except BaseException as exc:
                action['source_streams']={role:list(rows) for role,rows in self.streams.items()};action['error']=str(exc);raise
            finally:stop_process(process);stop_instrument(self.host,self.record,self.layout,'capture')
    def one(self,commands):return lambda i,rows:[self.pulse(c) for c in commands] if i==1 else []
    def simple(self,name,kind,commands):return self.capture(name,kind,self.one(commands))
    def pointer(self,name,kind):
        path=None
        def move(i,rows):
            nonlocal path
            if i==1:
                rect=rows[1]['rectangles'].get(str(SCENE))
                if not rect:raise BenchError('No identified scene before system move/resize')
                path=pointer_path(rect,self.profile,kind)
                return [self.pulse(path[0]),self.pulse('down')]
            if i<11:return [self.pulse(path[i-1])]
            return [self.pulse('up')]
        return self.capture(name,kind,move)
    def point(self,x,y):return self.pulse(f"point {round(x*65535/(self.profile['mode']['width']-1))} {round(y*65535/(self.profile['mode']['height']-1))}")
    def close(self):
        errors=[]
        try:
            try:self.collect()
            except (BenchError,OSError,ValueError) as exc:errors.append(str(exc))
            self.result['source_streams']=self.streams
            for role in list(self.processes):
                try:self.stop(role)
                except (BenchError,OSError,ValueError) as exc:errors.append(str(exc))
        finally:
            stop_process(self.keyboard)
            if self.sysfs:
                deadline=time.monotonic()+2
                while Path(self.sysfs).exists() and time.monotonic()<deadline:time.sleep(.05)
                self.result['uinput_cleanup']='passed' if not Path(self.sysfs).exists() else 'failed'
            self.errors.close()
        if errors:raise BenchError('; '.join(errors))


def ui_actions(c,prefix=''):
    opened=c.simple(prefix+'launcher','launcher-open',['launcher'])
    closed=c.simple(prefix+'launcher-close','launcher-close',['escape'])
    if changed(opened['captures'][1]['tiles'],closed['captures'][-1]['tiles'])>8:raise BenchError('Launcher did not return to original closed state')
    c.pointer(prefix+'drag','drag')


def loaded(c,mode):
    label=mode+'-loaded-';prefix=c.directory/f'{c.target}-desktop-load-{mode}'
    cmd=([str(c.layout.helper.parent/'vela-bench-cpuload'),'--json','--until-stdin-eof'] if mode=='cpu' else
         [str(c.layout.helper.parent/'vela-slowgpu'),'--contention','--json','--until-stdin-eof'] if mode=='gpu' else
         [str(c.layout.helper.parent/'vela-slowgpu'),'150','--json','--until-stdin-eof','--desktop-load',*(['--implicit'] if mode=='implicit' else [])])
    process=None;lines=None;start=len(c.result['actions'])
    with prefix.with_suffix('.stderr').open('wb') as errors,ThreadPoolExecutor(max_workers=1) as pool:
        try:
            process=c.host.spawn(user_command(c.record,c.layout,'producer',cmd,runtime_seconds=120),errors);lines=Lines(process)
            deadline=time.monotonic()+110
            if mode in ('cpu','gpu'):
                lines.next(deadline)
                while not contention.ready(lines.records,mode,c.record['uid']):lines.next(deadline)
            else:
                while lines.next(deadline).get('event')!='producer-presented':pass
            def drain():
                while lines.next(deadline).get('event')!=('producer-summary' if mode in ('explicit','implicit') else mode+'-load-summary'):pass
            future=pool.submit(drain)
            # Keep the identified scene above the stable-colour producer; its
            # exposed border still submits/presents, without animating the UI detector.
            c.start()
            try:ui_actions(c,label)
            finally:process.stdin.close();process.stdin=None
            future.result(timeout=8)
            if process.wait(timeout=5):raise BenchError('Observed load helper failed')
            actions=c.result['actions'][start:]
            window=[actions[0]['captures'][0]['begin_monotonic_ns'],actions[-1]['captures'][-1]['end_monotonic_ns']]
            metric=(producer_metrics(lines.records,mode,window) if mode in ('explicit','implicit') else contention.metrics(lines.records,mode,window,c.record['uid']))
            c.result['loads'][mode]={'window_monotonic_ns':window,'metrics':metric}
        finally:
            stop_instrument(c.host,c.record,c.layout,'producer');stop_process(process)
            if lines:prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in lines.records))


def run_observation(host,record,directory,target,layout,profile,phase,health,progress=None):
    result={'schema':1,'method':VERSION,'spec':SPEC,'phase':phase,'status':'failed','failures':[],
            'actions':[],'loads':{},'source_streams':{},'uid':record['uid'],'uinput_cleanup':'failed','family_complete':False}
    c=None
    try:
        if phase not in PHASES:raise BenchError('Unknown P3 phase')
        if phase in ('responsiveness','motion'):
            base=legacy.run_observation(host,record,directory,target,layout,profile,phase,health,progress)
            atomic_json(directory/f'{target}-desktop-base.json',base)
            result['base']=base
            if base['status']!='passed':raise BenchError('P3 base actions failed: '+'; '.join(base['failures']))
        c=Controller(host,record,directory,target,layout,profile,health,progress,result);c.input();c.start()
        if phase=='responsiveness':
            c.start(peer=True)
            c.capture('alt-tab','alt-tab',lambda i,rows:[c.pulse('alt-tab-begin')] if i==1 else [c.pulse('alt-tab-end')] if i==5 else [])
            c.stop('peer');time.sleep(.4)
            opened=c.simple('window-menu','menu-open',['window-menu' if target=='vela' else 'window-menu-plasma'])
            closed=c.simple('window-menu-close','menu-close',['escape'])
            if changed(opened['captures'][1]['tiles'],closed['captures'][-1]['tiles'])>8:raise BenchError('Window menu did not return to baseline')
        elif phase=='motion':
            c.pointer('window-resize','resize')
            for kind in ('snap-left','snap-right','maximize','restore'):
                c.start();commands=scaling_actions(target,{'maximize':'maximized','restore':'restored'}.get(kind,kind))
                if kind=='restore':c.capture(kind,kind,lambda i,rows:[c.pulse(commands[0])] if i==1 else [c.pulse(commands[1])] if i==6 else [])
                else:c.simple(kind,kind,commands)
            initial=c.workspace();result['workspace_initial']=initial
            if initial['current_index']!=0:raise BenchError('Fresh desktop did not start on workspace one')
            prepared=c.workspace('create');result['workspace_prepared']=prepared
            try:
                c.start()
                for kind in ('workspace-out','workspace-back'):
                    command=('workspace-next' if kind=='workspace-out' else 'workspace-previous') if target=='vela' else ('workspace-second-plasma' if kind=='workspace-out' else 'workspace-first-plasma')
                    c.simple(kind,kind,[command])
            finally:
                if c.workspace()['current_index']!=0:
                    c.pulse('workspace-previous' if target=='vela' else 'workspace-first-plasma');time.sleep(.3)
                result['workspace_restored']=c.workspace('remove') if initial['count']==1 else c.workspace()
        else:
            for mode in SPEC['loads']:
                c.start();ui_actions(c,mode+'-control-')
                c.start();loaded(c,mode)
        if phase=='slow-apps':result['paired_effects']=paired_effects(result['actions'])
        result['status']='passed'
    except (BenchError,OSError,ValueError,KeyError,TypeError,TimeoutError,subprocess.TimeoutExpired) as exc:result['failures'].append(str(exc))
    finally:
        if c:
            try:c.close()
            except (BenchError,OSError,ValueError) as exc:result['failures'].append('Desktop cleanup: '+str(exc));result['status']='failed'
        if result['uinput_cleanup']!='passed':result['status']='failed';result['failures'].append('Desktop input cleanup not verified')
        atomic_json(directory/f'{target}-desktop.json',result)
    return result


def audit_observation(directory,target,profile,phase):
    directory=Path(directory);saved=read_json(directory/f'{target}-desktop.json')
    if saved.get('method')==legacy.VERSION:return legacy.audit_observation(directory,target,profile,phase)
    if (saved.get('method')!=VERSION or saved.get('spec')!=SPEC or saved.get('status')!='passed' or saved.get('failures')!=[]
            or saved.get('phase')!=phase or phase not in PHASES or saved.get('uinput_cleanup')!='passed' or saved.get('family_complete') is not False):
        raise BenchError('P3 observation incomplete or fixed specification changed')
    uid=saved['uid']
    if type(uid) is not int or uid<=0:raise BenchError('Invalid desktop benchmark UID')
    expected=([('alt-tab','alt-tab'),('window-menu','menu-open'),('window-menu-close','menu-close')] if phase=='responsiveness' else
              [('window-resize','resize'),*[(s,s) for s in ('snap-left','snap-right','maximize','restore','workspace-out','workspace-back')]] if phase=='motion' else
              [(mode+'-'+condition+'-'+name,kind) for mode in SPEC['loads'] for condition in ('control','loaded') for name,kind in (('launcher','launcher-open'),('launcher-close','launcher-close'),('drag','drag'))])
    if [(a.get('id'),a.get('kind')) for a in saved['actions']]!=expected:raise BenchError('P3 extended action matrix incomplete')
    # Decode byte-identical captures once per audit, after checking each file's
    # digest. The cache stores only derived detector data, never skips raw hashes.
    pixel_cache={}
    def read_pixels(path):
        digest=hashlib.sha256(path.read_bytes()).digest()
        if digest not in pixel_cache:pixel_cache[digest]=evidence(path)
        return pixel_cache[digest]
    previous=-1
    for action in saved['actions']:
        if action['captures'][0]['begin_monotonic_ns']<previous:raise BenchError('P3 action order changed')
        if action.get('metrics')!=metrics(action,directory,target,profile,action['source_streams'],uid,pixel_reader=read_pixels):raise BenchError('P3 metrics differ from raw pixels/sources')
        previous=action['captures'][-1]['end_monotonic_ns']
    if phase in ('responsiveness','motion'):
        # Audit the original method from a separate immutable source file.
        basefile=directory/f'{target}-desktop-base.json'
        base=read_json(basefile)
        if saved.get('base')!=base or base.get('method')!=legacy.VERSION:raise BenchError('P3 base evidence changed')
        legacy.audit_observation(directory,target,profile,phase,saved=base)
        for action in base['actions']:
            for row in action['captures']:
                pixels=read_pixels(directory/row['image_file'])
                if any(row.get(k)!=pixels[k] for k in ('width','height','tiles','markers')):raise BenchError('P3 base metadata differs from original PNG')
        if base['actions'][-1]['captures'][-1]['end_monotonic_ns']>saved['actions'][0]['captures'][0]['begin_monotonic_ns']:raise BenchError('P3 base/extension clocks overlap')
    if phase=='motion':
        before,prepared,after=(saved.get(k,{}) for k in ('workspace_initial','workspace_prepared','workspace_restored'))
        for value in (before,prepared,after):workspace_audit(value,target,uid)
        added=before['count']==1
        if not int(before['begin_monotonic_ns'])<=int(prepared['begin_monotonic_ns'])<=saved['actions'][-2]['captures'][0]['begin_monotonic_ns']<=int(after['begin_monotonic_ns']):raise BenchError('Workspace fixture clocks changed')
        if (before['current_index']!=0 or prepared['count']!=max(2,before['count']) or prepared['current_index']!=0
                or any(before[k]!=after[k] for k in ('count','names','ids','current_index'))
                or (added and (prepared['names']!=before['names']+['Vela benchmark fixture'] or prepared['ids'][:1]!=before['ids']))
                or (not added and any(before[k]!=prepared[k] for k in ('count','names','ids','current_index')))):
            raise BenchError('Workspace fixture restoration failed')
    pairs=[(saved['actions'][1],saved['actions'][2])] if phase=='responsiveness' else [(saved['actions'][i],saved['actions'][i+1]) for i in range(0,24,3)] if phase=='slow-apps' else []
    for opened,closed in pairs:
        if changed(opened['captures'][1]['tiles'],closed['captures'][-1]['tiles'])>8:raise BenchError('Native menu/launcher did not return to baseline')
    if phase=='slow-apps':
        if saved.get('paired_effects')!=paired_effects(saved['actions']):raise BenchError('Paired observed load effects changed')
        if set(saved['loads'])!=set(SPEC['loads']):raise BenchError('Observed load matrix incomplete')
        gpu_identity=None
        for mode in SPEC['loads']:
            actions=[a for a in saved['actions'] if a['id'].startswith(mode+'-loaded-')]
            window=[actions[0]['captures'][0]['begin_monotonic_ns'],actions[-1]['captures'][-1]['end_monotonic_ns']]
            rows=[decode_json(line) for line in (directory/f'{target}-desktop-load-{mode}.jsonl').read_text().splitlines()]
            if mode in ('explicit','implicit') and (not rows or rows[0].get('uid')!=uid or rows[0].get('desktop_static') is not True):raise BenchError('Observed producer UID differs')
            value=producer_metrics(rows,mode,window) if mode in ('explicit','implicit') else contention.metrics(rows,mode,window,uid)
            if saved['loads'][mode]!={'window_monotonic_ns':window,'metrics':value}:raise BenchError('Observed load differs from source evidence')
            if mode!='cpu':
                identity=value['device'] if mode in ('explicit','implicit') else value['source']
                keys=('vendor_id','device_id','drm_major','drm_minor')
                if gpu_identity and any(identity[k]!=gpu_identity[k] for k in keys):raise BenchError('Observed load GPU identity changed')
                gpu_identity=identity
    return saved


def workspace_audit(value,target,uid):
    if (value.get('schema')!=1 or value.get('event')!='workspace-state' or value.get('target')!=target or value.get('uid')!=uid
            or type(value.get('count')) is not int or not 1<=value['count']<=32
            or type(value.get('current_index')) is not int or not 0<=value['current_index']<value['count']
            or any(not isinstance(value.get(k),list) or len(value[k])!=value['count'] or any(not isinstance(v,str) for v in value[k]) for k in ('ids','names'))
            or len(set(value['ids']))!=value['count'] or not str(value.get('begin_monotonic_ns','')).isdigit()):raise BenchError('Invalid workspace source identity/topology')


def paired_effects(actions):
    pairs={}
    for mode in SPEC['loads']:
        result={}
        for name in ('launcher','launcher-close','drag'):
            pair=[next(a for a in actions if a['id']==mode+'-'+condition+'-'+name)['metrics'] for condition in ('control','loaded')]
            keys=('first_observed_response_upper_ms','first_observed_stable_state_upper_ms','maximum_capture_gap_ms')
            if name=='drag':keys+=('longest_observed_unchanged_interval_upper_ms',)
            result[name]={k:{'control':pair[0][k],'loaded':pair[1][k],
                            'loaded_minus_control':pair[1][k]-pair[0][k] if all(p[k] is not None for p in pair) else None} for k in keys}
        pairs[mode]=result
    return pairs


def pointer_path(rect,profile,kind):
    x,y,w,h=rect;ow,oh=profile['mode']['width'],profile['mode']['height']
    if kind=='resize':
        dx=dy=min(120,ow-x-w-4,oh-y-h-4)
        if dx<80:raise BenchError('Insufficient physical space for resize workload')
        px,py=x+w-16,y+h-16
    else:
        room,dx,dy=max(((x,-1,0),(ow-x-w,1,0),(y,0,-1),(oh-y-h,0,1)))
        distance=min(162,room-2)
        if distance<80:raise BenchError('Insufficient physical space for drag workload')
        dx*=distance;dy*=distance;px,py=x+16+DRAG_GRIP[0],y+16+DRAG_GRIP[1]
    return [f'point {round((px+dx*i/9)*65535/(ow-1))} {round((py+dy*i/9)*65535/(oh-1))}' for i in range(10)]
