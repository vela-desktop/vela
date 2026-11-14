#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Choose safe, useful Ninja parallelism on a rented NVIDIA CI container.

Vast.ai allocates fractional CPU capacity and container memory. nproc can
report host affinity without accounting for CFS quota, and /proc/meminfo
can report host memory without the container's memory limit.
"""

import math
import os
from pathlib import Path
import sys

GIB = 1024 ** 3
MAX_JOBS = 16
RESERVED_MEMORY = 2 * GIB
MEMORY_PER_JOB = GIB


def read_int(path):
    try:
        return int(Path(path).read_text(encoding="ascii").strip())
    except (OSError, ValueError):
        return None


def cgroup_cpu_quota():
    try:
        quota, period = Path("/sys/fs/cgroup/cpu.max").read_text(encoding="ascii").split()[:2]
        if quota != "max" and int(period) > 0:
            return max(1, math.floor(int(quota) / int(period)))
    except (OSError, ValueError, IndexError):
        pass

    # Legacy cgroup v1 fallback.
    quota = read_int("/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
    period = read_int("/sys/fs/cgroup/cpu/cpu.cfs_period_us")
    if quota is not None and quota > 0 and period and period > 0:
        return max(1, quota // period)
    return None


def available_memory():
    host_available = None
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemAvailable:"):
                host_available = int(line.split()[1]) * 1024
                break
    except (OSError, ValueError, IndexError):
        pass

    limit = read_int("/sys/fs/cgroup/memory.max")
    usage = read_int("/sys/fs/cgroup/memory.current")
    if limit is None:
        limit = read_int("/sys/fs/cgroup/memory/memory.limit_in_bytes")
        usage = read_int("/sys/fs/cgroup/memory/memory.usage_in_bytes")
    # Huge sentinel values in cgroup v1 represent unlimited memory.
    cgroup_available = (
        max(0, limit - usage)
        if limit is not None and usage is not None and limit < (1 << 60)
        else None
    )
    if host_available is None:
        return cgroup_available
    if cgroup_available is None:
        return host_available
    return min(host_available, cgroup_available)


def recommended_jobs(cpus, memory_bytes):
    """Constrain build concurrency by CPU and *available* (not total) RAM."""
    jobs = max(1, min(MAX_JOBS, int(cpus)))
    if memory_bytes is not None:
        memory_jobs = max(1, (max(0, memory_bytes - RESERVED_MEMORY)) // MEMORY_PER_JOB)
        jobs = min(jobs, memory_jobs)
    return jobs


def main():
    try:
        cpus = len(os.sched_getaffinity(0))
    except AttributeError:
        cpus = os.cpu_count() or 1
    quota = cgroup_cpu_quota()
    if quota is not None:
        cpus = min(cpus, quota)
    memory = available_memory()
    jobs = recommended_jobs(cpus, memory)
    print(f"NVIDIA CI build: {cpus} usable CPUs, "
          f"{memory / GIB:.1f} GiB available RAM" if memory is not None
          else f"NVIDIA CI build: {cpus} usable CPUs, memory availability unknown",
          file=sys.stderr)
    print(jobs)


if __name__ == "__main__":
    main()
