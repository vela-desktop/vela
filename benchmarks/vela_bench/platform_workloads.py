# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""P4 native pattern and UID resource workloads. Application corpus is partial."""
import base64
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import select
import subprocess
import time

from . import BenchError
from .archive import atomic_json, decode_json, read_json
from .calibration import Lines, stop_process
from .conditions import compare_outputs, display_command, normalized_outputs
from .diagnostics import user_command, stop_instrument, parse_probe
from .pixels import pattern_metrics
from . import resources

VERSION = 'p4-pattern-resources-v1'
PHASES = ('scaling','overhead')
SCALES = (1.0,1.25,1.5,1.75,2.0)
STATES = ('opened','snap-left','snap-right','maximized','restored')
SPEC = {'version':VERSION,'scales':list(SCALES),'states':list(STATES),'pattern_logical_size':[401,301],
        'capture_backend_fallback':False,'pixel_oracle':'coordinate-pattern-v1',
        'maximum_pattern_edge_overhang_pixels':2,'excluded_corner_radius_logical_pixels':8,
        'pattern_pointer_parking':'bottom-right-output-corner',
        'resource_idle_seconds':30,'resource_settle_seconds':10,'resource_sample_interval_seconds':1,
        'active_animation_frames':1800,'hash_chunks':resources.HASH_CHUNKS,
        'capture_during_resource_measurement':False,'energy_required':False,
        'scope':'native-wayland-pattern-and-reserved-uid-resources; application-corpus-incomplete'}


def query_outputs(host,record,layout,target):
    raw=host.run(user_command(record,layout,'display',display_command(layout,target)),timeout=30).stdout
    return normalized_outputs(raw,target)


def apply_outputs(host,record,layout,target,outputs):
    host.run(user_command(record,layout,'display',display_command(layout,target,outputs)),timeout=30)
    observed=query_outputs(host,record,layout,target)
    compare_outputs(observed,outputs)
    return observed


def scaling_metrics(row,directory,profile,target):
    scale,state=row['scale'],row['state']
    if type(scale) is not float or scale not in SCALES or state not in STATES:
        raise BenchError('Unknown scaling condition')
    name=f'{target}-scale-{round(scale*100)}-{state}.png'
    path=directory/name
    if row.get('image_file') != name or hashlib.sha256(path.read_bytes()).hexdigest() != row.get('image_sha256'):
        raise BenchError('Scaling capture image changed')
    backend='ext-image-copy-capture-v1' if target=='vela' else 'kwin-screenshot2-v1'
    capture=row['capture']
    if (capture.get('backend')!=backend or capture.get('event')!='capture' or capture.get('index')!=0
            or [capture.get('width'),capture.get('height')]!=[profile['mode']['width'],profile['mode']['height']]
            or any(type(capture.get(k)) is not int for k in ('begin_monotonic_ns','end_monotonic_ns'))
            or not 0 <= capture['begin_monotonic_ns'] <= capture['end_monotonic_ns']):
        raise BenchError('Scaling capture backend or clock changed')
    outputs=row['outputs']
    wanted=copy.deepcopy(profile['outputs'])
    for output in wanted:
        if output['enabled']: output['scale']=scale
    compare_outputs(outputs,wanted)
    meta=row['pattern']
    if type(meta.get('monotonic_ns')) is not int or not 0 <= meta['monotonic_ns'] <= capture['begin_monotonic_ns']:
        raise BenchError('Pattern state was not established before capture')
    logical=[meta.get('logical_width'),meta.get('logical_height')]
    if state in ('opened','restored') and logical != [401,301]:
        raise BenchError('Normal/restored pattern did not retain its original logical size')
    if state=='maximized' and (meta.get('maximized') is not True
            or meta['buffer_width']<.9*profile['mode']['width'] or meta['buffer_height']<.6*profile['mode']['height']):
        raise BenchError('Pattern did not acknowledge maximization')
    if state.startswith('snap-') and (meta.get('maximized') is not False or
            not .35 <= logical[0]*scale/profile['mode']['width'] <= .65):
        raise BenchError('Pattern did not establish half-output geometry')
    injections=row['injections']
    commands={'opened':[],'snap-left':['snap-left'],'snap-right':['snap-right'],
              'maximized':['maximize'],'restored':['maximize','restore']}[state]
    if ([r.get('command') for r in injections]!=commands or any(r.get('event')!='injected'
            or any(type(r.get(k)) is not int for k in ('before_monotonic_ns','after_monotonic_ns'))
            or not 0 <= r['before_monotonic_ns'] <= r['after_monotonic_ns'] <= capture['begin_monotonic_ns']
            or meta['monotonic_ns'] < r['before_monotonic_ns']
            for r in injections)):
        raise BenchError('Scaling actions were not verified before the captured configure')
    result=pattern_metrics(path,meta,[profile['mode']['width'],profile['mode']['height']],scale)
    # Pixel differences are the measured outcome, not an instrument failure.
    # Invalid identity/scale/capture/state still rejects the measurement.
    if state.startswith('snap-'):
        midpoint=result['origin'][0]+result['buffer_size'][0]/2
        if (state=='snap-left' and midpoint >= profile['mode']['width']/2
                or state=='snap-right' and midpoint <= profile['mode']['width']/2):
            raise BenchError('Snapped pattern is on the wrong output side')
    return result


