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
    commands='alt-tab-begin\nalt-tab-end\nwindow-menu\nwindow-menu-plasma\nworkspace-next\nworkspace-previous\nworkspace-first-plasma\nworkspace-second-plasma\nquit\n'
    result,keys,rows=run(binary,commands)
    assert result.returncode==0,result.stderr
    expected=[(56,1),(15,1),(15,0),(56,0),
              (56,1),(57,1),(57,0),(56,0),(56,1),(61,1),(61,0),(56,0),
              (125,1),(29,1),(106,1),(106,0),(29,0),(125,0),
              (125,1),(29,1),(105,1),(105,0),(29,0),(125,0),
              (29,1),(59,1),(59,0),(29,0),(29,1),(60,1),(60,0),(29,0)]
    assert keys[:len(expected)]==expected,keys
    assert [r['action_id'] for r in rows[1:]]==list(range(1,9)),rows
    for command in commands.splitlines()[:-1]:
        result,keys,rows=run(binary,command+'\n',False)
        assert result.returncode!=0 and len(rows)==1 and not any(v==1 for _,v in keys),(command,keys,rows)
    print('Production evdev shortcut/timestamp/guard checks passed; no kernel device used.')


if __name__=='__main__':main(sys.argv[1])
