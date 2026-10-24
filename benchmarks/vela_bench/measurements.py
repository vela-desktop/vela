# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Versioned P2 neutral-client measurements. Shell/window scenarios remain separate."""

from concurrent.futures import ThreadPoolExecutor
import json
import statistics
import subprocess
import time
from pathlib import Path

from . import BenchError
from .archive import atomic_json, decode_json, read_json
from .calibration import Lines, parse_input, run_input, stop_process
from .diagnostics import distribution, parse_probe, stop_instrument, user_command

VERSION = 'p2-neutral-client-v1'
PHASES = ('responsiveness', 'motion', 'slow-apps')
INPUT_WARMUP, INPUT_SAMPLES = 5, 100
MOTION_WARMUP, MOTION_SAMPLES = 30, 600
SPEC = {'version': VERSION, 'input_warmup_actions': INPUT_WARMUP, 'input_actions': INPUT_SAMPLES,
        'motion_warmup_frames': MOTION_WARMUP, 'motion_frames': MOTION_SAMPLES,
        'producer_delay_ms': 150, 'producer_sync_modes': ['explicit', 'implicit'],
        'neutral_buffer_size': [640, 360], 'producer_buffer_size': [1024, 640],
        'producer_controls': 'fresh-no-load-control-before-each-sync-mode',
        'scope': 'native-neutral-client-only', 'shell_response': False, 'window_movement': False,
        'cpu_contention_workload': False, 'gpu_contention_workload': False,
        'gpu_producer_contention_excluded': False}


def records(path):
    raw = Path(path).read_text()
    if len(raw.encode()) > 2 * 1024 * 1024 or not raw.endswith('\n'):
        raise BenchError('Measurement stream is oversized or truncated')
    rows = [decode_json(line) for line in raw.splitlines()]
    if any(not isinstance(row, dict) for row in rows):
        raise BenchError('Invalid measurement record')
    return raw, rows


def native(rows):
    meta = [row for row in rows if row.get('event') == 'probe']
    if len(meta) != 1 or any(meta[0].get(k) != v for k, v in
            {'display_protocol': 'wayland', 'window_protocol': 'xdg-shell', 'buffer_backend': 'wl_shm',
             'buffer_width': 640, 'buffer_height': 360}.items()):
        raise BenchError('Measurement requires the declared native Wayland SHM client')


def input_metrics(rows, injections):
    native(rows)
    parse_input(rows, injections, count=INPUT_WARMUP + INPUT_SAMPLES)
    inputs = {row['action_id']: row for row in rows if row.get('event') == 'input'}
    commits = {row['input_id']: row for row in rows if row.get('event') == 'commit'}
    feedback = {row['frame']: row for row in rows if row.get('event') == 'presented'}
    upper, lower, receipt, client, uncertainty = [], [], [], [], []
    for action in range(INPUT_WARMUP + 1, INPUT_WARMUP + INPUT_SAMPLES + 1):
        injection = injections[action - 1]
        shown = feedback[commits[action]['frame']]
        stamp = shown['presentation_ns']
        upper.append(stamp - injection['before_monotonic_ns'])
        lower.append(max(0, stamp - injection['after_monotonic_ns']))
        receipt.append(inputs[action]['received_monotonic_ns'] - injection['before_monotonic_ns'])
        client.append(stamp - inputs[action]['received_monotonic_ns'])
        uncertainty.append(injection['after_monotonic_ns'] - injection['before_monotonic_ns'])
    return {'method': VERSION, 'scope': 'software-input-to-neutral-client-presentation',
            'warmup_actions': INPUT_WARMUP, 'actions': INPUT_SAMPLES, 'failed_actions': 0,
            'injection_to_presentation_upper_ms': distribution(upper),
            'injection_to_presentation_lower_ms': distribution(lower),
            'injection_to_client_receipt_ms': distribution(receipt),
            'client_receipt_to_presentation_ms': distribution(client),
            'injection_interval_ms': distribution(uncertainty),
            'window_monotonic_ns': [injections[INPUT_WARMUP]['before_monotonic_ns'],
                                    feedback[commits[INPUT_WARMUP + INPUT_SAMPLES]['frame']]['received_monotonic_ns']],
            'optical_latency': False, 'p99_available': False}


