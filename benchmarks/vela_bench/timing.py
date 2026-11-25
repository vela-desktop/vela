# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Boot-local runner timing collected outside the instrument hot paths."""

from contextlib import contextmanager
from functools import cache
from pathlib import Path
import time


@cache
def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


class Timeline:
    def __init__(self):
        self.begin_ns = time.monotonic_ns()
        self.boot = boot_id()
        self.stages = []

    @contextmanager
    def stage(self, name, **fields):
        record = {'stage': name, 'begin_ns': time.monotonic_ns(), **fields}
        cpu = time.process_time_ns()
        self.stages.append(record)
        try:
            yield record
        except KeyboardInterrupt:
            record['status'] = 'interrupted'
            raise
        except BaseException:
            record['status'] = 'failed'
            raise
        else:
            record.setdefault('status', 'finished')
        finally:
            record['end_ns'] = time.monotonic_ns()
            record['cpu_ns'] = time.process_time_ns() - cpu

    def result(self, *, finished=True):
        end_ns = time.monotonic_ns() if finished else None
        result = {'schema': 1, 'clock': 'CLOCK_MONOTONIC', 'boot_id': self.boot,
                  'cpu_scope': 'current-process', 'begin_ns': self.begin_ns, 'end_ns': end_ns,
                  'stages': list(self.stages)}
        if end_ns is not None:
            covered_ns = 0
            previous_end = self.begin_ns
            for stage in self.stages:
                if stage['end_ns'] > previous_end:
                    covered_ns += stage['end_ns'] - max(stage['begin_ns'], previous_end)
                    previous_end = stage['end_ns']
            result['elapsed_ns'] = end_ns - self.begin_ns
            result['unattributed_ns'] = result['elapsed_ns'] - covered_ns
        return result
