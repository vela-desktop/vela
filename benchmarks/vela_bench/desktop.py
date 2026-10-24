# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed P3 actions and coarse output observations; no frame-level motion claims."""
import base64
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import time

from . import BenchError
from .archive import atomic_json, decode_json, read_json
from .calibration import Lines, stop_process
from .diagnostics import user_command, stop_instrument

VERSION = 'p3-desktop-observation-v1'
PHASES = ('responsiveness', 'motion')
SCENE, LAUNCHED = 0x715000, 0x715001
# Relative to the marker's top-left, not the window: the pointer must remain
# below the identity bits even when the capture contains a software cursor.
DRAG_GRIP = (220, 80)
SPEC = {'version': VERSION, 'capture_samples_per_action': 12, 'capture_interval_ms': 100,
        'launcher_warmup': 1, 'launcher_actions': 5, 'scene_size': [640, 360],
        'input': 'linux-uinput-fixed-desktop-v1', 'capture_scope': 'coarse-output-state-only',
        'fine_grained_motion': False, 'capture_fallback': False, 'changed_tiles_minimum': 12,
        'stable_tiles_maximum': 8, 'tile_grid': [32,18], 'tile_rgb_l1_threshold': 30,
        'drag_minimum_distance_pixels': 80, 'drag_maximum_distance_pixels':162, 'drag_pointer_steps':9}


def prepare_scene(layout, record):
    # Caller has stopped the UID and reset its profile. No live target can
    # replace these paths while the root worker creates its fixed fixture.
    parent = layout.home / '.local/share/applications'
    parent.mkdir(parents=True)
    for path in (layout.home / '.local', layout.home / '.local/share', parent):
        os.chown(path, record['uid'], record['gid'])
    path = parent / 'org.vela.BenchmarkScene.desktop'
    executable = str(layout.helper.parent / 'vela-bench-scene')
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', executable):
        raise BenchError('Unsafe installed scene executable path')
    path.write_text('[Desktop Entry]\nType=Application\nName=velabenchscene\n'
        'Exec=' + executable + ' --launch\nIcon=applications-system\nTerminal=false\nCategories=Utility;\n')
    os.chown(path, record['uid'], record['gid'])


def changed(a, b):
    if any(not isinstance(grid, list) or len(grid) != 576 or any(type(v) is not int or not 0 <= v <= 0xffffff for v in grid)
           for grid in (a, b)):
        raise BenchError('Invalid fixed 32x18 capture fingerprint')
    return sum(sum(abs(((x >> shift) & 255) - ((y >> shift) & 255)) for shift in (0, 8, 16)) > SPEC['tile_rgb_l1_threshold']
               for x, y in zip(a, b))


def marker(row, identity=SCENE):
    found = [item for item in row['markers'] if item['frame'] == identity]
    if len(found) != 1:
        observed = [item.get('frame') for item in row['markers']]
        raise BenchError(f"Expected one visible identified scene window {identity:#x}; capture {row.get('index', '?')}: observed IDs {observed}")
    return found[0]


