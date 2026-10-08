#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual production cache guard; synthetic standard-control wrapper, no hardware."""
from pathlib import Path
import re,tempfile,subprocess,json
r=Path(__file__).resolve().parents[1]
s=(r/'duet_p1_continuous.c').read_text();m=re.search(r'^static int capture_conditions\(',s,re.M);assert m
e=s.index('{',m.start())+1;depth=1
while depth:depth+=(s[e]=='{')-(s[e]=='}');e+=1
fn=s[m.start():e]
head='''#include "raw-interface-shim.h"
struct integrated_state {struct {struct v4l2_subdev *sensor;} graph;bool sensor_on,seninf_on;u32 requested_exposure,requested_gain;struct {void *dev;} resources;};
'''
body='''
int main(void){
 struct v4l2_ctrl_handler h={.ctrl={{1,1583,190,7151},{2,430,4,7150},{3,64,16,248},{4,0,0,1}}};
 struct v4l2_subdev sensor={.ctrl_handler=&h};struct integrated_state state={.graph={.sensor=&sensor}};
 int blanks[]={190,191,470,887,1258,1580,1583,7151};
 for(unsigned int i=0;i<sizeof(blanks)/sizeof(blanks[0]);i++){
  h.ctrl[0].value=blanks[i];assert(!capture_conditions(&state));
  assert(h.ctrl[0].value==blanks[i]&&h.ctrl[1].value==430&&h.ctrl[2].value==64);
  assert(state.requested_exposure==430&&state.requested_gain==64);
 }
 h.ctrl[0].value=189;assert(capture_conditions(&state)==-EINVAL);
 h.ctrl[0].value=7152;assert(capture_conditions(&state)==-EINVAL);
 h.ctrl[0].minimum=24;h.ctrl[0].maximum=6000;h.ctrl[0].value=24;assert(!capture_conditions(&state));
 h.ctrl[0].value=23;assert(capture_conditions(&state)==-EINVAL);
 h.ctrl[0].minimum=4000;h.ctrl[0].maximum=5000;h.ctrl[0].value=3999;assert(capture_conditions(&state)==-EINVAL);
 h.ctrl[0].value=4000;assert(!capture_conditions(&state));
 h.ctrl[3].value=1;assert(capture_conditions(&state)==-EINVAL);h.ctrl[3].value=0;
 h.ctrl[3].id=0;assert(capture_conditions(&state)==-ENOENT);h.ctrl[3].id=4;
 state.sensor_on=true;assert(capture_conditions(&state)==-EPERM);state.sensor_on=false;
 state.seninf_on=true;assert(capture_conditions(&state)==-EPERM);state.seninf_on=false;
 sensor.ctrl_handler=NULL;assert(capture_conditions(&state)==-EPERM);
 return 0;
}
'''
oldfn=fn.replace('v4l2_ctrl_g_ctrl(vblank)<vblank->minimum||\n    v4l2_ctrl_g_ctrl(vblank)>vblank->maximum||','v4l2_ctrl_g_ctrl(vblank)<1258||')
assert oldfn!=fn
oldbody='''int main(void){struct v4l2_ctrl_handler h={.ctrl={{1,887,190,7151},{2,430,4,7150},{3,64,16,248},{4,0,0,1}}};struct v4l2_subdev sensor={.ctrl_handler=&h};struct integrated_state state={.graph={.sensor=&sensor}};assert(capture_conditions(&state)==-EINVAL);return 0;}'''
with tempfile.TemporaryDirectory(prefix='duet-fps-conditions-') as tmp:
 d=Path(tmp)
 for name,f,b in [('old',oldfn,oldbody),('new',fn,body)]:
  c=d/(name+'.c');c.write_text(head+f+b);exe=d/name
  subprocess.run(['clang','-std=gnu11','-DDCV_RAW_HOST','-pthread','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(r),'-I'+str(r/'tests'),str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=15)
assert 'v4l2_ctrl_s_ctrl' not in fn
print(json.dumps(dict(status='PASS',actual_function='capture_conditions',old_rejects_valid_20fps=True,front_blank_cases=8,dynamic_sensor_mode_ranges=True,invalid_bounds_and_test_pattern_rejected=True,controls_not_changed=True,scope='Synthetic control/PM wrappers, not hardware or lock/concurrency proof')))
