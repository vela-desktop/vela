# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed capture-free window actions, verified through native client records."""
import json
import re
import select
import time

from . import BenchError
from .archive import atomic_json, decode_json
from .calibration import Lines

METHOD='native-window-maximize-restore-v1'
SPEC={'method':METHOD,'cycles':12,'actions_per_cycle':['maximize','restore'],
      'minimum_command_interval_ns':750_000_000,'logical_size':[401,301],
      'capture_enabled':False,'state_evidence':'native-configure-and-buffer-submission',
      'scene_teardown':'after-final-resource-sample',
      'latency_measurement':False,'input_helper_cost':'root-emitter-outside-UID; not-measured'}


def command(target,operation):
    if target not in ('vela','plasma') or operation not in ('maximize','restore'):
        raise BenchError('Unknown fixed window interaction')
    return operation if target=='vela' else 'maximize-toggle'


def native_pattern(row,profile):
    if (not isinstance(profile,dict) or not isinstance(profile.get('mode'),dict)
            or any(type(profile['mode'].get(k)) is not int or profile['mode'][k]<=0 for k in ('width','height'))):
        raise BenchError('Invalid interaction output profile')
    keys=('sequence','monotonic_ns','logical_width','logical_height','buffer_width','buffer_height','scale120')
    if (not isinstance(row,dict) or any(type(row.get(k)) is not int or row[k]<=0 for k in keys)
            or row.get('event')!='pattern' or row.get('method')!='coordinate-pattern-v1'
            or row.get('protocol')!='wayland' or row.get('viewport') is not True
            or type(row.get('maximized')) is not bool or row['scale120']!=120
            or row['logical_width']!=row['buffer_width'] or row['logical_height']!=row['buffer_height']
            or row['buffer_width']>profile['mode']['width']+2 or row['buffer_height']>profile['mode']['height']+2):
        raise BenchError('Invalid native interaction geometry, backend or scale')


def matches(row,operation,profile):
    native_pattern(row,profile)
    if operation=='restore':
        return row['maximized'] is False and [row['logical_width'],row['logical_height']]==SPEC['logical_size']
    return (row['maximized'] is True and row['buffer_width']>=.9*profile['mode']['width']
            and row['buffer_height']>=.6*profile['mode']['height'])


def settled(lines,operation,profile,after):
    deadline=time.monotonic()+5
    matched=None
    while True:
        if time.monotonic()>=deadline or len(lines.records)>=1024:
            raise BenchError('Window interaction state did not settle: '+operation)
        if matched is not None and not lines.pending and not select.select([lines.process.stdout],[],[],.1)[0]:
            return matched
        row=lines.next(deadline)
        matched=row if matches(row,operation,profile) and row['monotonic_ns']>=after else None


def run(scene,keyboard,target,profile,ready,keyboard_lines,failure_path=None):
    lines=Lines(scene)
    actions=[]
    try:
        initial=settled(lines,'restore',profile,0)
        for _ in range(SPEC['cycles']):
            for operation in SPEC['actions_per_cycle']:
                began=time.monotonic_ns()
                key=command(target,operation)
                keyboard.stdin.write((key+'\n').encode());keyboard.stdin.flush()
                injection=keyboard_lines.next(time.monotonic()+5)
                injection['command']=key
                if (injection.get('event')!='injected' or type(injection.get('before_monotonic_ns')) is not int
                        or injection['before_monotonic_ns']<began):
                    raise BenchError('Invalid window interaction injection')
                state=settled(lines,operation,profile,injection['before_monotonic_ns'])
                actions.append({'operation':operation,'injection':injection,'pattern':state,
                                'verified_monotonic_ns':time.monotonic_ns()})
                remaining=(injection['before_monotonic_ns']+SPEC['minimum_command_interval_ns']-time.monotonic_ns())/1e9
                if remaining>0:time.sleep(remaining)
        row={'event':'window-interaction-complete','method':METHOD,'target':target,'spec':SPEC,
             'begin_monotonic_ns':lines.records[0]['monotonic_ns'],'end_monotonic_ns':time.monotonic_ns(),
             'keyboard_ready':ready,'initial':initial,'pattern_events':lines.records,'actions':actions}
        audit(json.dumps(row),target,profile)
        return (json.dumps(row)+'\n').encode(),None
    except (BenchError,OSError,ValueError,KeyError,TypeError) as exc:
        if failure_path is not None:
            atomic_json(failure_path,{'status':'failed','method':METHOD,'spec':SPEC,'target':target,'error':str(exc),
                'keyboard_ready':ready,'pattern_events':lines.records,'completed_actions':actions})
        raise


