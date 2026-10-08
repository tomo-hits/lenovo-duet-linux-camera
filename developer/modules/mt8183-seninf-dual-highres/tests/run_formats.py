#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual receiver pad functions; all CFA orders and ACTIVE/TRY regression."""
from pathlib import Path
import re,tempfile,subprocess
m=Path(__file__).resolve().parents[1];s=(m/'mtk_seninf.c').read_text()
def fn(n):
 a=re.search(r'^static int '+n+r'\(',s,re.M).start();e=s.index('{',a)+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
unit=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define NUM_PADS 12
#define CAM_MUX_IDX_MIN 4
#define DEFAULT_WIDTH 1632
#define DEFAULT_HEIGHT 1224
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define V4L2_SUBDEV_FORMAT_TRY 0
#define V4L2_FIELD_ANY 0
#define V4L2_FIELD_NONE 1
#define MEDIA_BUS_FMT_SBGGR10_1X10 0x3007
#define MEDIA_BUS_FMT_SGBRG10_1X10 0x300e
#define MEDIA_BUS_FMT_SGRBG10_1X10 0x300a
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct v4l2_mbus_framefmt {u32 code,width,height,field,colorspace,xfer_func,ycbcr_enc,quantization;};
struct v4l2_subdev_format {unsigned pad,which;struct v4l2_mbus_framefmt format;};
struct v4l2_subdev {int dummy;};struct v4l2_subdev_state {struct v4l2_mbus_framefmt fmt[12];};
struct mtk_seninf {struct v4l2_subdev subdev;int lock;bool streaming;struct v4l2_subdev_format fmt[12];};
static const struct v4l2_mbus_framefmt mtk_seninf_default_fmt={.code=MEDIA_BUS_FMT_SRGGB10_1X10,.width=1632,.height=1224,.field=1};
static void mutex_lock(int *p){assert(!*p);*p=1;}static void mutex_unlock(int *p){assert(*p);*p=0;}
static struct v4l2_mbus_framefmt *v4l2_subdev_state_get_format(struct v4l2_subdev_state *s,unsigned p){assert(p<12);return &s->fmt[p];}
'''
a=s.index('/* The capture frontend preserves CFA');b=s.index('static int seninf_set_fmt',a);unit+=s[a:b]+fn('seninf_set_fmt')+'\n'+fn('seninf_get_fmt')+r'''
int main(void){struct mtk_seninf p={0};struct v4l2_subdev_state state={0};
 for(unsigned geometry=0;geometry<2;geometry++)for(unsigned which=0;which<2;which++)for(unsigned i=0;i<4;i++){
  struct v4l2_subdev_format f={.pad=geometry,.which=which,.format={.code=dcv_bayer_codes[i],.width=geometry?1600:1632,.height=geometry?1200:1224,.field=1}};
  assert(!seninf_set_fmt(&p.subdev,&state,&f));
  f.pad=4;assert(!seninf_get_fmt(&p.subdev,&state,&f));assert(f.format.code==dcv_bayer_codes[i]);assert(f.format.width==(geometry?1600U:1632U)&&f.format.height==(geometry?1200U:1224U));
 }
 struct v4l2_subdev_format f={.pad=0,.which=1,.format={.code=dcv_bayer_codes[0],.width=1632,.height=1224,.field=1}};
 struct v4l2_mbus_framefmt saved=p.fmt[4].format;p.streaming=true;assert(seninf_set_fmt(&p.subdev,&state,&f)==-EBUSY);assert(!memcmp(&saved,&p.fmt[4].format,sizeof saved));p.streaming=false;
 f.pad=12;assert(seninf_set_fmt(&p.subdev,&state,&f)==-EINVAL);f.pad=0;f.format.code=0;assert(seninf_set_fmt(&p.subdev,&state,&f)==-EINVAL);f.format.code=dcv_bayer_codes[0];f.format.width=800;assert(seninf_set_fmt(&p.subdev,&state,&f)==-EINVAL);
 puts("SENINF_DUAL_TWO_GEOMETRIES_FOUR_CFA_ACTIVE_TRY_PROPAGATION_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='duet-seninf-cfa-') as t:
 p=Path(t);(p/'test.c').write_text(unit)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
