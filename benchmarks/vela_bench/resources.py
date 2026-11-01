# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""UID-wide Linux process inventory, cgroup CPU counters and fixed local work."""
import hashlib
import math
import os
from pathlib import Path
import resource
import time

from . import BenchError

HASH_CHUNKS = 16384
HASH_DATA = bytes(range(256))*4096


def hash_client():
    began = time.monotonic_ns()
    digest = None
    for _ in range(HASH_CHUNKS): digest = hashlib.sha256(HASH_DATA).hexdigest()
    return {'event':'local-work-complete','method':'fixed-sha256-corpus-v1',
            'chunks':HASH_CHUNKS,'bytes':len(HASH_DATA)*HASH_CHUNKS,'digest':digest,
            'begin_monotonic_ns':began,'end_monotonic_ns':time.monotonic_ns()}


def cpu_stat(path):
    values = dict(line.split() for line in path.read_text().splitlines())
    value = values.get('usage_usec')
    if value is None or not value.isdigit(): raise BenchError('UID cgroup CPU counter unavailable')
    return int(value)


def process_stat(raw):
    # comm can contain spaces and parentheses. Fields after its final ')' start
    # at state (3); utime/stime/starttime are fields 14/15/22.
    end = raw.rfind(')')
    fields = raw[end+2:].split()
    if end < 0 or len(fields) < 20: raise BenchError('Truncated process stat')
    return int(fields[11])+int(fields[12]), int(fields[19])


def snapshot(uid, *, proc=Path('/proc'), cgroup=Path('/sys/fs/cgroup')):
    if type(uid) is not int or uid <= 0: raise BenchError('Resource inventory requires a reserved non-root UID')
    begin = time.monotonic_ns()
    rows, vanished = [], []
    for path in sorted(proc.iterdir(), key=lambda p:p.name):
        if not path.name.isdigit(): continue
        try:
            status = path.joinpath('status').read_text()
            values = {line.split(':',1)[0]:line.split(':',1)[1].strip() for line in status.splitlines() if ':' in line}
            uids = [int(v) for v in values.get('Uid','').split()]
            if uid not in uids: continue
            if uids != [uid]*4: raise BenchError('Reserved UID process changed credentials')
            ticks,start = process_stat(path.joinpath('stat').read_text())
            rollup = path.joinpath('smaps_rollup').read_text()
            pss = [line.split() for line in rollup.splitlines() if line.startswith('Pss:')]
            if len(pss) != 1 or pss[0][2:] != ['kB'] or not pss[0][1].isdigit():
                raise BenchError('Process PSS accounting unavailable')
            membership = path.joinpath('cgroup').read_text().strip()
            if len(membership) > 4096 or not membership.startswith('0::/user.slice/user-'+str(uid)+'.slice/'):
                raise BenchError('Reserved UID process escaped its session accounting boundary')
            again_ticks,again_start = process_stat(path.joinpath('stat').read_text())
            if again_start != start or again_ticks < ticks: raise BenchError('PID identity changed while sampling')
            rows.append({'pid':int(path.name),'start_ticks':start,'cpu_ticks':again_ticks,
                         'pss_kib':int(pss[0][1]),'name':values.get('Name','')[:80],
                         'cgroup':membership,'classification':'benchmark-workload' if
                         any(unit in membership for unit in ('vela-benchmark-measurement.service','vela-benchmark-scene.service')) else 'session'})
        except (FileNotFoundError, ProcessLookupError):
            # Exits are visible gaps in the point-in-time PSS inventory; cgroup
            # CPU still includes their work, unlike summing surviving PIDs.
            if 'uids' in locals() and uid in uids: vanished.append(int(path.name))
        except PermissionError as exc:
            if path.stat().st_uid == uid: raise BenchError('Reserved UID process accounting permission denied') from exc
        finally:
            uids = []
    if not rows: raise BenchError('No live reserved UID processes in resource inventory')
    cpu_begin = time.monotonic_ns()
    usage = cpu_stat(cgroup/'user.slice'/f'user-{uid}.slice'/'cpu.stat')
    cpu_end = time.monotonic_ns()
    own = resource.getrusage(resource.RUSAGE_SELF)
    return {'event':'resource-sample','begin_monotonic_ns':begin,'end_monotonic_ns':time.monotonic_ns(),
            'uid':uid,'clock_ticks_per_second':os.sysconf('SC_CLK_TCK'),
            'cpu_begin_monotonic_ns':cpu_begin,'cpu_end_monotonic_ns':cpu_end,'cpu_usage_usec':usage,
            'processes':rows,'vanished_during_inventory':vanished,
            'collector_cpu_seconds':own.ru_utime+own.ru_stime}


