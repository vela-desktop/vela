# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic evdev boundaries; these tests never grab operator hardware."""
import ctypes
import ctypes.util
import os
from pathlib import Path
import signal
import stat
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench import BenchError
from vela_bench.host import Layout
from vela_bench import input_isolation as isolation


class InputIsolation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.layout = Layout(Path(self.temp.name))
        self.guard = isolation.InputIsolation(self.layout)

    def device(self, index=0, *, virtual=False, name='Physical keyboard', events='3', keys='2'):
        root = self.layout.path('sys/devices/virtual/input' if virtual else 'sys/devices/platform/input')
        device = root / f'input{index}'
        (device / 'capabilities').mkdir(parents=True)
        (device / 'capabilities/ev').write_text(events)
        (device / 'capabilities/key').write_text(keys)
        (device / 'name').write_text(name)
        (device / 'id').mkdir()
        (device / 'id/bustype').write_text('0006' if virtual else '0003')
        event = device / f'event{index}'
        event.mkdir()
        (event / 'dev').write_text(f'13:{64+index}')
        (event / 'device').symlink_to(device)
        entry = self.layout.path(f'sys/class/input/event{index}')
        entry.parent.mkdir(parents=True, exist_ok=True)
        entry.symlink_to(event)
        return device

    def test_physical_and_unrelated_virtual_devices_are_included_but_future_instruments_bypass(self):
        self.device()
        self.device(1, virtual=True, name='Bluetooth or remapping keyboard')
        self.device(2, virtual=True, name='Vela benchmark desktop input')
        self.device(3, name='Vela benchmark desktop input')  # A name alone is insufficient.
        all_devices = isolation.input_devices(self.layout)
        self.assertEqual(set(all_devices), {'event0', 'event1', 'event2', 'event3'})
        self.assertEqual(set(isolation.input_devices(self.layout, instruments=True)), {'event0', 'event1', 'event3'})
        self.assertEqual(all_devices['event0']['device_number'], os.makedev(13, 64))

    def test_mouse_touchscreen_and_keyboard_are_included_without_blocking_power_only_buttons(self):
        self.device(events='7')
        self.device(1, events='9', keys='0', name='Touchscreen')
        self.device(2, keys=f'{1 << 116:x}', name='Power button')
        self.device(3, events='21', keys='0', name='Lid switch')
        self.assertEqual(set(isolation.input_devices(self.layout)), {'event0', 'event1'})

    def test_missing_enumeration_fails_closed_and_held_keys_close_the_grab(self):
        with self.assertRaisesRegex(BenchError, 'Cannot enumerate'):
            self.guard.start()
        self.assertTrue(self.guard.released)
        descriptor = os.open('/dev/null', os.O_RDONLY)
        def ioctl(fd, operation, value):
            if operation == isolation.EVIOCGKEY:
                value[0] = 2
        with patch.object(isolation.os, 'open', return_value=descriptor), \
                patch.object(isolation.os, 'fstat', return_value=SimpleNamespace(st_mode=stat.S_IFCHR, st_rdev=42)), \
                patch.object(isolation.fcntl, 'ioctl', side_effect=ioctl) as calls:
            with self.assertRaisesRegex(BenchError, 'Release all physical keys'):
                self.guard.grab('event0', {'device_number': 42})
        with self.assertRaises(OSError):
            os.fstat(descriptor)
        self.assertEqual(calls.call_args_list[0].args[1:], (isolation.EVIOCGRAB, 1))

    def test_evdev_identity_and_grab_failure_cannot_leak_open_descriptors(self):
        for wrong_node in (False, True):
            descriptor = os.open('/dev/null', os.O_RDONLY)
            with patch.object(isolation.os, 'open', return_value=descriptor), \
                    patch.object(isolation.os, 'fstat', return_value=SimpleNamespace(st_mode=stat.S_IFREG if wrong_node else stat.S_IFCHR, st_rdev=42)), \
                    patch.object(isolation.fcntl, 'ioctl', side_effect=OSError('busy')) as ioctl:
                with self.assertRaises((BenchError, OSError)):
                    self.guard.grab('event0', {'device_number': 42})
                if wrong_node:
                    ioctl.assert_not_called()
            with self.assertRaises(OSError):
                os.fstat(descriptor)
        with patch.object(isolation.os, 'open') as open_node:
            with self.assertRaises(BenchError):
                self.guard.grab('../other', {})
            open_node.assert_not_called()

    def test_partial_acquisition_releases_previously_grabbed_devices(self):
        descriptor = os.open('/dev/null', os.O_RDONLY)
        def grab(name, identity):
            if name == 'event1':
                raise OSError('second device busy')
            self.guard.fds[name] = descriptor
        with patch.object(isolation, 'input_devices', return_value={'event0': {'name': 'keyboard'}, 'event1': {'name': 'mouse'}}), \
                patch.object(self.guard, 'grab', side_effect=grab):
            with self.assertRaises(OSError):
                self.guard.start()
        with self.assertRaises(OSError):
            os.fstat(descriptor)
        self.guard.close()  # Also safe after partial acquisition.

    def test_hotplug_is_grabbed_and_invalidates_the_whole_block(self):
        self.guard.devices = {'event0': {}}
        with patch.object(isolation.select, 'select', return_value=([], [], [])), \
                patch.object(isolation.time, 'monotonic', side_effect=[0, 1, 1]), \
                patch.object(isolation, 'input_devices', return_value={'event0': {}, 'event1': {'name': 'new'}}), \
                patch.object(self.guard, 'grab') as grab:
            self.guard.monitor()
        grab.assert_called_once_with('event1', {'name': 'new'})
        with self.assertRaisesRegex(BenchError, 'topology changed'):
            self.guard.check()
        self.assertEqual(self.guard.evidence()['status'], 'failed')

    def test_physical_events_are_discarded_and_abort_chord_terminates_only_worker(self):
        self.guard.pressed[42] = set()
        def event(code, value=1):
            return isolation.EVENT.pack(0, 0, 1, code, value)
        with patch.object(isolation.os, 'kill') as kill:
            self.guard.consume(42, event(30) + event(30, 0))
            kill.assert_not_called()
            self.guard.consume(42, event(29) + event(56) + event(1))
            kill.assert_called_once_with(os.getpid(), signal.SIGTERM)
        self.assertEqual(self.guard.discarded_events, 5)
        with self.assertRaisesRegex(BenchError, 'overflowed'):
            self.guard.consume(42, isolation.EVENT.pack(0, 0, 0, 3, 0))

    def test_successful_guard_closes_descriptors_and_refuses_measurement_after_release(self):
        descriptor = os.open('/dev/null', os.O_RDONLY)
        self.guard.fds['event0'] = descriptor
        self.guard.thread = Mock()
        self.guard.thread.is_alive.return_value = True
        self.guard.check()
        self.guard.close()
        self.assertEqual(self.guard.evidence()['status'], 'released')
        with self.assertRaisesRegex(BenchError, 'not running'):
            self.guard.check()
        with self.assertRaises(OSError):
            os.fstat(descriptor)