def validate_frames(rows, target, profile, directory, stem, *, partial=False, offset=0, previous_end=-1):
    if (not partial and len(rows) != 12) or not 1 <= len(rows) <= 12:
        raise BenchError('Incomplete desktop capture sequence')
    backend = 'ext-image-copy-capture-v1' if target == 'vela' else 'kwin-screenshot2-v1'
    end = previous_end
    for index, row in enumerate(rows,offset):
        if (row.get('event') != 'capture' or row.get('index') != index or row.get('backend') != backend
                or (row.get('width'), row.get('height')) != (profile['mode']['width'], profile['mode']['height'])):
            raise BenchError('Desktop capture route/geometry/sample changed')
        for key in ('begin_monotonic_ns', 'end_monotonic_ns'):
            value = row.get(key)
            if isinstance(value, str) and value.isdigit(): value = int(value)
            if type(value) is not int or value < 0: raise BenchError('Invalid desktop capture clock')
            row[key] = value
        if not end <= row['begin_monotonic_ns'] <= row['end_monotonic_ns']:
            raise BenchError('Reversed or overlapping desktop capture intervals')
        end = row['end_monotonic_ns']
        changed(row.get('tiles'), row.get('tiles'))
        if not isinstance(row.get('markers'), list) or len(row['markers']) > 16:
            raise BenchError('Invalid desktop marker list')
        for value in row['markers']:
            if (not isinstance(value, dict) or value.get('marker') != 'vela-frame-id-v2'
                    or any(type(value.get(k)) is not int for k in ('frame','x','y'))
                    or not 0 <= value['frame'] <= 0xffffffff
                    or not 0 <= value['x'] <= row['width'] - 608 or not 0 <= value['y'] <= row['height'] - 32):
                raise BenchError('Invalid desktop scene marker')
        name = f'{target}-desktop-{stem}-{index:02d}.png'
        if 'png_base64' in row:
            try: png = base64.b64decode(row.pop('png_base64'), validate=True)
            except (ValueError, TypeError) as exc: raise BenchError('Invalid desktop PNG encoding') from exc
            if not 24 <= len(png) <= 12 * 1024 * 1024 or png[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('!II',png[16:24]) != (row['width'],row['height']):
                raise BenchError('Desktop PNG geometry differs from capture')
            (directory / name).write_bytes(png)
            row.update(image_file=name, image_sha256=hashlib.sha256(png).hexdigest())
        if row.get('image_file') != name or hashlib.sha256((directory/name).read_bytes()).hexdigest() != row.get('image_sha256'):
            raise BenchError('Desktop capture image changed')
    return rows


def action_metrics(rows, actions, kind):
    if not actions or any(a.get('event') != 'injected' or type(a.get('before_monotonic_ns')) is not int
                          or type(a.get('after_monotonic_ns')) is not int
                          or not 0 <= a['before_monotonic_ns'] <= a['after_monotonic_ns'] for a in actions):
        raise BenchError('Invalid desktop input intervals')
    commands = [a.get('command') for a in actions]
    expected = {'launcher-open':['launcher'], 'desktop-hide':['desktop'], 'desktop-restore':['desktop']}
    if kind in expected and commands != expected[kind]: raise BenchError('Desktop action differs from the fixed scenario')
    if kind == 'baseline' and commands not in (['escape'], ['pulse']): raise BenchError('Invalid baseline action')
    if kind == 'drag':
        if (len(commands)!=12 or commands[1]!='down' or commands[-1]!='up'
                or any(not isinstance(c,str) or not re.fullmatch(r'point [0-9]{1,5} [0-9]{1,5}',c)
                       or any(int(v)>65535 for v in c.split()[1:]) for c in [commands[0],*commands[2:-1]])):
            raise BenchError('Invalid fixed pointer drag sequence')
    if any(a['after_monotonic_ns']>b['before_monotonic_ns'] for a,b in zip(actions,actions[1:])):
        raise BenchError('Desktop injection ordering changed')
    injection = actions[0]
    if rows[1]['end_monotonic_ns'] > injection['before_monotonic_ns']:
        raise BenchError('No pre-action capture baseline')
    baseline = rows[1]['tiles']
    if changed(rows[0]['tiles'], baseline) > 8:
        raise BenchError('Desktop baseline is not settled')
    post = [r for r in rows[2:] if r['begin_monotonic_ns'] >= injection['after_monotonic_ns']]
    if len(post) < 2: raise BenchError('Insufficient post-action observations')
    result = {'scope':'coarse-output-state-only', 'optical_latency':False, 'fine_grained_motion':False,
              'capture_count':len(rows), 'maximum_capture_gap_ms':max(b['end_monotonic_ns']-a['begin_monotonic_ns'] for a,b in zip(rows,rows[1:]))/1e6}
    if kind == 'launcher-open':
        responsive = [r for r in post if changed(baseline,r['tiles']) >= 12]
        if len(responsive) < 2 or changed(post[-2]['tiles'],post[-1]['tiles']) > 8:
            raise BenchError('Launcher absent, ambiguous or not settled in captured output')
        result.update(first_observed_response_upper_ms=(responsive[0]['end_monotonic_ns']-injection['before_monotonic_ns'])/1e6,
                      first_observed_response_lower_ms=0, changed_tiles=changed(baseline,post[-1]['tiles']))
    elif kind == 'desktop-hide':
        marker(rows[1])
        if any(v['frame']==SCENE for r in post[-2:] for v in r['markers']):
            raise BenchError('Show-desktop did not hide the scene')
    elif kind == 'desktop-restore':
        if any(v['frame']==SCENE for v in rows[1]['markers']): raise BenchError('Desktop was not hidden before restore')
        marker(post[-1])
    elif kind == 'drag':
        origin, final = marker(rows[1]), marker(rows[-1])
        distance = ((final['x']-origin['x'])**2+(final['y']-origin['y'])**2)**.5
        positions = [marker(r) for r in post]
        if distance < 80 or len({(p['x'],p['y']) for p in positions}) < 3:
            raise BenchError('Window drag did not establish identified visible movement')
        result.update(start=origin, final=final, distance_pixels=distance,
                      observed_positions=[{'x':p['x'],'y':p['y']} for p in positions],
                      missed_refreshes=None, frame_by_frame_regularity=None)
    elif kind != 'baseline': raise BenchError('Unknown desktop observation case')
    return result


def run_observation(host, record, directory, target, layout, profile, phase, health, progress=None):
    result = {'schema':1,'method':VERSION,'spec':SPEC,'phase':phase,'status':'failed','failures':[],
              'actions':[],'launcher_launch_verified':False,'uinput_cleanup':'failed','family_complete':False}
    keyboard = scene = None
    sysfs = None
    scene_log = directory / f'{target}-desktop-scene.stderr'
    def pulse(command):
        health()
        keyboard.stdin.write((command+'\n').encode()); keyboard.stdin.flush()
        row = input_lines.next(time.monotonic()+5)
        row['command'] = command
        return row
    def capture(stem, kind, action):
        health()
        if progress: progress(stem)
        command = user_command(record,layout,'capture',['/usr/bin/python3','-I',str(layout.helper),'_capture',target,profile['primary']],runtime_seconds=60)
        process = None
        rows, injected = [], []
        with (directory/f'{target}-desktop-{stem}.stderr').open('wb') as errors:
            try:
                process = host.spawn(command,errors)
                lines = Lines(process, max_record_bytes=16*1024*1024)
                deadline = time.monotonic()+65
                for i in range(12):
                    row=lines.next(deadline)
                    validate_frames([row],target,profile,directory,stem,partial=True,offset=i,
                                    previous_end=rows[-1]['end_monotonic_ns'] if rows else -1)
                    rows.append(row)
                    if i >= 1:
                        injected.extend(action(i,rows) or [])
                if process.wait(timeout=5): raise BenchError('Desktop capture failed')
            except BaseException as exc:
                try: validate_frames(rows,target,profile,directory,stem,partial=True)
                except (BenchError,OSError,ValueError,KeyError,TypeError):
                    for row in rows: row.pop('png_base64',None)
                result['actions'].append({'id':stem,'kind':kind,'injections':injected,'captures':rows,'error':str(exc)})
                raise
            finally:
                stop_process(process)
                stop_instrument(host,record,layout,'capture')
        validate_frames(rows,target,profile,directory,stem)
        observation = {'id':stem,'kind':kind,'injections':injected,'captures':rows}
        result['actions'].append(observation)
        try:
            observation['metrics'] = action_metrics(rows,injected,kind)
        except (BenchError,OSError,ValueError,KeyError,TypeError) as exc:
            observation['error'] = str(exc)
            raise
        return rows
    def one(command): return lambda i,rows: [pulse(command)] if i==1 else []
    try:
        if phase not in PHASES: raise BenchError('Unsupported desktop observation phase')
        with scene_log.open('wb') as errors:
            scene = host.spawn(user_command(record,layout,'scene',[str(layout.helper.parent/'vela-bench-scene'),'--scene'],runtime_seconds=180),errors)
            scene_lines = Lines(scene)
            ready = scene_lines.next(time.monotonic()+10)
            if ready != {'event':'scene-ready','protocol':'wayland','width':640,'height':360}: raise BenchError('Invalid native scene')
            keyboard = host.spawn([str(layout.helper.parent/'vela-bench-input'),'--desktop'],errors)
            input_lines = Lines(keyboard)
            ready = input_lines.next(time.monotonic()+5)
            sysfs = ready.get('sysfs','')
            if (ready.get('event') != 'uinput-ready' or ready.get('method') != SPEC['input']
                    or ready.get('clock') != 'CLOCK_MONOTONIC'
                    or not re.fullmatch(r'/sys/devices/virtual/input/input[0-9]+',sysfs)):
                raise BenchError('Invalid desktop uinput identity')
            time.sleep(.5)
            if phase == 'responsiveness':
                for index in range(6):
                    opened = capture(f'launcher-{index}', 'launcher-open', one('launcher'))
                    baseline = opened[1]['tiles']
                    closed = capture(f'close-{index}', 'baseline', one('escape'))
                    if changed(closed[-1]['tiles'],baseline)>8: raise BenchError('Launcher did not return to its closed baseline')
                    marker(closed[-1])
                # Content verification: search the installed unique fixture
                # through the native launcher and require its distinct surface.
                result['launch_actions']=[pulse('launcher')]; time.sleep(.3)
                result['launch_actions'].append(pulse('search')); time.sleep(1)
                result['launch_actions'].append(pulse('enter')); time.sleep(1)
                receipt = host.run(user_command(record,layout,'observation',[str(layout.helper.parent/'vela-bench-scene'),'--receipt']),timeout=30)
                receipt = decode_json(receipt.stdout)
                if (receipt.get('event')!='launcher-app-started' or receipt.get('uid')!=record['uid']
                        or type(receipt.get('pid')) is not int or receipt['pid']<=0 or receipt.get('marker')!=LAUNCHED):
                    raise BenchError('Native launcher did not start the declared fixture')
                launched = capture('launched','baseline',one('pulse'))
                marker(launched[1], LAUNCHED)
                result.update(launcher_launch_verified=True,launch_receipt=receipt)
                result['launch_actions'].append(pulse('close')); time.sleep(.3)
                capture('desktop-hide','desktop-hide',one('desktop'))
                capture('desktop-restore','desktop-restore',one('desktop'))
            else:
                def drag(i, rows):
                    nonlocal initial
                    events=[]
                    if i==1:
                        initial=marker(rows[-1])
                        # Select a direction with enough physical room for the
                        # entire scene; small displays may need vertical motion.
                        x0,y0=initial['x']-16,initial['y']-16
                        room,dx,dy=max(((x0,-1,0),(profile['mode']['width']-x0-640,1,0),
                                       (y0,0,-1),(profile['mode']['height']-y0-360,0,1)))
                        distance=min(162,room-2)
                        if distance<80: raise BenchError('Output has insufficient room for the fixed window-drag observation')
                        initial=dict(initial,dx=dx*distance,dy=dy*distance)
                        x,y=initial['x']+DRAG_GRIP[0],initial['y']+DRAG_GRIP[1]
                        events.append(pulse(f"point {round(x*65535/(profile['mode']['width']-1))} {round(y*65535/(profile['mode']['height']-1))}"))
                        time.sleep(.1); events.append(pulse('down'))
                        moved=scene_lines.next(time.monotonic()+5)
                        if moved != {'event':'move-request','accepted':True}: raise BenchError('Scene move request was not accepted')
                    elif i<11:
                        x=initial['x']+DRAG_GRIP[0]+initial['dx']*(i-1)/9
                        y=initial['y']+DRAG_GRIP[1]+initial['dy']*(i-1)/9
                        events.append(pulse(f"point {round(x*65535/(profile['mode']['width']-1))} {round(y*65535/(profile['mode']['height']-1))}"))
                    elif i==11: events.append(pulse('up'))
                    return events
                initial=None
                capture('window-drag','drag',drag)
        result['status']='passed'
    except (BenchError,OSError,ValueError,TimeoutError,subprocess.TimeoutExpired) as exc:
        result['failures'].append(str(exc))
    finally:
        if keyboard and keyboard.poll() is None:
            try:
                keyboard.stdin.write(b'quit\n'); keyboard.stdin.flush(); keyboard.wait(timeout=2)
            except (OSError,subprocess.TimeoutExpired): pass
        stop_process(keyboard); stop_process(scene)
        stop_instrument(host,record,layout,'scene')
        if sysfs:
            deadline=time.monotonic()+2
            while Path(sysfs).exists() and time.monotonic()<deadline: time.sleep(.05)
            result['uinput_cleanup']='failed' if Path(sysfs).exists() else 'passed'
        if result['uinput_cleanup']!='passed':
            result['status']='failed'; result['failures'].append('Desktop input device cleanup not verified')
        atomic_json(directory/f'{target}-desktop.json',result)
    return result


def audit_observation(directory, target, profile, phase):
    if phase not in PHASES: raise BenchError('Unsupported desktop observation phase')
    saved = read_json(directory/f'{target}-desktop.json')
    if (saved.get('status')!='passed' or saved.get('method')!=VERSION or saved.get('spec')!=SPEC
            or saved.get('phase')!=phase or saved.get('failures')!=[] or saved.get('uinput_cleanup')!='passed'
            or saved.get('family_complete') is not False):
        raise BenchError('Desktop observation failed or its fixed workload changed')
    expected = ([(f'{kind}-{i}', 'launcher-open' if kind=='launcher' else 'baseline') for i in range(6) for kind in ('launcher','close')]
                + [('launched','baseline'),('desktop-hide','desktop-hide'),('desktop-restore','desktop-restore')]
                if phase=='responsiveness' else [('window-drag','drag')])
    if [(a.get('id'),a.get('kind')) for a in saved.get('actions',[])] != expected:
        raise BenchError('Missing or unexpected desktop actions')
    previous_end=-1
    for action in saved['actions']:
        validate_frames(action['captures'],target,profile,directory,action['id'])
        if action['captures'][0]['begin_monotonic_ns'] < previous_end:
            raise BenchError('Desktop observation sequences overlap or reverse their clock')
        previous_end=action['captures'][-1]['end_monotonic_ns']
        if action['metrics'] != action_metrics(action['captures'],action['injections'],action['kind']):
            raise BenchError('Desktop metrics differ from captured evidence')
    if phase=='responsiveness':
        if saved.get('launcher_launch_verified') is not True: raise BenchError('Unverified launcher content')
        receipt=saved.get('launch_receipt',{})
        if (receipt.get('schema')!=1 or receipt.get('event')!='launcher-app-started'
                or type(receipt.get('uid')) is not int or type(receipt.get('pid')) is not int
                or receipt['uid']<=0 or receipt['pid']<=0 or receipt.get('marker')!=LAUNCHED):
            raise BenchError('Missing native launcher app receipt')
        for i in range(6):
            opened,closed=saved['actions'][i*2:i*2+2]
            if changed(opened['captures'][1]['tiles'],closed['captures'][-1]['tiles'])>8:
                raise BenchError('Launcher failed its return-to-baseline control')
        marker(saved['actions'][12]['captures'][1],LAUNCHED)
        launch=saved.get('launch_actions',[])
        if ([a.get('command') for a in launch] != ['launcher','search','enter','close']
                or any(a.get('event')!='injected' or type(a.get('before_monotonic_ns')) is not int
                    or type(a.get('after_monotonic_ns')) is not int
                    or not 0 <= a['before_monotonic_ns'] <= a['after_monotonic_ns'] for a in launch)
                or any(a['after_monotonic_ns']>b['before_monotonic_ns'] for a,b in zip(launch,launch[1:]))
                or launch[2]['after_monotonic_ns']>saved['actions'][12]['captures'][0]['begin_monotonic_ns']
                or launch[3]['before_monotonic_ns']<saved['actions'][12]['captures'][-1]['end_monotonic_ns']):
            raise BenchError('Missing or inconsistent native launcher search/launch actions')
    return saved
