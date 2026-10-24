# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Desktop benchmark orchestration, independent of the desktop under test."""

VERSION = "0.1.0-dev"
SCHEMA = 1


class BenchError(Exception):
    """An actionable configuration, evidence or execution error."""
