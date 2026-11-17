# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference desktop profile: core session services, no accessory autostarts.

XDG Hidden overrides live only in the reserved disposable home. Normal desktop
entries, the host's autostart files and personal notification settings stay owned
by their original users. Unexpected pixels still fail the strict capture audit.
"""
import hashlib
import os
from pathlib import Path
import re

from . import BenchError
from .host import trusted

AUTOSTART_DIRS = ('etc/xdg/autostart', 'usr/share/autostart', 'usr/share/xdg/autostart')
CORE_AUTOSTART = (
    'at-spi-dbus-bus.desktop', 'gmenudbusmenuproxy.desktop', 'kaccess.desktop',
    'org.kde.kded6.desktop', 'org.kde.kglobalaccel.desktop', 'org.kde.plasmashell.desktop',
    'pam_kwallet_init.desktop', 'polkit-kde-authentication-agent-1.desktop',
    'powerdevil.desktop', 'xembedsniproxy.desktop', 'xdg-user-dirs.desktop',
)
POLICY = {'version': 'core-autostart-quiet-profile-v1', 'allowed_autostarts': list(CORE_AUTOSTART),
          'notifications': 'do-not-disturb', 'personal_settings': 'disposable-reserved-home-only'}


def autostart_inventory(layout):
    inventory = []
    for relative in AUTOSTART_DIRS:
        directory = layout.path(relative)
        if not directory.exists():
            continue
        trusted(directory, directory=True)
        for path in sorted(directory.glob('*.desktop')):
            if not re.fullmatch(r'[A-Za-z0-9_.-]{1,240}\.desktop', path.name):
                raise BenchError('Unsupported system autostart filename')
            trusted(path.resolve())
            if path.stat().st_size > 65536:
                raise BenchError('Oversized system autostart entry')
            inventory.append({'directory': relative, 'name': path.name,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'allowed': path.name in CORE_AUTOSTART})
    return inventory


def prepare_profile(layout, record, inventory):
    # These files are created before the reserved user or its manager starts.
    payload = {Path('.config/Vela/vela-shell.conf'): b'[notifications]\ndoNotDisturb=true\n',
        Path('.config/plasmanotifyrc'): (
            b'[DoNotDisturb][$i]\nUntil=2099,12,31,23,59,59,UTC\nNotificationSoundsMuted=true\n'
            b'[Notifications][$i]\nCriticalInDndMode=false\n')}
    disabled = []
    for entry in inventory:
        name = entry.get('name')
        if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,240}\.desktop', name)
                or entry.get('directory') not in AUTOSTART_DIRS
                or entry.get('allowed') is not (name in CORE_AUTOSTART)):
            raise BenchError('Invalid frozen autostart identity')
        if name not in CORE_AUTOSTART:
            payload[Path('.config/autostart') / name] = b'[Desktop Entry]\nType=Application\nHidden=true\n'
            disabled.append(name)
    for relative, data in payload.items():
        path = layout.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for path in layout.home.rglob('*'):
        os.chown(path, record['uid'], record['gid'])
    return {'policy': POLICY, 'autostart_inventory': inventory, 'disabled_autostarts': sorted(set(disabled))}
