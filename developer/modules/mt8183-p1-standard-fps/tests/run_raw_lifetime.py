#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual parent cleanup under delayed final V4L2-node releases (host mock)."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
parent=(r/'duet_p1_continuous.c').read_text();front=(r/'camera_video.c').read_text()
def function(source,name):
 m=re.search(r'^(?:static )?(?:int|void) '+name+r'\(',source,re.M);assert m,name
 end=source.index('{',m.start())+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[m.start():end]
start=parent.index('struct duet_p1_graph {');end=parent.index('\n};',start)+3
graph=parent[start:end].replace('P1_NUM_PADS','2')
header='\n'.join(x for x in (r/'camera_video.h').read_text().splitlines() if not x.startswith('#include'))
unit='#include "raw-lifetime-shim.h"\n'+graph+'\n'+header+'\n'
unit+='\n'.join(function(parent,n) for n in ['graph_nodes_release','graph_cleanup'])+'\n'
unit+='\n'.join(function(front,n) for n in ['dcv_unregister','dcv_cleanup'])+'\n'
unit+=(r/'tests/test_raw_lifetime.c').read_text()
with tempfile.TemporaryDirectory(prefix='duet-raw-lifetime-') as temp:
 d=Path(temp);(d/'actual.c').write_text(unit)
 subprocess.run(['clang','-std=gnu11','-pthread','-DDCV_RAW_HOST','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(r),'-I'+str(r/'tests'),str(d/'actual.c'),'-o',str(d/'check')],check=True)
 subprocess.run([str(d/'check')],check=True,timeout=30)
