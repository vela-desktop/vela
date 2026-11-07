# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError,corpus
from vela_bench.pixels import png_bytes,png_rgb


def inventory(available=True):
    result={}
    for case,tool in corpus.CASES.items():
        if not available:result[case]={'tool':tool,'status':'unsupported','reason':'not-installed'};continue
        desc={'schema':1,'method':corpus.METHOD,'content_sha256':corpus.CONTENT_HASH,'font_requested':'DejaVu Sans',
              'font_size_px':16,'font_resolved':'DejaVu Sans','font_sha256':'b'*64,'font_file':'/fixed/font.ttf',
              'toolkit':'GTK' if case=='gtk-wayland' else 'Qt','toolkit_version':'1.0',
              'cases':[c for c,t in corpus.CASES.items() if t==tool]}
        if case=='browser-wayland':desc.update(engine_version='1.0',chromium_version='100.0')
        result[case]={'tool':tool,'status':'available','binary_sha256':'a'*64,'description':desc}
    return result


def source(case='qt-wayland'):
    return dict(inventory()[case]['description'],event='corpus-state',case=case,uid=1001,pid=42,protocol='wayland',backend='wayland',
                canvas_width=100,canvas_height=80,maximized=False,reported_scale=1,sequence=1,monotonic_ns=20)