def motion_metrics(raw, rows, profile, *, physical_output_required=True):
    native(rows)
    parse_probe(raw, frames=MOTION_WARMUP + MOTION_SAMPLES)
    shown = [row for row in rows if row.get('event') == 'presented' and row['frame'] > MOTION_WARMUP]
    discarded = [row for row in rows if row.get('event') == 'discarded' and row['frame'] > MOTION_WARMUP]
    if len(shown) < 2:
        raise BenchError('Insufficient measured presentation feedback')
    period = 1_000_000_000_000 / profile['mode']['refresh_mhz']
    physical = not any(not row['flags'] & 1 or not row['refresh_ns'] or abs(row['refresh_ns'] - period) > .03 * period for row in shown)
    if physical_output_required and not physical:
        raise BenchError('Presentation feedback does not verify the declared fixed refresh')
    gaps = [b['presentation_ns'] - a['presentation_ns'] for a, b in zip(shown, shown[1:])]
    commits = [r for r in rows if r.get('event') == 'commit' and r['frame'] > MOTION_WARMUP]
    return {'method': VERSION, 'scope': 'neutral-client-active-animation',
            'warmup_frames': MOTION_WARMUP, 'submitted': MOTION_SAMPLES,
            'presented': len(shown), 'discarded': len(discarded),
            'presentation_intervals_ms': distribution(gaps),
            'submission_to_presentation_ms': distribution([r['presentation_ns'] - r['commit_clock_ns'] for r in shown]),
            'gaps_over_1_5_refresh_periods': sum(gap > 1.5 * period for gap in gaps),
            'refresh_period_ns': period, 'missed_kernel_vblanks': None,
            'window_monotonic_ns': [commits[0]['monotonic_ns'], shown[-1]['received_monotonic_ns']],
            'p99_available': False, 'desktop_motion': False, 'physical_output_verified': physical}


