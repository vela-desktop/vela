# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import copy
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

from vela_bench import BenchError
from vela_bench import resources


class ResourceTransport(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name);self.proc=self.root/'proc';self.proc.mkdir()
        self.cg=self.root/'cg';slice_=self.cg/'user.slice/user-1001.slice';slice_.mkdir(parents=True)
        (slice_/'cpu.stat').write_text('usage_usec 100000\n')
        self.controller='0::/system.slice/vela-benchmark-pilot.service'
        parent=self.proc/str(os.getpid());parent.mkdir();(parent/'cgroup').write_text(self.controller)
        self.command=['setpriv','--reuid=1001','/usr/bin/systemd-run','--user','--wait','--pipe',
                      '--unit=vela-benchmark-measurement.service','/usr/bin/sleep','1']
        self.child=SimpleNamespace(pid=33,args=self.command,poll=lambda:None)
        self.process(22,'0::/user.slice/user-1001.slice/user@1001.service/app.slice/work.service')
        self.process(33,self.controller)

    def process(self,pid,membership):
        path=self.proc/str(pid);path.mkdir(exist_ok=True)
        (path/'status').write_text(f'Name:\tsystemd-run\nUid:\t1001\t1001\t1001\t1001\nPPid:\t{os.getpid()}\n')
        fields=['S']+['0']*19;fields[11]='10';fields[19]=str(pid)
        (path/'stat').write_text(f'{pid} (systemd-run) '+' '.join(fields))
        (path/'smaps_rollup').write_text('Pss: 1024 kB\n')
        (path/'cgroup').write_text(membership)
        (path/'cmdline').write_bytes(b'\0'.join(c.encode() for c in self.command[2:])+b'\0')

    def identity(self):return resources.transport_identity(self.child,self.command,proc=self.proc)

    def snapshot(self,identity=None):return resources.snapshot(1001,proc=self.proc,cgroup=self.cg,transport=identity)

    def test_only_owned_wait_client_is_separate_from_session_pss_and_cpu(self):
        identity=self.identity()
        with self.assertRaisesRegex(BenchError,'PID 33 .*systemd-run.*system.slice'):
            self.snapshot()
        row=self.snapshot(identity)
        self.assertEqual([p['pid'] for p in row['processes']],[22])
        self.assertEqual([p['pid'] for p in row['controller_transports']],[33])
        later=copy.deepcopy(row)
        for key in ('begin_monotonic_ns','end_monotonic_ns','cpu_begin_monotonic_ns','cpu_end_monotonic_ns'):
            later[key]+=2_000_000_000
        later['cpu_usage_usec']+=500000;later['controller_transports'][0]['cpu_ticks']+=5
        result=resources.metrics([row,later],1001,identity)
        self.assertEqual(result['cpu_seconds'],.5)
        self.assertEqual(result['pss_mib_median'],1)
        self.assertEqual(result['collector_transport_pss_mib_max'],1)
        self.assertAlmostEqual(result['collector_transport_cpu_seconds_observed'],5/os.sysconf('SC_CLK_TCK'))
        self.assertFalse(result['collector_transport_in_uid_cpu_totals'])
        with self.assertRaisesRegex(BenchError,'registered identity'):
            resources.metrics([row,later],1001)

    def test_another_systemd_run_in_the_controller_cgroup_is_still_refused(self):
        identity=self.identity();self.process(44,self.controller)
        with self.assertRaisesRegex(BenchError,'boundary: PID 44'):
            self.snapshot(identity)

    def test_pid_reuse_reparenting_migration_and_command_changes_are_refused(self):
        for field,value in [('start_ticks',999),('parent_pid',999),('cgroup','0::/other'),('command',['/usr/bin/false'])]:
            identity=self.identity();identity[field]=value
            with self.subTest(field=field),self.assertRaisesRegex(BenchError,'transport identity changed'):
                self.snapshot(identity)
        self.child.args=['/usr/bin/false']
        with self.assertRaisesRegex(BenchError,'owned launch request'):self.identity()

    def test_registration_handles_a_short_lived_client_without_reusing_its_pid(self):
        self.child.pid=9999;self.child.poll=Mock(side_effect=[None,0])
        self.assertIsNone(self.identity())

    def test_offline_transport_identity_and_cpu_tampering_is_refused(self):
        identity=self.identity();row=self.snapshot(identity)
        for field,value in [('start_ticks',999),('cgroup','0::/other'),('command',['/usr/bin/false']),
                            ('parent_pid',999),('cpu_ticks',0),('classification','session')]:
            changed=copy.deepcopy(row);changed['controller_transports'][0][field]=value
            with self.subTest(field=field),self.assertRaisesRegex(BenchError,'registered identity'):
                resources.metrics([changed,changed],1001,identity)
