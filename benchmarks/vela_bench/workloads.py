# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixed measurement implementations selected by phase, never by a file path."""

PHASES = ('responsiveness', 'motion', 'slow-apps', 'scaling', 'overhead')


def implementation(phase):
    if phase in ('scaling', 'overhead'):
        from . import platform_workloads
        return platform_workloads
    if phase in PHASES:
        from . import measurements
        return measurements
    from . import BenchError
    raise BenchError('Unknown fixed measurement phase')


def audit_measurements(directory, target, profile, phase):
    return implementation(phase).audit_measurements(directory, target, profile, phase)