def producer_metrics(rows, mode, window, *, physical_output_required=True):
    if not rows or rows[0].get('event') != 'producer' or rows[-1].get('event') != 'producer-summary':
        raise BenchError('GPU producer stream is incomplete')
    meta, summary = rows[0], rows[-1]
    if (meta.get('method') != 'delayed-gpu-acquire-v1' or meta.get('schema') != 1
            or meta.get('sync') != mode or meta.get('requested_delay_ms') != 150
            or meta.get('buffer_width') != 1024 or meta.get('buffer_height') != 640
            or meta.get('contention_excluded') is not False or summary.get('complete') is not True):
        raise BenchError('GPU producer condition differs from the declared workload')
    for key in ('vendor_id', 'device_id', 'drm_major', 'drm_minor', 'queue_family', 'timestamp_valid_bits'):
        if type(meta.get(key)) is not int or meta[key] < 0:
            raise BenchError('Missing GPU/device/timestamp identity')
    if (not 1 <= meta['timestamp_valid_bits'] <= 64 or type(meta.get('timestamp_period_ns')) not in (int, float)
            or not meta['timestamp_period_ns'] > 0):
        raise BenchError('Unsupported GPU timestamp domain')
    commits = [r for r in rows if r.get('event') == 'producer-commit']
    done = [r for r in rows if r.get('event') == 'gpu-complete']
    visible = [r for r in rows if r.get('event') == 'producer-presented']
    discarded = [r for r in rows if r.get('event') == 'producer-discarded']
    if (len(commits) < 5 or len(done) != len(commits) or summary.get('submitted') != len(commits)
            or summary.get('completed') != len(done)
            or len(visible) < 5 or summary.get('presented') != len(visible) or summary.get('discarded') != len(discarded)
            or any(r.get('event') not in ('producer', 'calibrated', 'producer-commit', 'gpu-complete', 'producer-summary', 'producer-presented', 'producer-discarded') for r in rows)
            or len([r for r in rows if r.get('event') == 'producer']) != 1
            or len([r for r in rows if r.get('event') == 'calibrated']) != 1
            or len([r for r in rows if r.get('event') == 'producer-summary']) != 1):
        raise BenchError('GPU producer did not establish continued buffer production')
    for index, (commit, completed) in enumerate(zip(commits, done), 1):
        if (type(commit.get('frame')) is not int or commit['frame'] != index
                or type(completed.get('frame')) is not int or completed['frame'] != index
                or commit.get('fence_pending') is not True
                or any(type(r.get(key)) is not int or r[key] < 0 for r, key in
                       ((commit, 'monotonic_ns'), (completed, 'submitted_monotonic_ns'), (completed, 'observed_monotonic_ns')))
                or not completed['submitted_monotonic_ns'] <= commit['monotonic_ns'] <= completed['observed_monotonic_ns']
                or type(completed.get('gpu_duration_ms')) not in (int, float)
                or not 0 < completed['gpu_duration_ms'] < 5000
                or completed['gpu_duration_ms'] * 1_000_000 > completed['observed_monotonic_ns'] - completed['submitted_monotonic_ns'] + 1_000_000):
            raise BenchError('GPU job/fence/clock evidence is invalid')
    stamps = [r['monotonic_ns'] for r in commits]
    feedback_ids = [r.get('frame') for r in visible + discarded]
    if (any(type(frame) is not int or not 1 <= frame <= len(commits) for frame in feedback_ids)
            or len(set(feedback_ids)) != len(feedback_ids)
            or any(type(r.get('received_monotonic_ns')) is not int or r['received_monotonic_ns'] < 0 for r in visible)
            or any(type(r.get('flags')) is not int or (physical_output_required and not r['flags'] & 1) for r in visible)):
        raise BenchError('GPU producer visibility evidence is invalid')
    visibility = [r['received_monotonic_ns'] for r in visible]
    if (visibility[0] > window[0] or visibility[-1] < window[1] - 2_000_000_000
            or any(b <= a or b-a > 2_000_000_000 for a,b in zip(visibility,visibility[1:]))):
        raise BenchError('GPU producer did not remain visibly updating alongside the neutral client')
    if (any(b <= a or b-a > 2_000_000_000 for a,b in zip(stamps, stamps[1:]))
            or stamps[0] > window[0] or stamps[-1] < window[1] - 2_000_000_000
            or type(summary.get('monotonic_ns')) is not int or summary['monotonic_ns'] < window[1]):
        raise BenchError('GPU producer did not cover the measurement window continuously')
    duration = [r['gpu_duration_ms'] * 1_000_000 for r in done]
    measured = distribution(duration)
    if not 75 <= measured['p50'] <= 225:
        raise BenchError('GPU timestamps did not establish the fixed 150 ms workload within 50% tolerance')
    return {'method': 'delayed-gpu-acquire-v1', 'sync': mode, 'device': meta,
            'submitted': len(commits), 'completed': len(done), 'gpu_duration_ms': measured,
            'presented': len(visible), 'discarded': len(discarded), 'visible_updates_verified': True,
            'pending_feedback_at_stop': len(commits) - len(visible) - len(discarded),
            'physical_output_verified': all(r['flags'] & 1 != 0 for r in visible),
            'max_commit_gap_ms': max(b-a for a,b in zip(stamps, stamps[1:])) / 1_000_000,
            'measurement_window_covered': True, 'contention_excluded': False}


def run_motion(host, record, directory, target, layout, profile, label):
    prefix = directory / f'{target}-measure-{label}-motion'
    command = user_command(record, layout, 'measurement', [str(layout.probe), '--frames',
        str(MOTION_WARMUP + MOTION_SAMPLES), '--timeout', '55'], runtime_seconds=60)
    outcome = {'status': 'failed', 'scenario': label + '-motion'}
    try:
        result = host.run(command, check=False, timeout=65)
        prefix.with_suffix('.jsonl').write_text(result.stdout)
        prefix.with_suffix('.stderr').write_text(result.stderr)
        if result.returncode:
            raise BenchError(f'Animation client exited {result.returncode}')
        raw, rows = records(prefix.with_suffix('.jsonl'))
        outcome.update(status='passed', metrics=motion_metrics(raw, rows, profile))
    except (BenchError, OSError, ValueError) as exc:
        outcome['error'] = str(exc)
    finally:
        stop_instrument(host, record, layout, 'measurement')
        atomic_json(prefix.with_suffix('.json'), outcome)
    return outcome


