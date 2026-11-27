#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""No display required: descriptors and fixed CLI/capability refusal contracts."""
import json
import os
import subprocess
import sys

binary=sys.argv[1]
env={'PATH':'/usr/bin:/bin','LANG':'C','HOME':'/nonexistent','QT_QPA_PLATFORM':'invalid-display-for-describe'}
result=subprocess.run([binary,'--describe'],env=env,capture_output=True,text=True,timeout=10,check=True)
row=json.loads(result.stdout)
assert row['schema']==1 and row['method']=='toolkit-scaling-corpus-v1' and len(row['content_sha256'])==64
assert row['font_requested']=='DejaVu Sans' and row['font_size_px']==16
for args in ([],['--case','unknown'],['--url','https://example.invalid'],['--describe','extra']):
 result=subprocess.run([binary,*args],env=env,capture_output=True,text=True,timeout=10)
 assert result.returncode==1,(args,result.returncode,result.stderr)
if os.getuid()!=0:
 if 'qt-xwayland' in row['cases']:
  result=subprocess.run([binary,'--case','qt-xwayland'],env=env,capture_output=True,text=True,timeout=10)
  assert result.returncode==77
  assert json.loads(result.stdout)['reason'] in ('fixed-font-unavailable','x11-display-unavailable')
 if 'browser-wayland' in row['cases']:
  result=subprocess.run([binary,'--case','browser-wayland'],env=dict(env,QTWEBENGINE_DISABLE_SANDBOX='1'),capture_output=True,text=True,timeout=10)
  assert result.returncode==77
  assert json.loads(result.stdout)['reason'] in ('fixed-font-unavailable','browser-rendering-overrides')
print('Corpus descriptor, fixed CLI and capability refusal passed')
