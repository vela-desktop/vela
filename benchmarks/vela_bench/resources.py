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


def transport_identity(process, command, *, proc=Path('/proc')):
    """Register only our actual launch client, before sampling its target UID.

    setpriv changes the client's credentials, not its inherited cgroup. The
    real workload is separately started inside the target user manager.
    """
    if process.args!=command or command.count('/usr/bin/systemd-run')!=1:
        raise BenchError('Resource transport command is not the owned launch request')
    if process.poll() is not None:return None
    path=proc/str(process.pid)
    try:
        ticks,start=process_stat((path/'stat').read_text())
        parent=int(next(line.split()[1] for line in (path/'status').read_text().splitlines() if line.startswith('PPid:')))
        if parent!=os.getpid():raise BenchError('Resource transport is not a direct controller child')
        membership=(proc/str(parent)/'cgroup').read_text().strip()
        if (path/'cgroup').read_text().strip()!=membership:
            raise BenchError('Resource transport left its controller cgroup')
        return {'pid':process.pid,'start_ticks':start,'initial_cpu_ticks':ticks,'parent_pid':parent,
                'cgroup':membership,'command':command[command.index('/usr/bin/systemd-run'):]}
    except (FileNotFoundError,ProcessLookupError):
        if process.poll() is not None:return None
        raise BenchError('Resource transport identity disappeared before registration')


def snapshot(uid, *, proc=Path('/proc'), cgroup=Path('/sys/fs/cgroup'), transport=None):
    if type(uid) is not int or uid <= 0: raise BenchError('Resource inventory requires a reserved non-root UID')
    begin = time.monotonic_ns()
    rows, vanished, transports = [], [], []
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
            is_transport=transport is not None and int(path.name)==transport['pid']
            if is_transport:
                command=path.joinpath('cmdline').read_bytes().removesuffix(b'\0').decode().split('\0')
                parent=int(values.get('PPid','0'))
                if (start!=transport['start_ticks'] or membership!=transport['cgroup']
                        or parent!=transport['parent_pid'] or command!=transport['command']):
                    raise BenchError(f'Resource transport identity changed: PID {path.name}, cgroup {membership}')
            elif len(membership) > 4096 or not membership.startswith('0::/user.slice/user-'+str(uid)+'.slice/'):
                raise BenchError(f'Reserved UID process escaped its session accounting boundary: '
                                 f'PID {path.name} ({values.get("Name","")}), start {start}, cgroup {membership}')
            again_ticks,again_start = process_stat(path.joinpath('stat').read_text())
            if again_start != start or again_ticks < ticks: raise BenchError('PID identity changed while sampling')
            row={'pid':int(path.name),'start_ticks':start,'cpu_ticks':again_ticks,
                         'pss_kib':int(pss[0][1]),'name':values.get('Name','')[:80],
                         'cgroup':membership,'classification':'benchmark-workload' if
                         any(unit in membership for unit in ('vela-benchmark-measurement.service','vela-benchmark-scene.service')) else 'session'}
            if is_transport:
                row.update(classification='collector-transport',parent_pid=parent,command=command)
                transports.append(row)
            else:rows.append(row)
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
            'processes':rows,'controller_transports':transports,'vanished_during_inventory':vanished,
            'collector_cpu_seconds':own.ru_utime+own.ru_stime}


def metrics(rows, uid, transport=None):
    if len(rows) < 2: raise BenchError('Resource window has fewer than two samples')
    previous = None
    process_counters = {}
    transport_rows=[]
    if transport is not None and (any(type(transport.get(k)) is not int or transport[k]<=0
            for k in ('pid','start_ticks','parent_pid')) or type(transport.get('initial_cpu_ticks')) is not int
            or transport['initial_cpu_ticks']<0 or not isinstance(transport.get('cgroup'),str)
            or not transport['cgroup'].startswith('0::/') or '\n' in transport['cgroup']
            or not isinstance(transport.get('command'),list) or not transport['command']
            or transport['command'][0]!='/usr/bin/systemd-run'):
        raise BenchError('Invalid controller transport identity')
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
        controls=row.get('controller_transports',[])
        if not isinstance(controls,list) or len(controls)>1:raise BenchError('Invalid controller transport inventory')
        for control in controls:
            if (transport is None or any(control.get(k)!=transport[k] for k in
                    ('pid','start_ticks','parent_pid','cgroup','command'))
                    or control.get('classification')!='collector-transport'
                    or any(type(control.get(k)) is not int or control[k]<0 for k in ('cpu_ticks','pss_kib'))
                    or control['cpu_ticks'] < (transport_rows[-1]['cpu_ticks'] if transport_rows else transport['initial_cpu_ticks'])):
                raise BenchError('Controller transport differs from its registered identity')
            transport_rows.append(control)
        for process in row['processes']:
            if (any(type(process.get(k)) is not int or process[k] < 0 for k in ('pid','start_ticks','cpu_ticks','pss_kib'))
                    or not process['pid'] or not isinstance(process.get('cgroup'),str)
                    or not process['cgroup'].startswith(f'0::/user.slice/user-{uid}.slice/')
                    or process.get('classification') not in ('session','benchmark-workload')):
                raise BenchError('Invalid process resource identity')
            expected = ('benchmark-workload' if any(unit in process['cgroup'] for unit in
                ('vela-benchmark-measurement.service', 'vela-benchmark-scene.service')) else 'session')
            if process['classification'] != expected:
                raise BenchError('Process classification differs from its cgroup')
            if transport and process['pid']==transport['pid']:
                raise BenchError('Controller transport was counted as a session process')
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
            'collector_cpu_scope':'root-controller-process-only; registered launch transport separately observed',
            'collector_transport_samples':len(transport_rows),
            'collector_transport_pss_mib_max':max((r['pss_kib']/1024 for r in transport_rows),default=None),
            'collector_transport_cpu_seconds_observed':((transport_rows[-1]['cpu_ticks']-transport['initial_cpu_ticks'])/rows[0]['clock_ticks_per_second']
                                                       if transport_rows else None),
            'collector_transport_cpu_scope':'observed-live-client-only; exit CPU may be missing; not subtracted',
            'collector_transport_in_uid_cpu_totals':(transport['cgroup'].startswith(f'0::/user.slice/user-{uid}.slice/')
                                                     if transport else None),
            'pss_scope':'observed-live-session-and-workload-PIDs; registered-controller-transport-separate; non-atomic inventory',
            'maximum_inventory_span_ms':max(row['end_monotonic_ns']-row['begin_monotonic_ns'] for row in rows)/1e6,
            'shared_system_services':'not-attributed','gpu':'unsupported','energy':'unsupported',
            'capture_enabled':False,'estimated_collector_cost_subtracted':False}
