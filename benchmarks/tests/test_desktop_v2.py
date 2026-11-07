# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent PNG/source rejection tests for the P3 v2 acceptance path."""
import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError,desktop
from vela_bench.desktop_pixels import evidence
from vela_bench.pixels import png_bytes

PROFILE={'mode':{'width':1280,'height':720}}
UID=1001


def picture(rect,identity=desktop.SCENE,duplicate=False):
    width,height=1280,720;rgb=bytearray(b'\x08\x10\x18'*(width*height))
    def fill(x,y,w,h,color):
        for dy in range(y,y+h):rgb[(dy*width+x)*3:(dy*width+x+w)*3]=bytes(color)*w
    x,y,w,h=rect;fill(x,y,w,h,(32,40,48));x+=16;y+=16
    for left,span,color in ((0,16,(208,32,208)),(16,16,(32,208,32)),(576,16,(32,64,224)),(592,16,(224,128,32))):fill(x+left,y,span,32,color)
    for bit in range(32):fill(x+32+bit*17,y,17,32,(255,255,255) if identity>>bit&1 else (32,40,48))
    fill(rect[0]+w-24,rect[1]+h-24,16,16,(200,80,120) if identity==desktop.SCENE else (200,120,80))
    if duplicate:fill(20,650,16,16,(200,80,120))
    return png_bytes(width,height,rgb)


def source(sequence,stamp,w=640,h=360,active=True,identity=desktop.SCENE):
    return {'event':'scene-state','method':desktop.SPEC['scene_method'],'uid':UID,'pid':42,'protocol':'wayland',
            'id':identity,'sequence':sequence,'monotonic_ns':stamp,'width':w,'height':h,'active':active,'maximized':False,'scale':1}


def resize(directory):
    captures=[];stream=[{'event':'scene-ready','protocol':'wayland','width':640,'height':360},source(1,50_000_000)]
    path=desktop.pointer_path([80,80,640,360],PROFILE,'resize')
    commands=[path[0],'down',*path[1:],'up']
    injections=[]
    for i,c in enumerate(commands):
        stamp=211_000_000+i*70_000_000
        injections.append({'event':'injected','action_id':i+1,'command':c,'before_monotonic_ns':stamp,'after_monotonic_ns':stamp+1_000_000})
    stream.append({'event':'resize-request','accepted':True,'id':desktop.SCENE,'monotonic_ns':282_000_000})
    for i in range(12):
        growth=max(0,i-2)*13;w,h=640+growth,360+growth
        rect=[80,80,w,h];name=f'vela-desktop-window-resize-{i:02d}.png';file=directory/name;file.write_bytes(picture(rect))
        row=evidence(file);row.update(event='capture',index=i,backend='ext-image-copy-capture-v1',image_file=name,image_sha256=hashlib.sha256(file.read_bytes()).hexdigest(),
            begin_monotonic_ns=(100+i*100)*1_000_000,end_monotonic_ns=(110+i*100)*1_000_000)
        captures.append(row)
        if i>=3:stream.append(source(len(stream)-1,(95+i*100)*1_000_000,w,h))
    action={'id':'window-resize','kind':'resize','captures':captures,'injections':injections,'source_streams':{'scene':stream}}
    return action


