# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unprivileged development entry for the production capture worker."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench.calibration import CAPTURES, capture_worker
from vela_bench.host import Layout

if __name__ == '__main__':
    capture_worker(sys.argv[2], sys.argv[3], Layout(Path(sys.argv[1])), count=1 if sys.argv[4:] == ['--one'] else CAPTURES)