def run_scaling(host,record,directory,target,layout,profile,health,progress,result):
    keyboard=scene=None
    sysfs=None
    result['scale1_restored']=False
    try:
        with (directory/f'{target}-scaling.stderr').open('wb') as errors:
            keyboard=host.spawn([str(layout.helper.parent/'vela-bench-input'),'--desktop'],errors)
            lines=Lines(keyboard)
            ready=lines.next(time.monotonic()+5)
            sysfs=ready.get('sysfs','')
            if (ready.get('event')!='uinput-ready' or ready.get('method')!='linux-uinput-fixed-desktop-v1'
                    or ready.get('clock')!='CLOCK_MONOTONIC' or not re.fullmatch(r'/sys/devices/virtual/input/input[0-9]+',sysfs)):
                raise BenchError('Invalid scaling input device')
            time.sleep(.5)
            for scale in SCALES:
                wanted=copy.deepcopy(profile['outputs'])
                for output in wanted:
                    if output['enabled']: output['scale']=scale
                health()
                observed=apply_outputs(host,record,layout,target,wanted)
                keyboard.stdin.write(b'point 65535 65535\n');keyboard.stdin.flush()
                parked=lines.next(time.monotonic()+5)
                parked['command']='point 65535 65535'
                for state in STATES:
                    health()
                    label=f'{round(scale*100)}-{state}'
                    if progress: progress('pattern/'+label)
                    scene=host.spawn(user_command(record,layout,'scene',
                        [str(layout.helper.parent/'vela-pattern'),'--benchmark','401','301'],runtime_seconds=60),errors)
                    scene_lines=Lines(scene)
                    meta=scene_lines.next(time.monotonic()+10)
                    injections=[]
                    time.sleep(.6)
                    actions={'opened':[],'snap-left':['snap-left'],'snap-right':['snap-right'],
                             'maximized':['maximize'],'restored':['maximize','restore']}[state]
                    for action in actions:
                        health()
                        keyboard.stdin.write((action+'\n').encode()); keyboard.stdin.flush()
                        injection=lines.next(time.monotonic()+5)
                        injection['command']=action; injections.append(injection)
                        time.sleep(.7)
                    # Drain already delivered configure/scale events, retaining
                    # all source records. A read barrier is not a timing metric.
                    drain_deadline=time.monotonic()+2
                    while scene_lines.pending or select.select([scene.stdout],[],[],.15)[0]:
                        if time.monotonic()>drain_deadline or len(scene_lines.records)>=128:
                            raise BenchError('Pattern configure stream did not settle')
                        meta=scene_lines.next(time.monotonic()+5)
                    command=user_command(record,layout,'capture',['/usr/bin/python3','-I',str(layout.helper),
                        '_capture',target,profile['primary'],'--one'],runtime_seconds=25)
                    captured=host.run(command,timeout=30)
                    row=decode_json(captured.stdout)
                    png=base64.b64decode(row.pop('png_base64'),validate=True)
                    if len(png)>12*1024*1024: raise BenchError('Scaling image exceeds evidence limit')
                    for k in ('begin_monotonic_ns','end_monotonic_ns'): row[k]=int(row[k])
                    name=f'{target}-scale-{label}.png'
                    (directory/name).write_bytes(png)
                    observation={'scale':scale,'state':state,'outputs':observed,'pattern':meta,
                        'pattern_events':scene_lines.records,'injections':injections,'pointer_parking':parked,'capture':row,
                        'image_file':name,'image_sha256':hashlib.sha256(png).hexdigest()}
                    result['scenarios'][label]=observation
                    try: observation['metrics']=scaling_metrics(observation,directory,profile,target)
                    except (BenchError,ValueError,KeyError) as exc:
                        observation['error']=str(exc)
                        raise BenchError(f'Scaling {label}: {exc}') from exc
                    stop_process(scene); scene=None
                    stop_instrument(host,record,layout,'scene')
    finally:
        stop_process(scene)
        stop_instrument(host,record,layout,'scene')
        stop_process(keyboard)
        if sysfs and Path(sysfs).exists():
            deadline=time.monotonic()+2
            while Path(sysfs).exists() and time.monotonic()<deadline: time.sleep(.05)
            if Path(sysfs).exists(): raise BenchError('Scaling input device was not removed')
        apply_outputs(host,record,layout,target,profile['outputs'])
        result['scale1_restored']=True


