#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Bounded native GPU workload startup/CLI; no GPU or display needed."""

from pathlib import Path
import socket
import subprocess
import sys
import unittest

TOOL=str(Path(sys.argv.pop(1)).resolve())

class Failures(unittest.TestCase):
    def test_help_and_invalid_arguments_before_display_connection(self):
        self.assertEqual(subprocess.run([TOOL,'--help'],env={},capture_output=True).returncode,0)
        for args in (['150oops'],['nan'],['0'],['1001'],['--json'],['--until-stdin-eof'],
                     ['--timeout'],['--json','--until-stdin-eof','--timeout','0']):
            with self.subTest(args=args):
                result=subprocess.run([TOOL,*args],env={},capture_output=True,timeout=3)
                self.assertNotEqual(result.returncode,0)
                self.assertNotIn(b'no Wayland compositor',result.stderr)

    def test_deadline_includes_stalled_registry_exchange(self):
        for args in ([],['--fullscreen'],['--fullscreen','--implicit'],['--monolithic']):
            with self.subTest(args=args):
                client,server=socket.socketpair()
                with client,server:
                    result=subprocess.run([TOOL,'150','--json','--until-stdin-eof','--timeout','1',*args],
                        env={'WAYLAND_SOCKET':str(client.fileno())},pass_fds=(client.fileno(),),capture_output=True,timeout=4)
                self.assertEqual(result.returncode,124)
                self.assertIn(b'deadline expired',result.stderr)
                self.assertNotIn(b'"complete":true',result.stdout)

if __name__=='__main__':unittest.main()
