#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Wait for the expected sharpness state AND a quiet rendered output.

A quiet compositor alone is not proof that an input shortcut was processed:
the previous frame might still be idle. Check the expected snap/maximize
transition first, then require stable window geometry and output frame counts.
"""

import json
import os
import socket
import sys
import time


PHASES = ("opened", "snapped-left", "snapped-right", "maximized", "restored")
LEFT = [0, 0, 6, 12]
RIGHT = [6, 0, 12, 12]


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


def phase_matches(state, phase):
    if phase not in PHASES:
        raise ValueError(f"unknown sharpness phase: {phase}")
    windows = state.get("windows", ())
    if len(windows) != 1 or not state.get("outputs"):
        return False
    w = windows[0]
    if w["minimized"]:
        return False
    if phase == "snapped-left":
        return w["snap"] == LEFT and not w["maximized"]
    if phase == "snapped-right":
        return w["snap"] == RIGHT and not w["maximized"]
    if phase == "maximized":
        return w["maximized"] and w["snap"] is None
    # Both the initial and restored window must be free-floating, at the
    # pattern's requested logical size. No stale snap/quarter may pass.
    return (not w["maximized"] and w["snap"] is None
            and (w["w"], w["h"]) == (401, 301))


def frame_signature(state):
    outputs = state["outputs"]
    windows = state["windows"]
    return (
        tuple((o["name"], o["frames"]) for o in outputs),
        tuple((w["id"], w["x"], w["y"], w["w"], w["h"],
               w["maximized"], w["minimized"], tuple(w["snap"] or ()))
              for w in windows),
    )


def wait_for_settled(read=read_state, *, phase="opened", timeout=6.0,
                     quiet=0.15, poll=0.05, clock=time.monotonic,
                     sleep=time.sleep):
    if phase not in PHASES:
        raise ValueError(f"unknown sharpness phase: {phase}")
    deadline = clock() + timeout
    previous = None
    unchanged_since = None
    while clock() < deadline:
        try:
            state = read()
            current = frame_signature(state) if phase_matches(state, phase) else None
        except (OSError, ValueError, KeyError):
            current = None  # The command socket or mapping may still be pending.
        now = clock()
        if current is None:
            previous = unchanged_since = None
        elif current != previous:
            previous = current
            unchanged_since = now
        elif now - unchanged_since >= quiet:
            return
        sleep(min(poll, max(0, deadline - clock())))
    raise TimeoutError(f"{phase}: expected state and output frames did not settle")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in PHASES:
        print(f"usage: {sys.argv[0]} " + "|".join(PHASES), file=sys.stderr)
        sys.exit(2)
    try:
        wait_for_settled(phase=sys.argv[1])
    except TimeoutError as error:
        print(f"sharpness: {error}", file=sys.stderr)
        sys.exit(1)
