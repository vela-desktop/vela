#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise production shortcut parsing against a link-only event sink."""
import json
import subprocess
import sys


def run(binary,commands,desktop=True):
    result=subprocess.run([binary,*(['--desktop'] if desktop else [])],input=commands,
                          capture_output=True,text=True,timeout=5)
    keys=[tuple(map(int,line.split()[2:])) for line in result.stderr.splitlines()
          if line.startswith('EVENT 1 ')]
    rows=[json.loads(line) for line in result.stdout.splitlines()]
    return result,keys,rows


def main(binary):
    # Maximize and restore both toggle on Plasma. The sink observes actual
    # evdev writes from the production parser, not a second shortcut mapping.
    result,keys,rows=run(binary,'maximize-toggle\nmaximize-toggle\nquit\n')
    assert result.returncode==0,result.stderr
    assert keys[:8]==[(125,1),(104,1),(104,0),(125,0)]*2,keys
    assert [r['action_id'] for r in rows[1:]]==[1,2],rows
    assert all(r['before_monotonic_ns']<=r['after_monotonic_ns'] for r in rows[1:]),rows
    result,keys,_=run(binary,'maximize\nrestore\nquit\n')
    assert result.returncode==0,result.stderr
    assert keys[:8]==[(125,1),(103,1),(103,0),(125,0),(125,1),(108,1),(108,0),(125,0)],keys
    for command,desktop in [('maximize-toggle\n',False),('maximize-toggle extra\n',True)]:
        result,keys,rows=run(binary,command,desktop)
        assert result.returncode!=0 and len(rows)==1,(result,rows)
        assert not any(value==1 for _,value in keys),keys
    print('Production evdev shortcut/timestamp/guard checks passed; no kernel device used.')


if __name__=='__main__':main(sys.argv[1])