def audit(raw,target,profile):
    row=decode_json(raw)
    if (not isinstance(row,dict) or row.get('event')!='window-interaction-complete' or row.get('method')!=METHOD
            or row.get('target')!=target or row.get('spec')!=SPEC
            or type(row.get('begin_monotonic_ns')) is not int or type(row.get('end_monotonic_ns')) is not int
            or not 0<row['begin_monotonic_ns']<row['end_monotonic_ns']):
        raise BenchError('Window interaction identity or completion changed')
    ready=row.get('keyboard_ready',{})
    if (not isinstance(ready,dict) or ready.get('event')!='uinput-ready' or ready.get('method')!='linux-uinput-fixed-desktop-v1'
            or ready.get('clock')!='CLOCK_MONOTONIC' or not isinstance(ready.get('sysfs'),str)
            or not re.fullmatch(r'/sys/devices/virtual/input/input[0-9]+',ready['sysfs'])):
        raise BenchError('Window interaction input instrument changed')
    events=row.get('pattern_events')
    if not isinstance(events,list) or not 1<=len(events)<=1024:
        raise BenchError('Missing window interaction source records')
    by_sequence={};previous=None
    for event in events:
        native_pattern(event,profile)
        if ((previous is None and event['sequence']!=1)
                or (previous and (event['sequence']!=previous['sequence']+1 or event['monotonic_ns']<previous['monotonic_ns']))
                or not row['begin_monotonic_ns']<=event['monotonic_ns']<=row['end_monotonic_ns']):
            raise BenchError('Window interaction native stream is reordered or truncated')
        by_sequence[event['sequence']]=event;previous=event
    if row['begin_monotonic_ns']!=events[0]['monotonic_ns']:
        raise BenchError('Window interaction start differs from its native source')
    initial=row.get('initial',{})
    if not isinstance(initial,dict) or by_sequence.get(initial.get('sequence'))!=initial or not matches(initial,'restore',profile):
        raise BenchError('Window interaction did not establish its initial normal state')
    actions=row.get('actions')
    if not isinstance(actions,list) or len(actions)!=2*SPEC['cycles']:
        raise BenchError('Window interaction did not complete its fixed action count')
    last_sequence=initial['sequence'];last_before=None;last_verified=initial['monotonic_ns']
    for index,action in enumerate(actions,1):
        if not isinstance(action,dict) or not isinstance(action.get('injection'),dict) or not isinstance(action.get('pattern'),dict):
            raise BenchError('Invalid window interaction action record')
        operation=SPEC['actions_per_cycle'][(index-1)%2]
        injection=action.get('injection',{});pattern=action.get('pattern',{})
        before,after=injection.get('before_monotonic_ns'),injection.get('after_monotonic_ns')
        verified=action.get('verified_monotonic_ns')
        if (action.get('operation')!=operation or injection.get('command')!=command(target,operation)
                or injection.get('event')!='injected' or type(injection.get('action_id')) is not int or injection['action_id']!=index
                or any(type(v) is not int for v in (before,after,verified))
                or not last_verified<=before<=after<=verified<=row['end_monotonic_ns']
                or (last_before is not None and before-last_before<SPEC['minimum_command_interval_ns'])
                or type(pattern.get('sequence')) is not int or pattern['sequence']<=last_sequence
                or by_sequence.get(pattern['sequence'])!=pattern
                or not before<=pattern['monotonic_ns']<=verified or not matches(pattern,operation,profile)):
            raise BenchError('Window interaction action, receipt or native state was not verified')
        last_sequence=pattern['sequence'];last_before=before;last_verified=verified
    return {'method':METHOD,'cycles':SPEC['cycles'],'verified_actions':len(actions),
            'state_evidence':SPEC['state_evidence'],'capture_enabled':False,'latency_measurement':False,
            'minimum_command_interval_ns':SPEC['minimum_command_interval_ns'],
            'input_helper_cost':SPEC['input_helper_cost']}