def resource_condition(host,record,directory,target,layout,label,health,progress):
    if progress: progress('resources/'+label)
    rows=[]
    cpu_before=time.monotonic_ns()
    process=None
    with (directory/f'{target}-resources-{label}.stderr').open('wb') as errors, ThreadPoolExecutor(max_workers=1) as pool:
        try:
            health(); rows.append(resources.snapshot(record['uid']))
            if label=='idle': future=None
            else:
                command=([str(layout.probe),'--frames',str(SPEC['active_animation_frames']),'--timeout','55'] if label=='active-animation'
                         else ['/usr/bin/python3','-I',str(layout.helper),'_resource-workload'])
                process=host.spawn(user_command(record,layout,'measurement',command,runtime_seconds=120),errors)
                future=pool.submit(process.communicate)
            end=time.monotonic()+SPEC['resource_idle_seconds']
            deadline=time.monotonic()+125
            while (time.monotonic()<end if future is None else not future.done()):
                if time.monotonic()>deadline: raise BenchError('Resource workload deadline')
                time.sleep(1)
                health(); rows.append(resources.snapshot(record['uid']))
            if future is not None:
                output,_=future.result(timeout=2)
                if process.returncode or len(output)>8*1024*1024: raise BenchError('Resource workload failed or output limit exceeded')
                raw=output.decode()
                (directory/f'{target}-resources-{label}.work.jsonl').write_text(raw)
            else: raw=''
            health(); rows.append(resources.snapshot(record['uid']))
        finally:
            stop_process(process)
            stop_instrument(host,record,layout,'measurement')
            atomic_json(directory/f'{target}-resources-{label}.json',{'uid':record['uid'],'samples':rows,'controller_begin_monotonic_ns':cpu_before})
    return resource_metrics(rows,record['uid'],label,raw)


def resource_metrics(rows,uid,label,raw):
    metrics=resources.metrics(rows,uid)
    if label=='idle':
        if raw or metrics['elapsed_seconds']<SPEC['resource_idle_seconds']: raise BenchError('Resource idle window incomplete')
    elif label=='active-animation':
        work=parse_probe(raw,frames=SPEC['active_animation_frames'],delay=0)
        metrics['workload']={'method':'native-client-fixed-animation','submitted':work['frames'],'presented':work['presented'],
                             'discarded':work['discarded']}
    elif label=='local-hash':
        work=decode_json(raw)
        if (work.get('event')!='local-work-complete' or work.get('method')!='fixed-sha256-corpus-v1'
                or work.get('chunks')!=resources.HASH_CHUNKS or work.get('bytes')!=len(resources.HASH_DATA)*resources.HASH_CHUNKS
                or work.get('digest')!=hashlib.sha256(resources.HASH_DATA).hexdigest()):
            raise BenchError('Local resource workload did not complete its fixed work')
        metrics['workload']=work
    else: raise BenchError('Unknown resource condition')
    if label!='idle':
        stamps=[decode_json(line) for line in raw.splitlines()]
        begin=(stamps[0].get('begin_monotonic_ns') if label=='local-hash' else
               next(r['monotonic_ns'] for r in stamps if r.get('event')=='commit'))
        end=(stamps[0].get('end_monotonic_ns') if label=='local-hash' else stamps[-1].get('monotonic_ns',0))
        # Presentation summaries do not carry a monotonic stamp; their last
        # feedback does. Keep this bound independent of process exit timing.
        if label=='active-animation': end=max(r.get('received_monotonic_ns',0) for r in stamps)
        if (type(begin) is not int or type(end) is not int or not rows[0]['cpu_end_monotonic_ns']<=begin<=end<=rows[-1]['cpu_begin_monotonic_ns']):
            raise BenchError('Resource samples do not cover the completed workload')
    return metrics


