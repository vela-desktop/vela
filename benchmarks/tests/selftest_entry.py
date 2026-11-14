# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Allow bounded test cancellation to run Python context-manager cleanup."""
import runpy
import signal
import sys


def terminate(*_):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise SystemExit(143)


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminate)
    sys.argv = sys.argv[1:]
    if sys.argv[0] == '-m':
        module = sys.argv[1]
        sys.argv = sys.argv[1:]
        runpy.run_module(module, run_name='__main__', alter_sys=True)
    else:
        runpy.run_path(sys.argv[0], run_name='__main__')