def image(case='qt-wayland',scale=1.,duplicate=False):
    w,h=400,300;rgb=bytearray(b'\xff\xff\xff'*(w*h));cw,ch=100,80
    for corner,color in enumerate(corpus.palette(case)):
        left=40+round((cw-24 if corner%2 else 8)*scale)
        top=40+round((ch-24 if corner//2 else 8)*scale)
        for y in range(top,top+round(16*scale)):
            for x in range(left,left+round(16*scale)):rgb[(y*w+x)*3:(y*w+x)*3+3]=color
    if duplicate:
        color=corpus.palette(case)[0]
        for y in range(230,246):
            for x in range(320,336):rgb[(y*w+x)*3:(y*w+x)*3+3]=color
    return w,h,rgb


class Corpus(unittest.TestCase):
    def test_original_and_crop_roundtrip_without_resampling(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'capture.png';w,h,rgb=image();path.write_bytes(png_bytes(w,h,rgb))
            self.assertEqual(png_rgb(path),(w,h,rgb))
            metrics,crop=corpus.image_metrics(path,source(),'qt-wayland',{'width':w,'height':h},1.)
            self.assertEqual(metrics['span_error_pixels'],0)
            self.assertEqual(metrics['crop_bounds'],[40,40,140,120])
            path.write_bytes(png_bytes(*crop));self.assertEqual(png_rgb(path),(100,80,bytearray(crop[2])))

    def test_wrong_scale_is_measured_outcome_not_fabricated_zero_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'capture.png';w,h,rgb=image();path.write_bytes(png_bytes(w,h,rgb))
            metrics,_=corpus.image_metrics(path,source(),'qt-wayland',{'width':w,'height':h},1.5)
            self.assertFalse(metrics['geometry_within_tolerance']);self.assertGreater(metrics['span_error_pixels'],2)
            self.assertIsNone(metrics['text_quality_score'])

    def test_ambiguous_marker_or_wrong_family_cannot_choose_good_crop(self):
        w,h,rgb=image(duplicate=True)
        with self.assertRaises(BenchError):corpus.markers(rgb,w,'qt-wayland')
        w,h,rgb=image()
        with self.assertRaises(BenchError):corpus.markers(rgb,w,'gtk-wayland')

    def test_all_four_unique_palettes_survive_fractional_scales(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'capture.png'
            for case in corpus.CASES:
                for scale in (1.,1.25,1.5,1.75,2.):
                    w,h,rgb=image(case,scale);path.write_bytes(png_bytes(w,h,rgb))
                    meta=source(case)
                    if case=='qt-xwayland':meta.update(x11_width=round(100*scale),x11_height=round(80*scale),x11_root_width=w,x11_root_height=h)
                    metrics,_=corpus.image_metrics(path,meta,case,{'width':w,'height':h},scale)
                    self.assertTrue(metrics['geometry_within_tolerance'],(case,scale,metrics))

    def test_x11_native_and_logical_root_policies_have_same_physical_mapping(self):
        mode={'width':1920,'height':1080};meta=dict(canvas_width=480,canvas_height=320,x11_width=960,x11_height=640,x11_root_width=1920,x11_root_height=1080)
        self.assertEqual(corpus.physical_scale(meta,'qt-xwayland',mode,2.),(2.,2.))
        meta.update(x11_width=480,x11_height=320,x11_root_width=960,x11_root_height=540)
        self.assertEqual(corpus.physical_scale(meta,'qt-xwayland',mode,2.),(2.,2.))
        del meta['x11_width']
        with self.assertRaises(BenchError):corpus.physical_scale(meta,'qt-xwayland',mode,2.)

    def test_source_backend_uid_font_content_and_sequence_are_required(self):
        item=inventory()['qt-wayland'];row=source()
        self.assertEqual(corpus.source_audit([row],'qt-wayland',item,1001),row)
        for key,value in [('backend','xcb'),('protocol','x11'),('uid',1000),('font_sha256','c'*64),
                          ('content_sha256','c'*64),('sequence',2),('monotonic_ns','20'),('reported_scale',float('nan'))]:
            with self.subTest(key=key),self.assertRaises(BenchError):
                corpus.source_audit([dict(row,**{key:value})],'qt-wayland',item,1001)

    def test_browser_requires_ready_local_sandboxed_page(self):
        row=source('browser-wayland');item=inventory()['browser-wayland']
        with self.assertRaises(BenchError):corpus.source_audit([row],'browser-wayland',item,1001)
        row.update(sandbox='enabled',page_requests='inline-local-only',dom={'ready':True,'width':100,'height':80})
        self.assertEqual(corpus.source_audit([row],'browser-wayland',item,1001),row)
        row['sandbox']='disabled'
        with self.assertRaises(BenchError):corpus.source_audit([row],'browser-wayland',item,1001)

    def test_unavailable_cases_are_explicit_and_do_not_count_as_tests(self):
        saved={'corpus_inventory':inventory(False),'corpus_uid':1001,'corpus_availability':{
            case:{'status':'unsupported','reason':'not-installed'} for case in corpus.CASES},'scenarios':{}}
        self.assertEqual(corpus.audit(saved,Path('/unused'),{},'vela'),[])
        del saved['corpus_availability']['gtk-wayland']
        with self.assertRaises(BenchError):corpus.audit(saved,Path('/unused'),{},'vela')

    def test_runtime_unsupported_needs_declared_receipt_not_silent_omission(self):
        saved={'corpus_inventory':inventory(),'corpus_uid':1001,'corpus_availability':{case:{'status':'unsupported','reason':'fixed-font-unavailable',
            'receipt':{'schema':1,'event':'corpus-unsupported','case':case,'reason':'fixed-font-unavailable'}} for case in corpus.CASES},'scenarios':{}}
        self.assertEqual(corpus.audit(saved,Path('/unused'),{},'vela'),[])
        saved['corpus_availability']['qt-wayland']['reason']='crashed'
        with self.assertRaises(BenchError):corpus.audit(saved,Path('/unused'),{},'vela')

    def test_tool_path_content_and_binary_digest_cannot_be_supplied_by_descriptor(self):
        for key,value in [('tool','/tmp/other'),('binary_sha256','unknown')]:
            inv=inventory();inv['qt-wayland'][key]=value
            with self.assertRaises(BenchError):corpus.validate_inventory(inv)
        inv=inventory();inv['qt-wayland']['description']['content_sha256']='c'*64
        with self.assertRaises(BenchError):corpus.validate_inventory(inv)

    def test_matrix_cannot_be_replaced_by_supported_label(self):
        saved={'corpus_inventory':inventory(),'corpus_uid':1001,'corpus_availability':{
            case:{'status':'supported','cases':25} for case in corpus.CASES},'scenarios':{}}
        with self.assertRaises((BenchError,KeyError)):corpus.audit(saved,Path('/unused'),{},'vela')

    def test_crop_pixels_and_initial_geometry_are_bound_to_source_not_only_hashes(self):
        case='qt-wayland';meta=source(case);item=inventory()[case]
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp);w,h,rgb=image();stem='vela-corpus-qt-wayland-100-opened'
            encoded=png_bytes(w,h,rgb);(directory/(stem+'.png')).write_bytes(encoded)
            metrics,crop=corpus.image_metrics(directory/(stem+'.png'),meta,case,{'width':w,'height':h},1.)
            crop_bytes=png_bytes(*crop);(directory/(stem+'-crop.png')).write_bytes(crop_bytes)
            row={'case':case,'scale':1.,'state':'opened','initial_canvas':[100,80],'outputs':[],
                'source':meta,'source_events':[meta],'injections':[],
                'pointer_parking':{'event':'injected','command':'point 65535 65535','before_monotonic_ns':10,'after_monotonic_ns':11},
                'capture':{'event':'capture','backend':'ext-image-copy-capture-v1','index':0,'width':w,'height':h,
                           'begin_monotonic_ns':30,'end_monotonic_ns':31},
                'image_file':stem+'.png','image_sha256':hashlib.sha256(encoded).hexdigest(),
                'crop_file':stem+'-crop.png','crop_sha256':hashlib.sha256(crop_bytes).hexdigest()}
            profile={'outputs':[],'mode':{'width':w,'height':h}}
            self.assertEqual(corpus.row_metrics(row,directory,profile,'vela',item,1001),metrics)
            row['initial_canvas']=[99,80]
            with self.assertRaisesRegex(BenchError,'initial size'):corpus.row_metrics(row,directory,profile,'vela',item,1001)
            row['initial_canvas']=[100,80]
            pixels=bytearray(crop[2]);pixels[0]^=1;changed=png_bytes(crop[0],crop[1],pixels)
            (directory/row['crop_file']).write_bytes(changed);row['crop_sha256']=hashlib.sha256(changed).hexdigest()
            with self.assertRaisesRegex(BenchError,'crop resampled'):corpus.row_metrics(row,directory,profile,'vela',item,1001)

    def test_png_crop_limits_and_crc_are_checked(self):
        with self.assertRaises(BenchError):png_bytes(9000,1,b'')
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'bad.png';raw=bytearray(png_bytes(1,1,b'abc'));raw[-1]^=1;p.write_bytes(raw)
            with self.assertRaises(BenchError):png_rgb(p)


class CorpusCampaign(unittest.TestCase):
    """Binding tests use a stub raw auditor; pixel audits are tested separately."""
    def setUp(self):
        from test_campaign import CalibrationCampaign
        CalibrationCampaign.setUp(self)
        self.fail_plasma=False;self.context['uid']=1001
        self.setup['files'][str(self.layout.helper.parent/'vela-pattern')]='fixture'
        self.inventory=inventory(False);self.tamper=False
        patch('vela_bench.corpus.describe_inventory',side_effect=lambda *args:copy.deepcopy(self.inventory)).start()
        patch('vela_bench.campaign.audit_pilot',return_value={'status':'passed','checks':{'restored':True}}).start()
        from vela_bench.archive import read_json
        patch('vela_bench.workloads.audit_measurements',side_effect=lambda directory,target,*args:read_json(directory/f'{target}-measurements.json')).start()

    def fake_begin(self,host,targets,**options):
        from test_campaign import CalibrationCampaign
        from vela_bench.archive import atomic_json,read_json
        from vela_bench.campaign import block_request
        from vela_bench.platform_workloads import VERSION,SPEC
        CalibrationCampaign.fake_begin(self,host,targets,**options)
        directory=self.layout.base/'pilots'/options['run_id'];target=targets[0]
        request=block_request(self.layout,options['campaign_run_id'],options['campaign_block_id'],target)
        state=read_json(directory/'state.json');state['campaign_block']=request;atomic_json(directory/'state.json',state)
        atomic_json(directory/'block-workload.json',dict(request,status='passed'))
        data={'schema':1,'method':VERSION,'spec':SPEC,'phase':'scaling','status':'passed','family_complete':False,
              'scenarios':{'corpus-fixture':{'image_file':'fixture.png','crop_file':'fixture-crop.png',
                  'metrics':{'span_error_pixels':1.,'offscreen_pixels':0}}},'corpus_inventory':request['corpus_inventory'],'corpus_uid':1001,'corpus_availability':{}}
        if self.tamper:data['corpus_inventory']={}
        atomic_json(directory/f'{target}-measurements.json',data)
        return 0

    def run_campaign(self,**options):
        from test_measurements import MeasurementCampaign
        return MeasurementCampaign.run_campaign(self,**dict(phases=['scaling'],**options))

    def test_inventory_is_frozen_in_plan_worker_receipt_and_hashed_export(self):
        from vela_bench.campaign import latest,export_campaign,read_export
        self.assertEqual(self.run_campaign(),0)
        data=latest(self.layout)
        self.assertTrue(all(b['audit']['checks']['corpus_inventory_binding'] for b in data['blocks']))
        output=export_campaign(self.layout.path('exports'),data,self.layout)
        self.assertEqual(read_export(output),data)
        plan=json.loads((output/'campaign-plan.json').read_text())
        self.assertEqual(plan['context']['corpus_inventory'],self.inventory)
        self.assertFalse(data['full_suite_complete'])

    def test_valid_measurement_cannot_replace_frozen_tools(self):
        self.tamper=True
        self.assertEqual(self.run_campaign(),1)
        from vela_bench.campaign import latest
        data=latest(self.layout)
        self.assertFalse(data['blocks'][0]['audit']['checks']['corpus_inventory_binding'])

    def test_changed_corpus_prevents_resume_before_another_session(self):
        from vela_bench.campaign import latest
        self.assertEqual(self.run_campaign(),0)
        run=latest(self.layout)['run_id'];before=list(self.calls)
        self.inventory=inventory()
        with self.assertRaises(BenchError):self.run_campaign(resume=run)
        self.assertEqual(self.calls,before)

if __name__=='__main__':unittest.main()