def metrics(rows, uid):
    if len(rows) < 2: raise BenchError('Resource window has fewer than two samples')
    previous = None
    process_counters = {}
    for row in rows:
        if (row.get('event') != 'resource-sample' or row.get('uid') != uid
                or any(type(row.get(k)) is not int or row[k] < 0 for k in
                       ('begin_monotonic_ns','end_monotonic_ns','cpu_begin_monotonic_ns','cpu_end_monotonic_ns','cpu_usage_usec'))
                or not row['begin_monotonic_ns'] <= row['cpu_begin_monotonic_ns'] <= row['cpu_end_monotonic_ns'] <= row['end_monotonic_ns']
                or not isinstance(row.get('processes'),list) or not row['processes']):
            raise BenchError('Invalid resource sample or clock')
        if (type(row.get('collector_cpu_seconds')) not in (int,float) or not math.isfinite(row['collector_cpu_seconds'])
                or row['collector_cpu_seconds']<0 or not isinstance(row.get('vanished_during_inventory'),list)
                or any(type(pid) is not int or pid<=0 for pid in row['vanished_during_inventory'])
                or type(row.get('clock_ticks_per_second')) is not int or row['clock_ticks_per_second']<=0):
            raise BenchError('Invalid collector or process accounting coverage')
        identities = set()
        for process in row['processes']:
            if (any(type(process.get(k)) is not int or process[k] < 0 for k in ('pid','start_ticks','cpu_ticks','pss_kib'))
                    or not process['pid'] or not isinstance(process.get('cgroup'),str)
                    or not process['cgroup'].startswith(f'0::/user.slice/user-{uid}.slice/')
                    or process.get('classification') not in ('session','benchmark-workload')):
                raise BenchError('Invalid process resource identity')
            key = (process['pid'],process['start_ticks'])
            if key in identities: raise BenchError('Duplicate process in resource inventory')
            if process['cpu_ticks'] < process_counters.get(key,0): raise BenchError('Live process CPU counter moved backwards')
            process_counters[key]=process['cpu_ticks']
            identities.add(key)
        if previous and (row['begin_monotonic_ns'] < previous['end_monotonic_ns']
                         or row['cpu_usage_usec'] < previous['cpu_usage_usec']
                         or row['collector_cpu_seconds'] < previous['collector_cpu_seconds']
                         or row['clock_ticks_per_second'] != previous['clock_ticks_per_second']
                         or row['begin_monotonic_ns']-previous['end_monotonic_ns'] > 5_000_000_000):
            raise BenchError('Resource CPU counter reset or sampling gap')
        previous=row
    first,last=rows[0],rows[-1]
    elapsed=(last['cpu_end_monotonic_ns']-first['cpu_end_monotonic_ns'])/1e9
    if elapsed <= 0: raise BenchError('Empty resource measurement interval')
    cpu=(last['cpu_usage_usec']-first['cpu_usage_usec'])/1e6
    pss=[sum(p['pss_kib'] for p in row['processes'])/1024 for row in rows]
    session_pss=[sum(p['pss_kib'] for p in row['processes'] if p['classification']=='session')/1024 for row in rows]
    import statistics
    return {'scope':'entire-reserved-uid-cgroup','samples':len(rows),'elapsed_seconds':elapsed,
            'cpu_seconds':cpu,'cpu_percent_one_logical_cpu':100*cpu/elapsed,
            'pss_mib_median':statistics.median(pss),'pss_mib_max':max(pss),
            'session_pss_mib_median':statistics.median(session_pss),
            'inventory_process_identities':len({(p['pid'],p['start_ticks']) for row in rows for p in row['processes']}),
            'vanished_during_inventory':sum(len(row['vanished_during_inventory']) for row in rows),
            'collector_cpu_seconds':last['collector_cpu_seconds']-first['collector_cpu_seconds'],
            'collector_cpu_scope':'root-controller-process-only; helpers remain in UID totals',
            'pss_scope':'observed-live-processes; non-atomic inventory intervals retained',
            'maximum_inventory_span_ms':max(row['end_monotonic_ns']-row['begin_monotonic_ns'] for row in rows)/1e6,
            'shared_system_services':'not-attributed','gpu':'unsupported','energy':'unsupported',
            'capture_enabled':False,'estimated_collector_cost_subtracted':False}
