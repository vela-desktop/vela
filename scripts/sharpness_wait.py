#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Wait for sharpness screenshots to see a settled compositor frame.

The graphical test still checks a single screenshot bit for bit. This helper
waits for the mapped window's geometry and output frame counters to stop
changing after a shortcut, rather than assuming every animation takes 800 ms.
"""

import json
import os
import socket
import sys
import time


def read_state():
    display = os.environ["WAYLAND_DISPLAY"]
    runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    path = os.path.join(runtime, f"vela-{display}.sock")
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(1.0)
        connection.connect(path)
        connection.sendall(b"state\n")
        data = bytearray()
        while not data.endswith(b"\n"):
            part = connection.recv(65536)
            if not part:
                break
            data.extend(part)
    return json.loads(data)


def frame_signature(state):
    """State whose stability means no window move or frame is still pending."""
    outputs = state.get("outputs", ())
    windows = state.get("windows", ())
    if not outputs or not windows:
        return None
    return (
        tuple((o["name"], o["frames"]) for o in outputs),
        tuple((w["id"], w["x"], w["y"], w["w"], w["h"],
               w["maximized"], w["minimized"], tuple(w["snap"] or ()))
              for w in windows),
    )


def wait_for_settled(read=read_state, *, timeout=6.0, quiet=0.15,
                     poll=0.05, clock=time.monotonic, sleep=time.sleep):
    deadline = clock() + timeout
    previous = None
    unchanged_since = None
    while clock() < deadline:
        try:
            current = frame_signature(read())
        except (OSError, ValueError):
            current = None  # The command socket may not be ready yet.
        now = clock()
        if current is None:
            previous = unchanged_since = None
        elif current != previous:
            previous = current
            unchanged_since = now
        elif now - unchanged_since >= quiet:
            return
        sleep(min(poll, max(0, deadline - clock())))
    raise TimeoutError("window or output frames did not settle before the deadline")


if __name__ == "__main__":
    try:
        wait_for_settled()
    except (TimeoutError, KeyError) as error:
        print(f"sharpness: {error}", file=sys.stderr)
        sys.exit(1)