def run_workload(host,record,directory,target,layout,profile,phase,health,progress=None):
    result={'schema':1,'method':VERSION,'spec':SPEC,'phase':phase,'status':'failed','scenarios':{},'failures':[],'family_complete':False}
    try:
        if phase=='scaling': run_scaling(host,record,directory,target,layout,profile,health,progress,result)
        elif phase=='overhead':
            health()
            result['settling_begin_monotonic_ns']=time.monotonic_ns()
            time.sleep(SPEC['resource_settle_seconds'])
            health()
            result['settling_end_monotonic_ns']=time.monotonic_ns()
            for label in ('idle','active-animation','local-hash'):
                result['scenarios'][label]={'resources':resource_condition(host,record,directory,target,layout,label,health,progress)}
        else: raise BenchError('Unknown P4 phase')
        result['status']='passed'
    except (BenchError,OSError,ValueError,KeyError,TypeError,subprocess.TimeoutExpired) as exc:
        result['failures'].append(str(exc))
    finally: atomic_json(directory/f'{target}-measurements.json',result)
    return result


def audit_measurements(directory,target,profile,phase):
    directory=Path(directory)
    saved=read_json(directory/f'{target}-measurements.json')
    if (saved.get('schema')!=1 or saved.get('method')!=VERSION or saved.get('spec')!=SPEC or saved.get('phase')!=phase
            or saved.get('status')!='passed' or saved.get('failures')!=[] or saved.get('family_complete') is not False):
        raise BenchError('P4 workload failed or specification changed')
    if phase=='scaling':
        expected=[f'{round(scale*100)}-{state}' for scale in SCALES for state in STATES]
        if sorted(saved['scenarios'])!=sorted(expected) or saved.get('scale1_restored') is not True:
            raise BenchError('Incomplete scaling matrix or scale restoration')
        previous_end=-1
        for name in expected:
            row=saved['scenarios'][name]
            if row['capture']['begin_monotonic_ns']<previous_end: raise BenchError('Scaling evidence order changed')
            previous_end=row['capture']['end_monotonic_ns']
            if name!=f"{round(row['scale']*100)}-{row['state']}" or row.get('metrics')!=scaling_metrics(row,directory,profile,target):
                raise BenchError('Scaling summary differs from original pixels')
            if not row.get('pattern_events') or row['pattern_events'][-1]!=row['pattern']:
                raise BenchError('Pattern summary differs from native configure evidence')
            parked=row.get('pointer_parking',{})
            if (parked.get('event')!='injected' or parked.get('command')!='point 65535 65535'
                    or any(type(parked.get(k)) is not int for k in ('before_monotonic_ns','after_monotonic_ns'))
                    or not 0<=parked['before_monotonic_ns']<=parked['after_monotonic_ns']<=row['pattern']['monotonic_ns']):
                raise BenchError('Pattern pointer parking was not verified before rendering')
            last=-1
            for index,event in enumerate(row['pattern_events'],1):
                if (event.get('sequence')!=index or type(event.get('monotonic_ns')) is not int
                        or event['monotonic_ns']<last or event.get('method')!='coordinate-pattern-v1'):
                    raise BenchError('Pattern configure stream identity or ordering changed')
                last=event['monotonic_ns']
    elif phase=='overhead':
        if set(saved['scenarios'])!={'idle','active-animation','local-hash'}: raise BenchError('Incomplete resource matrix')
        began,ended=saved.get('settling_begin_monotonic_ns'),saved.get('settling_end_monotonic_ns')
        if type(began) is not int or type(ended) is not int or not 0<=began<=ended-SPEC['resource_settle_seconds']*1_000_000_000:
            raise BenchError('Resource startup settling interval unavailable')
        session=read_json(directory/f'{target}-session.json')
        uid=session.get('session',{}).get('User','')
        if not isinstance(uid,str) or not uid.isdigit() or int(uid)<=0: raise BenchError('Missing reserved session UID')
        previous_end=ended
        for label in ('idle','active-animation','local-hash'):
            sample=saved['scenarios'][label]
            source=read_json(directory/f'{target}-resources-{label}.json')
            if source['uid']!=int(uid) or source['samples'][0]['begin_monotonic_ns']<previous_end:
                raise BenchError('Resource inventory differs from the measured session or settling interval')
            previous_end=source['samples'][-1]['end_monotonic_ns']
            raw='' if label=='idle' else (directory/f'{target}-resources-{label}.work.jsonl').read_text()
            if sample!={'resources':resource_metrics(source['samples'],source['uid'],label,raw)}:
                raise BenchError('Resource summary differs from raw process/cgroup evidence')
    else: raise BenchError('Unknown P4 phase')
    return saved