class DesktopV2(unittest.TestCase):
    def test_binary_marker_tolerance_includes_low_green_bytes_and_excludes_outside_values(self):
        from vela_bench.pixels import png_rgb
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'image.png';path.write_bytes(picture([80,80,640,360]))
            width,height,rgb=png_rgb(path)
            at=(96*width+96)*3
            for color,accepted in (((188,12,188),True),((228,52,228),True),
                                   ((187,32,208),False),((229,32,208),False),
                                   ((208,11,208),False),((208,53,208),False)):
                rgb[at:at+3]=bytes(color)
                with self.subTest(color=color), patch('vela_bench.desktop_pixels.png_rgb',return_value=(width,height,rgb)):
                    self.assertEqual(bool(evidence(path)['markers']),accepted)

    def test_detector_reads_encoded_identity_footer_geometry_and_rgb_grid(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'image.png';path.write_bytes(picture([80,80,640,360]))
            value=evidence(path)
            self.assertEqual(value['rectangles'],{str(desktop.SCENE):[80,80,640,360]})
            self.assertEqual(value['markers'],[{'x':96,'y':96,'frame':desktop.SCENE,'marker':'vela-frame-id-v2'}])
            self.assertEqual(len(value['tiles']),576)
            path.write_bytes(picture([80,80,640,360],duplicate=True))
            self.assertEqual(evidence(path)['rectangles'],{})

    def test_real_png_resize_rejects_rehashed_forged_metadata_and_wrong_sources_or_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);a=resize(directory)
            actual=desktop.metrics(a,directory,'vela',PROFILE,a['source_streams'],UID)
            self.assertGreater(actual['distance_pixels'],80);self.assertIsNone(actual['missed_refreshes'])
            for broken in ('pixels','source-uid','source-pid','source-sequence','request','path','input-clock','backend','size'):
                b=copy.deepcopy(a)
                if broken=='pixels':b['captures'][-1]['rectangles'][str(desktop.SCENE)][2]+=1
                elif broken=='source-uid':b['source_streams']['scene'][1]['uid']=99
                elif broken=='source-pid':b['source_streams']['scene'][-1]['pid']=43
                elif broken=='source-sequence':b['source_streams']['scene'][-1]['sequence']+=1
                elif broken=='request':b['source_streams']['scene'][2]['accepted']=False
                elif broken=='path':b['injections'][4]['command']='point 1 1'
                elif broken=='input-clock':b['injections'][0]['before_monotonic_ns']=1
                elif broken=='backend':b['captures'][0]['backend']='portal'
                else:b['source_streams']['scene'][-1]['width']-=10
                with self.subTest(broken=broken),self.assertRaises(BenchError):desktop.metrics(b,directory,'vela',PROFILE,b['source_streams'],UID)

    def test_coarse_stability_is_fingerprint_evidence_and_missing_response_has_no_number(self):
        rows=[{'tiles':[0]*576,'begin_monotonic_ns':i*100_000_000,'end_monotonic_ns':i*100_000_000+1} for i in range(12)]
        receipts=[{'event':'injected','action_id':1,'command':'launcher','before_monotonic_ns':110_000_000,'after_monotonic_ns':111_000_000}]
        result=desktop.visual_timing(rows,receipts)
        self.assertIsNone(result['first_observed_response_upper_ms']);self.assertIsNone(result['first_observed_stable_state_upper_ms']);self.assertFalse(result['fine_grained_motion'])
        for row in rows[3:]:row['tiles'][:5]=[0xffffff]*5
        menu=desktop.visual_timing(rows,receipts,minimum=4)
        self.assertIsNotNone(menu['first_observed_response_upper_ms'])
        self.assertGreaterEqual(menu['first_observed_stable_state_upper_ms'],menu['first_observed_response_upper_ms'])
        self.assertIsNone(desktop.visual_timing(rows,receipts)['first_observed_response_upper_ms'])

    def test_held_alt_tab_requires_two_identified_native_focus_sources(self):
        rows=[{'tiles':[0]*576,'begin_monotonic_ns':i*100_000_000,'end_monotonic_ns':i*100_000_000+1,
               'markers':[{'frame':desktop.SCENE}], 'rectangles':{}} for i in range(12)]
        for row in rows[3:6]:row['tiles'][:20]=[0xffffff]*20
        receipts=[{'event':'injected','action_id':i+1,'command':c,'before_monotonic_ns':stamp,'after_monotonic_ns':stamp+1} for i,(c,stamp) in enumerate([('alt-tab-begin',110_000_000),('alt-tab-end',510_000_000)])]
        a={'id':'alt-tab','kind':'alt-tab','captures':rows,'injections':receipts}
        ready={'event':'scene-ready','protocol':'wayland','width':640,'height':360}
        streams={'scene':[ready,source(1,1,active=False),source(2,520_000_000)],
                 'peer':[ready,source(1,1,identity=desktop.PEER),source(2,520_000_000,active=False,identity=desktop.PEER)]}
        with patch.object(desktop,'validate_frames',side_effect=lambda rows,*a:rows):
            desktop.metrics(a,Path('/unused'),'vela',PROFILE,streams,UID,check_pixels=False)
            streams['peer'][-1]['active']=True
            with self.assertRaisesRegex(BenchError,'focus'):desktop.metrics(a,Path('/unused'),'vela',PROFILE,streams,UID,check_pixels=False)

    def test_workspace_identity_and_topology_cannot_be_inferred_from_a_changed_picture(self):
        value={'schema':1,'event':'workspace-state','target':'plasma','uid':UID,'count':2,'current_index':0,'ids':['a','b'],'names':['Desktop 1','Vela benchmark fixture'],'begin_monotonic_ns':'100'}
        desktop.workspace_audit(value,'plasma',UID)
        for key,replacement in [('uid',1),('ids',['a','a']),('count',3),('current_index',2),('begin_monotonic_ns','none')]:
            broken=dict(value,**{key:replacement})
            with self.subTest(key=key),self.assertRaises(BenchError):desktop.workspace_audit(broken,'plasma',UID)

    def test_source_failure_still_stops_every_owned_scene_and_keyboard(self):
        import io
        c=desktop.Controller.__new__(desktop.Controller)
        c.host=c.record=c.layout=None;c.processes={'scene':object(),'peer':object()};c.lines={};c.streams={}
        c.result={};c.keyboard=object();c.sysfs=None;c.errors=io.BytesIO()
        with patch.object(c,'collect',side_effect=BenchError('scene exited')),patch.object(desktop,'stop_instrument') as stop,patch.object(desktop,'stop_process') as terminate:
            with self.assertRaisesRegex(BenchError,'scene exited'):c.close()
        self.assertEqual([call.args[-1] for call in stop.call_args_list],['scene','observation'])
        self.assertEqual(terminate.call_count,3);self.assertEqual(c.processes,{})
        self.assertTrue(c.errors.closed)

    def test_missing_matrix_and_legacy_identity_cannot_pass_new_spec(self):
        from vela_bench.archive import atomic_json
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            value={'method':desktop.VERSION,'spec':desktop.SPEC,'status':'passed','phase':'slow-apps','failures':[],
                   'uinput_cleanup':'passed','family_complete':False,'uid':UID,'source_streams':{},'actions':[]}
            atomic_json(directory/'vela-desktop.json',value)
            with self.assertRaisesRegex(BenchError,'matrix'):desktop.audit_observation(directory,'vela',PROFILE,'slow-apps')
