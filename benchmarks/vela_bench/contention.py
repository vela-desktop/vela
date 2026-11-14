# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed CPU/GPU contention evidence, separate from acquire-fence producers."""
import re

from . import BenchError

SPEC={'cpu':{'method':'pinned-integer-contention-v1','worker_cap':32,'iterations_per_chunk':65536,
             'minimum_worker_cpu_fraction':.25},
      'gpu':{'method':'graphics-compute-contention-v1','workgroups':256,'local_size':64,
             'target_job_ms':5,'minimum_timestamp_span_fraction':.35},
      'sample_period_ns':500_000_000,'maximum_sample_gap_ns':2_000_000_000,
      'capture':False,'controls':'fresh-input-and-motion-no-load-before-each-load',
      'saturation_claimed':False}


def integer(row,key,minimum=0):
    value=row.get(key)
    if type(value) is not int or value<minimum:raise BenchError('Invalid contention integer: '+key)
    return value


def metadata(rows,kind,uid):
    if (not rows or rows[0].get('event')!=kind+'-load' or rows[0].get('schema')!=1
            or rows[0].get('method')!=SPEC[kind]['method'] or rows[0].get('clock')!='CLOCK_MONOTONIC'
            or rows[0].get('uid')!=uid or type(uid) is not int or uid<=0):
        raise BenchError('Contention load identity, UID or clock changed')
    meta=rows[0];integer(meta,'pid',1)
    if meta.get('sample_period_ns')!=SPEC['sample_period_ns']:raise BenchError('Contention sampling policy changed')
    if kind=='cpu':
        workers=meta.get('workers',[]);allowed=integer(meta,'allowed_cpus',1)
        if (allowed>65536 or meta.get('worker_cap')!=32 or meta.get('iterations_per_chunk')!=65536
                or len(workers)!=min(allowed,32) or len({w.get('pid') for w in workers})!=len(workers)
                or len({w.get('cpu') for w in workers})!=len(workers)):
            raise BenchError('CPU worker policy or identities changed')
        for index,worker in enumerate(workers):
            if worker.get('worker')!=index or integer(worker,'pid',1)==meta['pid']:raise BenchError('CPU worker identity changed')
            if integer(worker,'cpu')>=65536:raise BenchError('Unsupported CPU identity')
    else:
        for key in ('vendor_id','device_id','drm_major','drm_minor','queue_family'):integer(meta,key)
        if (meta.get('graphics_queue') is not True or any(meta.get(k)!=SPEC['gpu'][k] for k in ('workgroups','local_size','target_job_ms'))
                or not 1<=integer(meta,'timestamp_valid_bits',1)<=64
                or type(meta.get('timestamp_period_ns')) not in (int,float) or not 0<meta['timestamp_period_ns']<1e6):
            raise BenchError('GPU queue, timestamps or workload geometry changed')
    return meta


def ready(rows,kind,uid):
    meta=metadata(rows,kind,uid)
    if kind=='gpu':return any(row.get('event')=='gpu-load-sample' and row.get('jobs',0)>0 for row in rows)
    seen={row.get('worker') for row in rows if row.get('event')=='cpu-load-sample' and row.get('chunks',0)>0}
    return seen==set(range(len(meta['workers'])))


def bounds(rows,window,keys):
    if len(rows)<2:raise BenchError('Insufficient contention progress samples')
    previous=None
    for index,row in enumerate(rows,1):
        if row.get('sequence')!=index:raise BenchError('Lost/reordered contention samples')
        stamp=integer(row,'monotonic_ns',1)
        if previous is not None:
            gap=stamp-previous['monotonic_ns']
            if not 0<gap<=SPEC['maximum_sample_gap_ns']:raise BenchError('Contention progress gap')
            for key in keys:
                if integer(row,key)<previous[key]:raise BenchError('Contention counter reset')
        else:
            for key in keys:integer(row,key)
        previous=row
    if rows[0]['monotonic_ns']>window[0] or rows[-1]['monotonic_ns']<window[1]:
        raise BenchError('Contention did not cover the complete input/motion window')
    # Enclose the full measured interval; retain the sampling uncertainty.
    before=max((row for row in rows if row['monotonic_ns']<=window[0]),key=lambda r:r['monotonic_ns'])
    after=min((row for row in rows if row['monotonic_ns']>=window[1]),key=lambda r:r['monotonic_ns'])
    return before,after,(after['monotonic_ns']-before['monotonic_ns'])/1e9


