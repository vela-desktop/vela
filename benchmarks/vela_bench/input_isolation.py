# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Worker-owned evdev grabs and a disposable transparent cursor theme.

No udev rules or device permissions are changed. Closing the worker's file
descriptors (including SIGKILL) releases every grab in the kernel. Only the
suite's future BUS_VIRTUAL instruments bypass the grabs. Hotplug invalidates
the block, even if it is grabbed before the compositor observes any events.
"""
import fcntl
import os
from pathlib import Path
import re
import select
import signal
import stat
import struct
import threading
import time

from . import BenchError

THEME = 'vela-benchmark-invisible'
PARK_COMMAND = 'point 0 32768'
POLICY = {'version': 'evdev-grab-transparent-cursor-v1',
          'physical_input': 'exclusive-worker-fds', 'hotplug': 'invalidate-block',
          'emergency_abort': 'Ctrl+Alt+Esc', 'cursor_theme': THEME,
          'scaling_pointer_parking': 'left-output-edge-midpoint'}
EVIOCGRAB = 0x40044590
KEY_BYTES = 96  # KEY_CNT / 8, including pointer buttons.
EVIOCGKEY = 0x80004518 | (KEY_BYTES << 16)
EVENT = struct.Struct('@llHHi')
INSTRUMENT_NAMES = {'Vela benchmark desktop input', 'Vela benchmark calibration keyboard'}
# XDG shapes, Qt/GTK aliases and the X cursor font names. No fallback theme:
# all supplied images have zero alpha, including move/resize/busy cursors.
CURSOR_NAMES = tuple(sorted(set('''
default left_ptr arrow top_left_arrow left_ptr_watch progress wait watch text xterm
ibeam vertical-text crosshair cross cross_reverse diamond_cross tcross hand hand1
hand2 pointer openhand closedhand grab grabbing help question_arrow whats_this
size_all all-scroll move fleur size_hor size_ver size_bdiag size_fdiag split_h
split_v e-resize w-resize n-resize s-resize ne-resize nw-resize se-resize sw-resize
ew-resize ns-resize nesw-resize nwse-resize col-resize row-resize h_double_arrow
v_double_arrow bd_double_arrow fd_double_arrow not-allowed forbidden no-drop
copy alias link dnd-copy dnd-link dnd-move dnd-none zoom-in zoom-out context-menu
cell up_arrow center_ptr right_ptr sb_h_double_arrow sb_v_double_arrow
X_cursor based_arrow_down based_arrow_up boat bogosity bottom_left_corner
bottom_right_corner bottom_side bottom_tee box_spiral circle clock coffee_mug
dot dotbox double_arrow draft_large draft_small draped_box exchange gobbler gumby
heart icon iron_cross left_side left_tee leftbutton ll_angle lr_angle man
middlebutton mouse pencil pirate plus right_side right_tee rightbutton rtl_logo
sailboat sb_down_arrow sb_left_arrow sb_right_arrow sb_up_arrow shuttle sizing
spider spraycan star target top_left_corner top_right_corner top_side top_tee
trek ul_angle umbrella ur_angle
'''.split())))


def transparent_cursor():
    """One transparent ARGB pixel in the documented Xcursor file format."""
    return (struct.pack('<4I', 0x72756358, 16, 0x10000, 1)
            + struct.pack('<3I', 0xfffd0002, 24, 28)
            + struct.pack('<9I', 36, 0xfffd0002, 24, 1, 1, 1, 0, 0, 0)
            + struct.pack('<I', 0))


def cursor_environment(home):
    return {'XCURSOR_THEME': THEME, 'XCURSOR_SIZE': '24',
            'XCURSOR_PATH': str(home / '.icons')}


def prepare_cursor_profile(layout, record):
    """Called only after reset_profile, before the reserved UID starts."""
    payload = {Path('.icons') / THEME / 'index.theme':
               f'[Icon Theme]\nName={THEME}\n'.encode(),
               Path('.config/kcminputrc'):
               f'[Mouse][$i]\ncursorTheme={THEME}\ncursorSize=24\n'.encode()}
    payload.update({Path('.icons') / THEME / 'cursors' / name: transparent_cursor()
                    for name in CURSOR_NAMES})
    environment = cursor_environment(layout.home)
    payload[Path('.config/environment.d/90-vela-benchmark-cursor.conf')] = (
        ''.join(f'{key}={value}\n' for key, value in environment.items()).encode())
    for relative, data in payload.items():
        path = layout.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for path in layout.home.rglob('*'):
        os.chown(path, record['uid'], record['gid'])
    return dict(POLICY, shapes=len(CURSOR_NAMES))


def bitmap(path):
    words = path.read_text().split()
    bits = struct.calcsize('@L') * 8
    return sum(int(word, 16) << (bits * i) for i, word in enumerate(reversed(words)))


def input_devices(layout, *, instruments=False):
    root = layout.path('sys/class/input')
    if not root.is_dir():
        raise BenchError('Cannot enumerate evdev devices for input isolation')
    devices = {}
    for entry in sorted(root.glob('event*')):
        if not re.fullmatch(r'event[0-9]+', entry.name):
            continue
        device = entry / 'device'
        try:
            events = bitmap(device / 'capabilities/ev')
            # Ignore lid switches, accelerometers and power-only buttons.
            keys = bitmap(device / 'capabilities/key') if events & 2 else 0
            power_only = sum(1 << k for k in (116, 142, 143))
            if not (events & 12 or keys & ~power_only):
                continue
            name = (device / 'name').read_text().strip()
            virtual = device.resolve().is_relative_to(layout.path('sys/devices/virtual/input'))
            if (instruments and virtual and name in INSTRUMENT_NAMES
                    and (device / 'id/bustype').read_text().strip() == '0006'):
                continue
            major, minor = map(int, (entry / 'dev').read_text().split(':'))
            devices[entry.name] = {'sysfs': str(device.resolve()), 'name': name,
                                   'device_number': os.makedev(major, minor)}
        except FileNotFoundError as exc:
            raise BenchError('Input topology changed during enumeration') from exc
    return devices


class InputIsolation:
    def __init__(self, layout):
        self.layout = layout
        self.devices = {}
        self.fds = {}
        self.stop = threading.Event()
        self.thread = None
        self.error = None
        self.released = False
        self.discarded_events = 0
        self.pressed = {}

    def grab(self, name, identity):
        # Device names come only from the fixed sysfs enumeration. Verify the
        # opened node against that kernel identity before issuing any ioctl.
        if not re.fullmatch(r'event[0-9]+', name):
            raise BenchError('Invalid evdev node identity')
        fd = os.open(self.layout.path('dev/input') / name,
                     os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            if not stat.S_ISCHR(info.st_mode) or info.st_rdev != identity['device_number']:
                raise BenchError('Evdev node differs from sysfs identity')
            fcntl.ioctl(fd, EVIOCGRAB, 1)
            held = bytearray(KEY_BYTES)
            fcntl.ioctl(fd, EVIOCGKEY, held)
            if any(held):
                raise BenchError('Release all physical keys and buttons before starting')
        except BaseException:
            os.close(fd)
            raise
        self.fds[name] = fd
        self.pressed[fd] = set()

    def start(self):
        try:
            self.devices = input_devices(self.layout)
            if any(d['name'] in INSTRUMENT_NAMES for d in self.devices.values()):
                raise BenchError('A benchmark input instrument already exists before takeover')
            for name, identity in self.devices.items():
                self.grab(name, identity)
            if input_devices(self.layout) != self.devices:
                raise BenchError('Input topology changed during acquisition')
            self.thread = threading.Thread(target=self.monitor, name='benchmark-input-guard', daemon=True)
            self.thread.start()
        except BaseException:
            self.close()
            raise

    def consume(self, fd, data):
        if len(data) % EVENT.size:
            raise BenchError('Truncated physical input event stream')
        self.discarded_events += len(data) // EVENT.size
        held = self.pressed[fd]
        for _, _, kind, code, value in EVENT.iter_unpack(data):
            if kind == 0 and code == 3:  # SYN_DROPPED: never trust a lost abort chord.
                raise BenchError('Physical input guard event queue overflowed')
            if kind != 1:
                continue
            if value == 1:
                held.add(code)
            elif value == 0:
                held.discard(code)
            if 1 in held and held.intersection((29, 97)) and held.intersection((56, 100)):
                # PID 1 runs ExecStopPost on termination and releases grabs even
                # if the worker is stuck inside a measurement subprocess.
                os.kill(os.getpid(), signal.SIGTERM)

    def monitor(self):
        try:
            next_scan = time.monotonic() + .5
            while not self.stop.is_set():
                ready, _, _ = select.select(list(self.fds.values()), [], [], .5)
                for fd in ready:
                    try:
                        data = os.read(fd, EVENT.size * 256)
                    except BlockingIOError:
                        continue
                    if not data:
                        raise BenchError('Physical input device disconnected')
                    self.consume(fd, data)
                if time.monotonic() < next_scan:
                    continue
                next_scan = time.monotonic() + .5
                actual = input_devices(self.layout, instruments=True)
                if actual != self.devices:
                    # Best effort: block newly attached hardware too, but reject
                    # the entire block because delivery before this scan can race.
                    for name in actual.keys() - self.devices.keys():
                        self.grab(name, actual[name])
                    raise BenchError('Physical input topology changed; benchmark block invalidated')
        except (OSError, BenchError) as exc:
            self.error = str(exc)

    def check(self):
        if self.error:
            raise BenchError('Input isolation failed: ' + self.error)
        if self.thread is None or not self.thread.is_alive() or self.released:
            raise BenchError('Physical input guard is not running')

    def close(self):
        self.stop.set()
        if self.thread is not None:
            self.thread.join(timeout=3)
        for fd in self.fds.values():
            os.close(fd)  # Kernel releases EVIOCGRAB, also on abnormal process exit.
        self.fds.clear()
        self.released = True

    def evidence(self):
        return {'policy': POLICY, 'devices': self.devices,
                'status': 'failed' if self.error else 'released' if self.released else 'active',
                'error': self.error, 'discarded_events': self.discarded_events,
                'release_method': 'worker-owned-file-descriptors'}