def input_result(directory, target, label):
    prefix = directory / (target + '-measure-' + label + 'input')
    saved = read_json(prefix.with_suffix('.json'))
    if saved.get('status') != 'passed' or saved.get('uinput_cleanup') != 'passed':
        raise BenchError('Input measurement or uinput cleanup failed')
    _, rows = records(prefix.with_suffix('.jsonl'))
    return input_metrics(rows, read_json(prefix.with_suffix('.injections.json')))


def paired_metrics(control, loaded):
    return {'input_p50_upper_delta_ms': loaded['input']['injection_to_presentation_upper_ms']['p50'] - control['input']['injection_to_presentation_upper_ms']['p50'],
            'input_p95_upper_delta_ms': loaded['input']['injection_to_presentation_upper_ms']['p95'] - control['input']['injection_to_presentation_upper_ms']['p95'],
            'animation_p95_interval_delta_ms': loaded['motion']['presentation_intervals_ms']['p95'] - control['motion']['presentation_intervals_ms']['p95'],
            'animation_gap_count_delta': loaded['motion']['gaps_over_1_5_refresh_periods'] - control['motion']['gaps_over_1_5_refresh_periods'],
            'scope': 'same-block-neutral-client-paired-control', 'causal_isolation_from_gpu_contention': False}