class InvisibleCursor(unittest.TestCase):
    def test_profile_is_disposable_and_shared_by_both_desktops(self):
        with tempfile.TemporaryDirectory() as temporary:
            layout = Layout(Path(temporary))
            layout.home.mkdir(parents=True)
            with patch.object(isolation.os, 'chown'):
                result = isolation.prepare_cursor_profile(layout, {'uid': 1001, 'gid': 1001})
            self.assertEqual(result['cursor_theme'], isolation.THEME)
            self.assertIn('[$i]', (layout.home / '.config/kcminputrc').read_text())
            self.assertIn('XCURSOR_THEME=' + isolation.THEME,
                          (layout.home / '.config/environment.d/90-vela-benchmark-cursor.conf').read_text())
            cursors = layout.home / '.icons' / isolation.THEME / 'cursors'
            self.assertEqual({p.name for p in cursors.iterdir()}, set(isolation.CURSOR_NAMES))
            self.assertTrue({'left_ptr', 'default', 'text', 'grabbing', 'nwse-resize', 'wait'} <= set(isolation.CURSOR_NAMES))
            self.assertTrue(all(p.read_bytes() == isolation.transparent_cursor() for p in cursors.iterdir()))
            self.assertEqual(isolation.cursor_environment(layout.home)['XCURSOR_PATH'], str(layout.home / '.icons'))

    def test_generated_cursor_is_decoded_by_real_libxcursor_as_zero_alpha(self):
        library = ctypes.util.find_library('Xcursor')
        if not library:
            self.skipTest('libXcursor is unavailable')
        class Image(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint32) for name in ('version', 'size', 'width', 'height', 'xhot', 'yhot', 'delay')] + [('pixels', ctypes.POINTER(ctypes.c_uint32))]
        api = ctypes.CDLL(library)
        api.XcursorFilenameLoadImage.argtypes = (ctypes.c_char_p, ctypes.c_int)
        api.XcursorFilenameLoadImage.restype = ctypes.POINTER(Image)
        api.XcursorImageDestroy.argtypes = (ctypes.POINTER(Image),)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'cursor'
            path.write_bytes(isolation.transparent_cursor())
            for size in (24, 30, 36, 42, 48):
                pointer = api.XcursorFilenameLoadImage(os.fsencode(path), size)
                self.assertTrue(pointer, size)
                try:
                    image = pointer.contents
                    self.assertEqual((image.width, image.height, image.xhot, image.yhot, image.pixels[0]), (1, 1, 0, 0, 0))
                finally:
                    api.XcursorImageDestroy(pointer)