def metrics(rows,kind,window,uid,*,capture=False):
    meta=metadata(rows,kind,uid)
    if (len(window)!=2 or any(type(v) is not int or v<0 for v in window) or window[1]<=window[0]
            or rows[-1].get('event')!=kind+'-load-summary' or rows[-1].get('complete') is not True
            or integer(rows[-1],'monotonic_ns')<window[1]):raise BenchError('Contention window or completion missing')
    summary=rows[-1]
    result={'method':SPEC[kind]['method'],'kind':kind,'uid':uid,'window_monotonic_ns':window,
            'window_covered':True,'saturation_claimed':False,'capture':capture,'source':meta}
    samples=[r for r in rows if r.get('event')==kind+'-load-sample']
    if any(integer(r,'monotonic_ns')>summary['monotonic_ns'] for r in samples):
        raise BenchError('Contention samples occur after completion')
    if kind=='cpu':
        if len(rows)!=len(samples)+2 or summary.get('workers_reaped')!=len(meta['workers']) or integer(summary,'end_request_monotonic_ns')<window[1]:
            raise BenchError('CPU worker cleanup or record set incomplete')
        workers=[]
        for identity in meta['workers']:
            stream=[r for r in samples if r.get('worker')==identity['worker']]
            if any(any(r.get(k)!=v for k,v in identity.items()) or not re.fullmatch('[0-9a-f]{16}',r.get('checksum','')) for r in stream):
                raise BenchError('CPU sample worker identity/checksum changed')
            before,after,elapsed=bounds(stream,window,('chunks','cpu_time_ns'))
            cpu=(after['cpu_time_ns']-before['cpu_time_ns'])/1e9;chunks=after['chunks']-before['chunks']
            if not chunks>0 or not SPEC['cpu']['minimum_worker_cpu_fraction']<=cpu/elapsed<=1.05:
                raise BenchError('CPU contention achieved load outside the fixed acceptance range')
            workers.append({'cpu':identity['cpu'],'pid':identity['pid'],'cpu_seconds':cpu,'chunks':chunks,
                            'sampled_elapsed_seconds':elapsed,'cpu_fraction':cpu/elapsed})
        if len(samples)!=sum(sum(r.get('worker')==w['worker'] for r in samples) for w in meta['workers']):
            raise BenchError('Unknown CPU load worker')
        result.update(workers=workers,worker_count=len(workers),cpu_seconds=sum(w['cpu_seconds'] for w in workers),
                      mean_worker_cpu_fraction=sum(w['cpu_fraction'] for w in workers)/len(workers))
    else:
        calibration=[r for r in rows if r.get('event')=='calibrated']
        if (len(calibration)!=1 or len(rows)!=len(samples)+3 or integer(calibration[0],'iterations',1)<1 or calibration[0].get('dispatches')!=1
                or type(calibration[0].get('wall_duration_ms')) not in (int,float) or not 0<calibration[0]['wall_duration_ms']<5000):
            raise BenchError('GPU contention calibration or records changed')
        before,after,elapsed=bounds(samples,window,('jobs','gpu_time_ns','last_submit_ns'))
        for row in samples:
            if integer(row,'last_submit_ns')>row['monotonic_ns']:raise BenchError('GPU submission clock changed')
        jobs=after['jobs']-before['jobs'];gpu=(after['gpu_time_ns']-before['gpu_time_ns'])/1e9
        if (jobs<5 or not SPEC['gpu']['minimum_timestamp_span_fraction']<=gpu/elapsed<=1.05
                or summary.get('submitted')!=samples[-1]['jobs'] or summary.get('completed')!=samples[-1]['jobs']):
            raise BenchError('GPU contention achieved work or completion outside acceptance')
        result.update(jobs=jobs,gpu_timestamp_seconds=gpu,sampled_elapsed_seconds=elapsed,
                      timestamp_span_fraction=gpu/elapsed,global_gpu_utilization=None,
                      mean_job_ms=gpu/jobs*1000,calibration=calibration[0])
    return result