def run_workload(host, record, directory, target, layout, profile, phase, health, progress=None):
    if phase not in PHASES:
        raise BenchError('Measurement phase is not implemented')
    result = {'schema': 1, 'method': VERSION, 'phase': phase, 'spec': SPEC, 'status': 'failed',
              'scenarios': {}, 'failures': [], 'family_complete': False}
    def measure(label, do_input=True, do_motion=True):
        sample = {}
        if do_input:
            if progress:
                progress((label or 'neutral-') + 'input')
            health()
            run_input(host, record, directory, target, layout, health, measurement_name='measure-' + label + 'input')
            sample['input'] = input_result(directory, target, label)
        if do_motion:
            if progress:
                progress((label or 'neutral-') + 'motion')
            health()
            outcome = run_motion(host, record, directory, target, layout, profile, label.rstrip('-') or 'neutral')
            if outcome['status'] != 'passed':
                raise BenchError(outcome.get('error', 'Motion measurement failed'))
            sample['motion'] = outcome['metrics']
        health()
        return sample
    try:
        if phase == 'responsiveness':
            result['scenarios']['neutral'] = measure('', do_motion=False)
        elif phase == 'motion':
            result['scenarios']['neutral'] = measure('', do_input=False)
        else:
            for mode in ('explicit', 'implicit'):
                control_name = mode + '-control'
                result['scenarios'][control_name] = measure(control_name + '-')
                prefix = directory / f'{target}-producer-{mode}'
                process = None
                with prefix.with_suffix('.stderr').open('wb') as errors, ThreadPoolExecutor(max_workers=1) as pool:
                    lines = None
                    try:
                        if progress:
                            progress(mode + '-producer-start')
                        command = user_command(record, layout, 'producer', [str(layout.helper.parent / 'vela-slowgpu'),
                            '150', '--json', '--until-stdin-eof', *(['--implicit'] if mode == 'implicit' else [])], runtime_seconds=120)
                        process = host.spawn(command, errors)
                        lines = Lines(process)
                        deadline = time.monotonic() + 112
                        while lines.next(deadline).get('event') != 'producer-presented':
                            pass
                        def drain():
                            while lines.next(deadline).get('event') != 'producer-summary':
                                pass
                        future = pool.submit(drain)
                        sample = measure(mode + '-')
                        process.stdin.close()
                        future.result(timeout=12)
                        if process.wait(timeout=5):
                            raise BenchError('GPU producer exited unsuccessfully')
                        window = [sample['input']['window_monotonic_ns'][0], sample['motion']['window_monotonic_ns'][1]]
                        sample['producer'] = producer_metrics(lines.records, mode, window)
                        if mode == 'implicit':
                            explicit = result['scenarios']['explicit']['producer']['device']
                            if any(sample['producer']['device'][key] != explicit[key] for key in ('vendor_id','device_id','drm_major','drm_minor')):
                                raise BenchError('GPU producer changed device between synchronization modes')
                        sample['degradation'] = paired_metrics(result['scenarios'][control_name], sample)
                        result['scenarios'][mode] = sample
                    finally:
                        stop_process(process)
                        stop_instrument(host, record, layout, 'producer')
                        if lines:
                            prefix.with_suffix('.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in lines.records))
        result['status'] = 'passed'
    except (BenchError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        result['failures'].append(str(exc))
        if 'prefix' in locals() and prefix.with_suffix('.stderr').is_file():
            detail = prefix.with_suffix('.stderr').read_text(errors='replace')[-8192:].strip()
            if detail:
                result['failures'].append(detail)
    finally:
        atomic_json(directory / f'{target}-measurements.json', result)
    return result


def audit_measurements(directory, target, profile, phase):
    saved = read_json(directory / f'{target}-measurements.json')
    if saved.get('status') != 'passed' or saved.get('spec') != SPEC or saved.get('phase') != phase or saved.get('method') != VERSION or saved.get('failures') != []:
        raise BenchError('Measurement workload failed or its specification changed')
    names = ['neutral'] if phase != 'slow-apps' else ['explicit-control', 'explicit', 'implicit-control', 'implicit']
    actual = {}
    for name in names:
        label = '' if name == 'neutral' else name + '-'
        row = {}
        if phase != 'motion':
            row['input'] = input_result(directory, target, label)
        if phase != 'responsiveness':
            prefix = directory / f'{target}-measure-{name}-motion'
            outcome = read_json(prefix.with_suffix('.json'))
            raw, rows = records(prefix.with_suffix('.jsonl'))
            row['motion'] = motion_metrics(raw, rows, profile)
            if outcome.get('status') != 'passed' or outcome.get('metrics') != row['motion']:
                raise BenchError('Motion summary differs from raw evidence')
        if name in ('explicit', 'implicit'):
            _, rows = records(directory / f'{target}-producer-{name}.jsonl')
            window = [row['input']['window_monotonic_ns'][0], row['motion']['window_monotonic_ns'][1]]
            row['producer'] = producer_metrics(rows, name, window)
            row['degradation'] = paired_metrics(actual[name + '-control'], row)
            if name == 'implicit':
                explicit = actual['explicit']['producer']['device']
                if any(row['producer']['device'][key] != explicit[key] for key in ('vendor_id','device_id','drm_major','drm_minor')):
                    raise BenchError('GPU device differs between synchronization conditions')
        actual[name] = row
    if saved.get('scenarios') != actual or saved.get('family_complete') is not False:
        raise BenchError('Measurement summary differs from raw evidence or overstates coverage')
    return saved


def aggregate(blocks):
    """Keep independent block summaries separate; never pool actions as repetitions."""
    groups = {}
    for block in blocks:
        data = block.get('audit', {}).get('measurements')
        if block.get('status') != 'complete' or not data:
            continue
        for scenario, sample in data['scenarios'].items():
            key = (block['phase'], block['target'], scenario)
            group = groups.setdefault(key, {'phase': key[0], 'target': key[1], 'scenario': key[2], 'blocks': []})
            group['blocks'].append({'repetition': block['repetition'], **sample})
    result = list(groups.values())
    for group in result:
        group['independent_blocks'] = len(group['blocks'])
        group['aggregation'] = 'median-and-range-of-independent-block-statistics; no pooled quantiles or significance claim'
        group['block_statistics'] = {}
        for scope, metric in (('input', 'injection_to_presentation_upper_ms'), ('motion', 'presentation_intervals_ms')):
            for quantile in ('p50', 'p95'):
                values = [b[scope][metric][quantile] for b in group['blocks'] if scope in b]
                if values:
                    group['block_statistics'][scope + '/' + quantile] = {'median': statistics.median(values), 'min': min(values), 'max': max(values), 'unit': 'ms'}
        for metric in ('input_p50_upper_delta_ms', 'input_p95_upper_delta_ms', 'animation_p95_interval_delta_ms', 'animation_gap_count_delta'):
            values = [b['degradation'][metric] for b in group['blocks'] if 'degradation' in b]
            if values:
                group['block_statistics']['paired/' + metric] = {'median': statistics.median(values), 'min': min(values),
                    'max': max(values), 'unit': 'count' if metric.endswith('count_delta') else 'ms'}
    return result
