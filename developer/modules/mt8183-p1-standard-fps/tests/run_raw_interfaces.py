#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual modified C functions, synthetic buffers/graph/control wrappers."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
front=(r/'camera_video.c').read_text()
parent=(r/'duet_p1_continuous.c').read_text()
def function(source,name):
 m=re.search(r'^(?:static )?(?:int|void) '+name+r'\(',source,re.M);assert m,name
 end=source.index('{',m.start())+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[m.start():end]
header='\n'.join(x for x in (r/'camera_video.h').read_text().splitlines() if not x.startswith('#include'))
structures='''
struct duet_p1_graph {struct media_device media;struct v4l2_subdev subdev,*sensor;};
struct integrated_state {struct duet_p1_graph graph;struct dcv_video video;bool sensor_on,seninf_on;u32 requested_exposure,requested_gain;struct {void *dev;} resources;};
'''
unit='#include "raw-interface-shim.h"\n'+header+'\n'+structures+'\n'
unit+=front[front.index('/* The capture frontend preserves CFA'):front.index('static int setup(')]+'\n'
unit+='\n'.join(function(front,n) for n in ['enumfmt','setup','prepare','queue_buffer','return_buffers','start','stop','format','setfmt','getfmt','validate_link','open_video','dcv_deliver'])+'\n'
unit+='\n'.join(function(parent,n) for n in ['graph_format','graph_init_state','graph_enum_code','graph_enum_size','graph_get_fmt','graph_set_fmt','graph_link_notify','capture_conditions'])+'\n'
unit+=(r/'tests/test_raw_interfaces.c').read_text()
assert 'camera_softisp.o' not in (r/'Makefile').read_text()
assert 'V4L2_CAP_IO_MC' in front and 'VB2_DMABUF' in front
assert 'v4l2_ctrl_s_ctrl' not in function(parent,'capture_conditions')
assert 'v->exposure' not in front and 'v->gain' not in front
assert parent.index('v4l2_device_put(&graph->v4l2)') < parent.index('wait_for_completion(&graph->nodes_released)') < parent.index('v4l2_subdev_cleanup(&graph->subdev)')
with tempfile.TemporaryDirectory(prefix='duet-raw-interfaces-') as temp:
 d=Path(temp);(d/'actual.c').write_text(unit)
 subprocess.run(['clang','-std=gnu11','-pthread','-DDCV_RAW_HOST','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(r),'-I'+str(r/'tests'),str(d/'actual.c'),str(r/'camera_raw.c'),'-o',str(d/'check')],check=True)
 subprocess.run([str(d/'check')],check=True,timeout=30)
