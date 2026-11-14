# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Quiet reference profiles and real XDG generator overrides, without login."""
import copy
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from vela_bench import BenchError
from vela_bench.host import Layout
from vela_bench.session_profile import autostart_inventory, prepare_profile, POLICY


class SessionProfile(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.layout = Layout(Path(self.temporary.name))
        self.layout.home.mkdir(parents=True)
        self.entries = self.layout.path('etc/xdg/autostart')
        self.entries.mkdir(parents=True)
        for name in ('arch-update-tray.desktop', 'unrelated-reminder.desktop', 'kaccess.desktop'):
            (self.entries / name).write_text('[Desktop Entry]\nType=Application\nName=Fixture\nExec=/usr/bin/true\n')
        self.addCleanup(patch.stopall)
        patch('vela_bench.session_profile.trusted').start()
        patch('vela_bench.session_profile.os.chown').start()

    def prepare(self):
        return prepare_profile(self.layout, {'uid': 1001, 'gid': 1001}, autostart_inventory(self.layout))

    def test_core_services_remain_and_accessory_autostarts_are_overridden_only_in_reserved_profile(self):
        before = {path: path.read_bytes() for path in self.entries.iterdir()}
        result = self.prepare()
        self.assertEqual(result['policy'], POLICY)
        self.assertEqual(result['disabled_autostarts'], ['arch-update-tray.desktop', 'unrelated-reminder.desktop'])
        overrides = self.layout.home / '.config/autostart'
        self.assertEqual({p.name for p in overrides.iterdir()}, set(result['disabled_autostarts']))
        for path in overrides.iterdir():
            self.assertIn('Hidden=true', path.read_text())
            self.assertNotIn('Exec=', path.read_text())
        self.assertEqual(before, {path: path.read_bytes() for path in self.entries.iterdir()})

    def test_notification_preferences_cover_both_desktops_before_startup(self):
        self.prepare()
        vela = (self.layout.home / '.config/Vela/vela-shell.conf').read_text()
        plasma = (self.layout.home / '.config/plasmanotifyrc').read_text()
        self.assertIn('[notifications]\ndoNotDisturb=true', vela)
        self.assertIn('Until=2099,12,31,23,59,59,UTC', plasma)
        self.assertIn('NotificationSoundsMuted=true', plasma)
        self.assertIn('CriticalInDndMode=false', plasma)
        self.assertIn('[DoNotDisturb][$i]', plasma)

    def test_inventory_binds_system_entries_and_changes_when_updater_changes(self):
        before = autostart_inventory(self.layout)
        item = next(row for row in before if row['name'] == 'arch-update-tray.desktop')
        path = self.entries / item['name']
        self.assertEqual(item['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertFalse(item['allowed'])
        path.write_text(path.read_text() + 'Comment=Changed package\n')
        self.assertNotEqual(autostart_inventory(self.layout), before)

    def test_recorded_names_and_allowlist_decisions_cannot_escape_or_override_core_services(self):
        inventory = autostart_inventory(self.layout)
        for key, value in (('name', '../../outside.desktop'), ('name', '/tmp/outside.desktop'),
                           ('directory', 'tmp/untrusted'), ('allowed', True)):
            changed = copy.deepcopy(inventory)
            next(row for row in changed if row['name'] == 'arch-update-tray.desktop')[key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(BenchError, 'frozen autostart'):
                prepare_profile(self.layout, {'uid': 1001, 'gid': 1001}, changed)
        self.assertEqual(list(self.layout.home.iterdir()), [])

    def test_real_systemd_generator_suppresses_updater_and_preserves_core_entry(self):
        generator = next((path for path in ('/usr/lib/systemd/user-generators/systemd-xdg-autostart-generator',
                        '/lib/systemd/user-generators/systemd-xdg-autostart-generator') if Path(path).is_file()), None)
        if generator is None:
            self.skipTest('systemd XDG autostart generator unavailable')
        runtime = self.layout.path('runtime'); runtime.mkdir(mode=0o700)
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'HOME': str(self.layout.home),
               'XDG_CONFIG_HOME': str(self.layout.home / '.config'), 'XDG_CONFIG_DIRS': str(self.entries.parent),
               'XDG_RUNTIME_DIR': str(runtime), 'XDG_CURRENT_DESKTOP': 'KDE'}
        def generate(label):
            destination = self.layout.path(label)
            destinations = [destination / name for name in ('normal', 'early', 'late')]
            for path in destinations: path.mkdir(parents=True)
            result = subprocess.run([generator, *map(str, destinations)], env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            return {p.name for p in destination.rglob('*.service')}
        before = generate('before')
        self.assertTrue(any('arch' in name for name in before), before)
        self.prepare()
        after = generate('after')
        self.assertFalse(any('arch' in name or 'reminder' in name for name in after), after)
        self.assertTrue(any('kaccess' in name for name in after), after)
