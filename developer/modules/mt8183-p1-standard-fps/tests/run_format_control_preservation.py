#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual capture_set_format; same format must not reset controls."""
from pathlib import Path
import tempfile,subprocess,re
r=Path(__file__).resolve().parents[1];s=(r/'duet_p1_continuous.c').read_text();a=s.index('static int capture_set_format(');b=s.index('\n/* Controlled single-owner',a);body=s[a:b]
unit=r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define V4L2_FIELD_NONE 0
struct v4l2_subdev_format {unsigned which,pad;struct {u32 width,height,code,field;} format;};
struct v4l2_subdev {struct v4l2_subdev_format fmt;unsigned sets;int vblank,exposure,gain,geterr,seterr;};
static int fake(struct v4l2_subdev *sd,int set,struct v4l2_subdev_format *f){
 if(!set){if(sd->geterr)return sd->geterr;*f=sd->fmt;return 0;}
 sd->sets++;if(sd->seterr)return sd->seterr;sd->fmt=*f;sd->vblank=1258;sd->exposure=1000;sd->gain=128;return 0;
}
#define get_fmt 0
#define set_fmt 1
#define v4l2_subdev_call(sd,group,op,state,fmt) fake(sd,op,fmt)
'''+body+r'''
int main(void){struct v4l2_subdev s={.fmt={.which=1,.format={1632,1224,0x3007,0}},.vblank=5000,.exposure=240,.gain=256};
 assert(!capture_set_format(&s,0,0x3007));assert(!s.sets&&s.vblank==5000&&s.exposure==240&&s.gain==256);
 s.geterr=-EIO;assert(capture_set_format(&s,0,0x3007)==-EIO&&!s.sets);s.geterr=0;
 s.fmt.format.width=3280;assert(!capture_set_format(&s,0,0x3007)&&s.sets==1&&s.fmt.format.width==1632);
 s.fmt.format.code=0x300f;s.seterr=-EPIPE;assert(capture_set_format(&s,0,0x3007)==-EPIPE&&s.sets==2);
 puts("REAR_ACTUAL_FORMAT_PRESERVES_OWNED_CONTROLS_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='duet-rear-format-') as t:
 p=Path(t);(p/'check.c').write_text(unit);subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True);subprocess.run([str(p/'check')],check=True)
