# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed toolkit scaling fixtures; geometry audit and unresampled visual evidence."""
import base64
import copy
import hashlib
from pathlib import Path
import re
import select
import time

from . import BenchError
from .archive import decode_json
from .calibration import Lines, stop_process
from .diagnostics import user_command, stop_instrument
from .pixels import png_rgb, png_bytes

METHOD='toolkit-scaling-corpus-v1'
CONTENT='Vela toolkit corpus v1|DejaVu Sans|16px|480x320|Alpha,100%,Ready|Beta,125%,Local|Gamma,150%,Fixed|Delta,200%,Offline|0123456789 AaBb CcDd|markers:8,16'
CONTENT_HASH=hashlib.sha256(CONTENT.encode()).hexdigest()
CASES={'qt-wayland':'vela-bench-corpus-qt','gtk-wayland':'vela-bench-corpus-gtk',
       'browser-wayland':'vela-bench-corpus-web','qt-xwayland':'vela-bench-corpus-qt'}
REASONS=('fixed-font-unavailable','x11-display-unavailable','x11-display-unreachable',
         'browser-rendering-overrides','browser-sandbox-user-namespace-unavailable')
SPEC={'method':METHOD,'cases':list(CASES),'content_sha256':CONTENT_HASH,'font':'DejaVu Sans',
      'font_px':16,'matrix':'five-scales-five-window-states-per-supported-case',
      'browser':'off-record-Qt-WebEngine-inline-local-HTML; sandbox-required',
      'xwayland_policy':'desktop-default; measured-root-to-output-mapping',
      'capture_resampled_by_suite':False,'text_quality_score':None,'geometry_tolerance_pixels':2}


def validate_inventory(inventory):
    if not isinstance(inventory,dict) or set(inventory)!=set(CASES):raise BenchError('Incomplete corpus inventory')
    for case,tool in CASES.items():
        item=inventory[case]
        if not isinstance(item,dict) or item.get('tool')!=tool:raise BenchError('Corpus tool identity changed')
        if item.get('status')=='unsupported':
            if item!={'tool':tool,'status':'unsupported','reason':'not-installed'}:raise BenchError('Invalid corpus availability')
            continue
        desc=item.get('description',{})
        if (not isinstance(desc,dict) or item.get('status')!='available' or not re.fullmatch('[0-9a-f]{64}',item.get('binary_sha256',''))
                or desc.get('schema')!=1 or desc.get('method')!=METHOD or desc.get('content_sha256')!=CONTENT_HASH
                or desc.get('font_requested')!='DejaVu Sans' or desc.get('font_size_px')!=16
                or desc.get('cases')!=[c for c,t in CASES.items() if t==tool] or desc.get('toolkit')!=('GTK' if case=='gtk-wayland' else 'Qt')
                or not isinstance(desc.get('toolkit_version'),str) or not desc['toolkit_version']):
            raise BenchError('Corpus descriptor differs from the fixed content/toolkit')
        if case=='browser-wayland' and any(not desc.get(k) for k in ('engine_version','chromium_version')):
            raise BenchError('Missing browser engine versions')
    return inventory


def describe_inventory(host,layout,setup):
    from .host import trusted,file_hash
    result={};descriptions={}
    for case,tool in CASES.items():
        path=layout.helper.parent/tool
        if str(path) not in setup['files']:
            result[case]={'tool':tool,'status':'unsupported','reason':'not-installed'};continue
        trusted(path)
        digest=file_hash(path)
        if digest!=setup['files'][str(path)]:raise BenchError('Corpus executable changed after setup')
        if tool not in descriptions:
            descriptions[tool]=decode_json(host.run([str(path),'--describe'],timeout=15).stdout)
        result[case]={'tool':tool,'status':'available','binary_sha256':digest,'description':descriptions[tool]}
    return validate_inventory(result)


