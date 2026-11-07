#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Real systemd alias-mask regression using only temporary harmless user units.

No VT, getty, PAM, desktop or system service is addressed. Production names
are mapped to random private templates whose only command is /usr/bin/true.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

from gi.repository import Gio,GLib

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vela_bench.host import Layout,SAFE_ENV,HostOperationError
from vela_bench.pilot import reserve_vt


def main():
    if os.getuid()==0: raise RuntimeError('Run this isolated user-manager check unprivileged')
    runtime=Path('/run/user')/str(os.getuid())
    units=runtime/'systemd/user'
    units.mkdir(parents=True,exist_ok=True)
    env=dict(SAFE_ENV,XDG_RUNTIME_DIR=str(runtime),DBUS_SESSION_BUS_ADDRESS='unix:path='+str(runtime/'bus'))
    prefix='vela-benchmark-vt-check-'+uuid.uuid4().hex
    canonical=prefix+'-canonical@.service';alias=prefix+'-alias@.service'
    def mapped(name):return (prefix+'-canonical' if name.startswith('getty@') else prefix+'-alias')+'@tty4.service'
    names=[mapped('getty@tty4.service'),mapped('autovt@tty4.service')]
    paths=[units/name for name in (canonical,alias,*names)]
    if any(path.exists() or path.is_symlink() for path in paths):raise RuntimeError('Unexpected private fixture collision')
    def run(argv,check=True):
        result=subprocess.run(argv,env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=8)
        if check and result.returncode:raise HostOperationError(argv,result.returncode,result.stdout,result.stderr)
        return result
    address=env['DBUS_SESSION_BUS_ADDRESS']
    connection=Gio.DBusConnection.new_for_address_sync(address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT|Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,None,None)
    try:
        paths[0].write_text('[Unit]\nDescription=Vela isolated alias mask check\n[Service]\nType=oneshot\nExecStart=/usr/bin/true\nRemainAfterExit=yes\n')
        paths[1].symlink_to(canonical)
        run(['systemctl','--user','daemon-reload'])
        # Preload both aliases and retain the same canonical object while it
        # is being masked, matching the cache state in the failed real run.
        for name in names:run(['systemctl','--user','show',name,'--property=Names'])
        connection.call_sync('org.freedesktop.systemd1','/org/freedesktop/systemd1',
            'org.freedesktop.systemd1.Manager','RefUnit',GLib.Variant('(s)',(names[0],)),None,Gio.DBusCallFlags.NONE,5000,None)
        with tempfile.TemporaryDirectory(prefix='vela-vt-reservation-') as temporary:
            root=Path(temporary);layout=Layout(root)
            observations={'canonical_mask_state_seen_through_unmasked_alias':False}
            class Host:
                def run(self,command,**options):
                    assert command[0]=='systemctl' and command[1] in ('show','mask'),command
                    unit=command[-1] if command[1]=='mask' else command[2]
                    actual=mapped(unit)
                    args=['systemctl','--user',command[1]]
                    args+=['--runtime',actual] if command[1]=='mask' else [actual,*command[3:]]
                    result=run(args,check=options.get('check',True))
                    if command[1]=='show' and unit.startswith('autovt@') and not (units/actual).is_symlink():
                        observations['canonical_mask_state_seen_through_unmasked_alias']=any(
                            line in ('UnitFileState=masked','UnitFileState=masked-runtime') for line in result.stdout.splitlines())
                    if command[1]=='mask':
                        real=units/actual
                        assert real.is_symlink() and os.readlink(real)=='/dev/null',real
                        mirror=layout.path('run/systemd/system')/unit
                        mirror.parent.mkdir(parents=True,exist_ok=True);mirror.symlink_to('/dev/null')
                    return result
            state={'target_vt':4,'masked_units':[]}
            reserve_vt(Host(),root,state,layout)
            assert state['masked_units']==['getty@tty4.service','autovt@tty4.service'],state
            assert observations['canonical_mask_state_seen_through_unmasked_alias'],observations
            for name in names:
                attempted=run(['systemctl','--user','start',name],check=False)
                assert attempted.returncode and 'masked' in attempted.stderr,attempted.stderr
            print(json.dumps({'scope':'real-user-manager-private-aliases-no-vt-or-system-units',
                'both_names_masked':True,'both_starts_refused':True,'owned_masks':state['masked_units'],**observations}))
    finally:
        for name in names:run(['systemctl','--user','stop',name],check=False)
        connection.close_sync(None)
        for path in reversed(paths):path.unlink(missing_ok=True)
        run(['systemctl','--user','daemon-reload'])
        for name in names:run(['systemctl','--user','reset-failed',name],check=False)


if __name__=='__main__':main()