def palette(case):
    family=list(CASES).index(case)
    colors=((208,32,208),(32,208,32),(32,64,224),(224,128,32))
    return [bytes((r+(-16 if i in (0,3) else 16)*family,
                   g+(-16 if i==1 else 16)*family,
                   b+(0 if i==0 else -16 if i==2 else 16)*family)) for i,(r,g,b) in enumerate(colors)]


def markers(rgb,width,case):
    centers=[]
    for color in palette(case):
        points=set();offset=0
        while True:
            offset=rgb.find(color,offset)
            if offset<0:break
            if offset%3==0:points.add(offset//3)
            offset+=3 if offset%3==0 else 1
            if len(points)>65536:raise BenchError('Corpus marker exceeds bounded area')
        candidates=[]
        while points:
            pending=[points.pop()];component=[]
            while pending:
                p=pending.pop();component.append(p)
                for q in (p-width,p+width,*([p-1] if p%width else []),*([p+1] if p%width<width-1 else [])):
                    if q in points:points.remove(q);pending.append(q)
            xs=[p%width for p in component];ys=[p//width for p in component]
            left,right,top,bottom=min(xs),max(xs)+1,min(ys),max(ys)+1
            w,h=right-left,bottom-top
            if 3<=w<=128 and 3<=h<=128 and .75<=w/h<=1.33 and len(component)>=.8*w*h:
                candidates.append([(left+right)/2,(top+bottom)/2])
        if len(candidates)!=1:raise BenchError('Expected one visible identified corpus marker per corner')
        centers.append(candidates[0])
    a,b,c,d=centers
    if (max(abs(a[1]-b[1]),abs(c[1]-d[1]),abs(a[0]-c[0]),abs(b[0]-d[0]))>2
            or b[0]<=a[0] or c[1]<=a[1]):raise BenchError('Corpus markers do not form one canvas')
    return centers


def source_audit(events,case,item,uid):
    if not events or len(events)>128:raise BenchError('Incomplete corpus configure stream')
    previous=-1;pid=events[0].get('pid')
    for index,event in enumerate(events,1):
        if (any(event.get(k)!=v for k,v in item['description'].items())
                or event.get('event')!='corpus-state' or event.get('case')!=case
                or event.get('uid')!=uid or type(pid) is not int or pid<=0 or event.get('pid')!=pid
                or event.get('protocol')!=('x11' if case=='qt-xwayland' else 'wayland')
                or event.get('backend')!=('xcb' if case=='qt-xwayland' else 'wayland')
                or event.get('sequence')!=index or type(event.get('monotonic_ns')) is not int
                or event['monotonic_ns']<previous or type(event.get('maximized')) is not bool
                or any(type(event.get(k)) is not int or not 32<event[k]<=8192 for k in ('canvas_width','canvas_height'))
                or type(event.get('reported_scale')) not in (int,float) or not .5<=event['reported_scale']<=4):
            raise BenchError('Corpus source identity, backend, font or geometry changed')
        if event.get('font_resolved')!='DejaVu Sans' or not re.fullmatch('[0-9a-f]{64}',event.get('font_sha256','')):
            raise BenchError('Corpus fixed font unavailable')
        if case=='browser-wayland' and (event.get('sandbox')!='enabled' or event.get('page_requests')!='inline-local-only'
                or event.get('dom',{}).get('ready') is not True
                or [event['dom'].get('width'),event['dom'].get('height')]!=[event['canvas_width'],event['canvas_height']]):
            raise BenchError('Browser local page is not ready in its sandbox')
        previous=event['monotonic_ns']
    return events[-1]


def physical_scale(meta,case,mode,scale):
    if case!='qt-xwayland':return scale,scale
    keys=('x11_width','x11_height','x11_root_width','x11_root_height')
    if any(type(meta.get(k)) is not int or not 0<meta[k]<=16384 for k in keys):
        raise BenchError('Missing X11 root/client mapping')
    return (meta['x11_width']/meta['canvas_width']*mode['width']/meta['x11_root_width'],
            meta['x11_height']/meta['canvas_height']*mode['height']/meta['x11_root_height'])


def image_metrics(path,meta,case,mode,scale):
    w,h,rgb=png_rgb(path)
    if [w,h]!=[mode['width'],mode['height']]:raise BenchError('Corpus capture is not physical output size')
    a,b,c,d=markers(rgb,w,case)
    cw,ch=meta['canvas_width'],meta['canvas_height']
    sx,sy=physical_scale(meta,case,mode,scale)
    observed_x=(b[0]-a[0])/(cw-32);observed_y=(c[1]-a[1])/(ch-32)
    error=max(abs((b[0]-a[0])-(cw-32)*sx),abs((c[1]-a[1])-(ch-32)*sy))
    bounds=[round(a[0]-16*observed_x),round(a[1]-16*observed_y),
            round(b[0]+16*observed_x),round(c[1]+16*observed_y)]
    left,top,right,bottom=bounds;clip=[max(0,left),max(0,top),min(w,right),min(h,bottom)]
    x,y,x2,y2=clip
    if x2<=x or y2<=y:raise BenchError('Corpus canvas outside capture')
    crop=b''.join(rgb[(row*w+x)*3:(row*w+x2)*3] for row in range(y,y2))
    metrics={'marker_centers': [a,b,c,d],'expected_physical_scale':[sx,sy],
             'observed_physical_scale':[observed_x,observed_y],'span_error_pixels':error,
             'geometry_within_tolerance':error<=2,'canvas_bounds':bounds,'crop_bounds':clip,
             'offscreen_pixels':(right-left)*(bottom-top)-(x2-x)*(y2-y),
             'capture_resampled_by_suite':False,'text_quality_score':None}
    return metrics,(x2-x,y2-y,crop)


def row_metrics(row,directory,profile,target,item,uid):
    from .platform_workloads import SCALES,STATES,scaling_actions
    from .conditions import compare_outputs
    case,scale,state=row['case'],row['scale'],row['state']
    if case not in CASES or type(scale) is not float or scale not in SCALES or state not in STATES:
        raise BenchError('Unknown corpus condition')
    stem=f'{target}-corpus-{case}-{round(scale*100)}-{state}'
    for key,suffix in (('image','.png'),('crop','-crop.png')):
        name=stem+suffix
        if row.get(key+'_file')!=name or hashlib.sha256((directory/name).read_bytes()).hexdigest()!=row.get(key+'_sha256'):
            raise BenchError('Corpus original/crop evidence changed')
    meta=source_audit(row['source_events'],case,item,uid)
    if row.get('source')!=meta:raise BenchError('Corpus summary differs from source events')
    cap=row['capture'];backend='ext-image-copy-capture-v1' if target=='vela' else 'kwin-screenshot2-v1'
    if (cap.get('event')!='capture' or cap.get('backend')!=backend or cap.get('index')!=0
            or [cap.get('width'),cap.get('height')]!=[profile['mode']['width'],profile['mode']['height']]
            or any(type(cap.get(k)) is not int for k in ('begin_monotonic_ns','end_monotonic_ns'))
            or not 0<=meta['monotonic_ns']<=cap['begin_monotonic_ns']<=cap['end_monotonic_ns']):
        raise BenchError('Corpus capture backend or clock mismatch')
    wanted=copy.deepcopy(profile['outputs'])
    for output in wanted:
        if output['enabled']:output['scale']=scale
    compare_outputs(row['outputs'],wanted)
    injections=row['injections'];park=row['pointer_parking']
    if [r.get('command') for r in injections]!=scaling_actions(target,state):raise BenchError('Corpus actions changed')
    previous=park.get('before_monotonic_ns',0)
    for receipt in [park,*injections]:
        if (receipt.get('event')!='injected' or any(type(receipt.get(k)) is not int for k in ('before_monotonic_ns','after_monotonic_ns'))
                or not 0<=receipt['before_monotonic_ns']<=receipt['after_monotonic_ns']<=cap['begin_monotonic_ns']
                or meta['monotonic_ns']<receipt['before_monotonic_ns']
                or receipt['before_monotonic_ns']<previous):raise BenchError('Corpus action/source ordering changed')
        previous=receipt['after_monotonic_ns']
    if injections and injections[0]['before_monotonic_ns']<row['source_events'][0]['monotonic_ns']:
        raise BenchError('Corpus action precedes its fresh client')
    if park.get('command')!='point 65535 65535':raise BenchError('Corpus pointer parking changed')
    initial=row['initial_canvas']
    if initial!=[row['source_events'][0]['canvas_width'],row['source_events'][0]['canvas_height']]:
        raise BenchError('Corpus initial size differs from fresh client source')
    if (len(initial)!=2 or any(type(n) is not int or not 32<n<=8192 for n in initial)
            or state in ('opened','restored') and [meta['canvas_width'],meta['canvas_height']]!=initial
            or meta['maximized']!=(state=='maximized')):raise BenchError('Corpus window state not acknowledged')
    sx,sy=physical_scale(meta,case,profile['mode'],scale)
    if state=='maximized' and (meta['canvas_width']*sx<.9*profile['mode']['width'] or meta['canvas_height']*sy<.6*profile['mode']['height']):
        raise BenchError('Corpus maximized geometry incomplete')
    if state.startswith('snap-') and not .35<=meta['canvas_width']*sx/profile['mode']['width']<=.65:
        raise BenchError('Corpus half-output geometry incomplete')
    metrics,crop=image_metrics(directory/row['image_file'],meta,case,profile['mode'],scale)
    if png_rgb(directory/row['crop_file'])!=(crop[0],crop[1],bytearray(crop[2])):
        raise BenchError('Corpus crop resampled or differs from original pixels')
    if state.startswith('snap-'):
        center=(metrics['canvas_bounds'][0]+metrics['canvas_bounds'][2])/2
        if (state=='snap-left' and center>=profile['mode']['width']/2 or state=='snap-right' and center<=profile['mode']['width']/2):
            raise BenchError('Corpus snap is on the wrong output side')
    return metrics


def run(host,record,directory,target,layout,profile,health,progress,result,keyboard,keys,errors):
    from .platform_workloads import SCALES,STATES,apply_outputs,scaling_actions
    inventory=validate_inventory(record['corpus_inventory'])
    result['corpus_inventory']=inventory;result['corpus_uid']=record['uid'];result['corpus_availability']={}
    for case in CASES:
        item=inventory[case]
        availability=result['corpus_availability']
        if item['status']=='unsupported':availability[case]={'status':'unsupported','reason':'not-installed'};continue
        availability[case]={'status':'failed'};unsupported=False
        for scale in SCALES:
            wanted=copy.deepcopy(profile['outputs'])
            for output in wanted:
                if output['enabled']:output['scale']=scale
            health();outputs=apply_outputs(host,record,layout,target,wanted)
            keyboard.stdin.write(b'point 65535 65535\n');keyboard.stdin.flush()
            parked=keys.next(time.monotonic()+5);parked['command']='point 65535 65535'
            for state in STATES:
                health();label=f'corpus-{case}-{round(scale*100)}-{state}'
                if progress:progress(label)
                process=None
                try:
                    process=host.spawn(user_command(record,layout,'scene',[str(layout.helper.parent/item['tool']),'--case',case],runtime_seconds=60),errors)
                    source=Lines(process);meta=source.next(time.monotonic()+15)
                    if meta.get('event')=='corpus-unsupported':
                        if (scale!=SCALES[0] or state!=STATES[0] or meta.get('schema')!=1 or meta.get('case')!=case
                                or meta.get('reason') not in REASONS or process.wait(timeout=3)!=77):
                            raise BenchError('Corpus capability failed after its first condition or without a valid receipt')
                        availability[case]={'status':'unsupported','reason':meta['reason'],'receipt':meta};unsupported=True;break
                    initial=[meta.get('canvas_width'),meta.get('canvas_height')];injections=[]
                    time.sleep(.25)
                    for action in scaling_actions(target,state):
                        keyboard.stdin.write((action+'\n').encode());keyboard.stdin.flush()
                        receipt=keys.next(time.monotonic()+5);receipt['command']=action;injections.append(receipt);time.sleep(.35)
                    deadline=time.monotonic()+3
                    while source.pending or select.select([process.stdout],[],[],.2)[0]:
                        if time.monotonic()>deadline or len(source.records)>=128:raise BenchError('Corpus configure stream did not settle')
                        meta=source.next(time.monotonic()+3)
                    for event in source.records:event['monotonic_ns']=int(event['monotonic_ns'])
                    source_audit(source.records,case,item,record['uid'])
                    cap=decode_json(host.run(user_command(record,layout,'capture',['/usr/bin/python3','-I',str(layout.helper),
                        '_capture',target,profile['primary'],'--one'],runtime_seconds=25),timeout=30).stdout)
                    png=base64.b64decode(cap.pop('png_base64'),validate=True)
                    if len(png)>12*1024*1024:raise BenchError('Corpus capture exceeds evidence limit')
                    for key in ('begin_monotonic_ns','end_monotonic_ns'):cap[key]=int(cap[key])
                    stem=f'{target}-{label}';image=stem+'.png';cropname=stem+'-crop.png';(directory/image).write_bytes(png)
                    row={'case':case,'scale':scale,'state':state,'initial_canvas':initial,'outputs':outputs,
                         'source':meta,'source_events':source.records,'injections':injections,'pointer_parking':parked,'capture':cap,
                         'image_file':image,'image_sha256':hashlib.sha256(png).hexdigest(),'crop_file':cropname}
                    result['scenarios'][label]=row
                    metrics,crop=image_metrics(directory/image,meta,case,profile['mode'],scale)
                    encoded=png_bytes(*crop);(directory/cropname).write_bytes(encoded);row['crop_sha256']=hashlib.sha256(encoded).hexdigest()
                    row['metrics']=row_metrics(row,directory,profile,target,item,record['uid'])
                    if row['metrics']!=metrics:raise BenchError('Corpus pixel summary changed during capture')
                finally:
                    # Stop the owned systemd instrument cgroup before its waiting
                    # transport; browser descendants belong to that instrument.
                    stop_instrument(host,record,layout,'scene');stop_process(process)
            if unsupported:break
        if not unsupported:availability[case]={'status':'supported','cases':len(SCALES)*len(STATES)}


def audit(saved,directory,profile,target):
    from .platform_workloads import SCALES,STATES
    inventory=validate_inventory(saved.get('corpus_inventory'));availability=saved.get('corpus_availability',{})
    uid=saved.get('corpus_uid')
    if type(uid) is not int or uid<=0 or set(availability)!=set(CASES):raise BenchError('Corpus availability/session UID missing')
    expected=[];previous=-1
    for case in CASES:
        item=inventory[case]
        status=availability[case]
        if status.get('status')=='unsupported':
            if item['status']=='unsupported':
                if status!={'status':'unsupported','reason':'not-installed'}:raise BenchError('Unavailable corpus evidence changed')
            else:
                receipt=status.get('receipt',{})
                if (status.get('reason') not in REASONS or receipt!={'schema':1,'event':'corpus-unsupported','case':case,'reason':status['reason']}):
                    raise BenchError('Corpus unsupported receipt changed')
            continue
        if item['status']!='available' or status!={'status':'supported','cases':25}:raise BenchError('Corpus matrix not complete')
        for scale in SCALES:
            for state in STATES:
                name=f'corpus-{case}-{round(scale*100)}-{state}';expected.append(name);row=saved['scenarios'][name]
                if [row.get('case'),row.get('scale'),row.get('state')]!=[case,scale,state] or row.get('metrics')!=row_metrics(row,directory,profile,target,item,uid):
                    raise BenchError('Corpus metrics differ from original evidence')
                if row['source_events'][0]['monotonic_ns']<previous:raise BenchError('Corpus capture/source order changed')
                previous=row['capture']['end_monotonic_ns']
    return expected
