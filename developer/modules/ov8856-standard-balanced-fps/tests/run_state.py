#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual upstream-based C: TRY/OPEN/GET must not change ACTIVE CFA/mode."""
from pathlib import Path
import re,tempfile,subprocess
r=Path(__file__).resolve().parents[1];s=(r/'ov8856.c').read_text()
def fn(n):
 m=re.search(r'^static int '+n+r'\(',s,re.M);assert m,n;a=m.start();e=s.index('{',a)+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
unit=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef int32_t s32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define V4L2_FIELD_NONE 0
#define V4L2_SUBDEV_FORMAT_TRY 0
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define OV8856_VTS_MAX 32767
static const unsigned ov8856_mbus_codes[]={0x3007,0x300a};
struct v4l2_mbus_framefmt {unsigned width,height,code,field;};
struct v4l2_subdev_state {struct v4l2_mbus_framefmt fmt;};
struct v4l2_subdev_format {unsigned which,pad;struct v4l2_mbus_framefmt format;};
struct v4l2_subdev_fh {struct v4l2_subdev_state *state;};
struct ov8856_mode {unsigned width,height,default_mbus_index,link_freq_index,data_lanes,vts_def,vts_min,hts;};
struct lane {struct ov8856_mode supported_modes[2];long link_freq_menu_items[2];};
struct ctrl {int val;};
struct ov8856 {struct lane *priv_lane;unsigned modes_size,cur_mbus_index;const struct ov8856_mode *cur_mode;int mutex;struct ctrl *link_freq,*pixel_rate,*vblank,*hblank;};
struct v4l2_subdev {struct ov8856 *sensor;};
static struct ov8856 *to_ov8856(struct v4l2_subdev *sd){return sd->sensor;}
static void mutex_lock(int *p){assert(!*p);*p=1;}static void mutex_unlock(int *p){assert(*p);*p=0;}
static struct v4l2_mbus_framefmt *v4l2_subdev_state_get_format(struct v4l2_subdev_state *s,unsigned pad){assert(!pad);return &s->fmt;}
static const struct ov8856_mode *nearest(const struct ov8856_mode *m,unsigned n,unsigned w,unsigned h){(void)h;assert(n==2);return w>2000?m:m+1;}
#define v4l2_find_nearest_size(m,n,x,y,w,h) nearest(m,n,w,h)
static void __v4l2_ctrl_s_ctrl(struct ctrl *p,int v){p->val=v;}static void __v4l2_ctrl_s_ctrl_int64(struct ctrl *p,long v){p->val=v;}
static void __v4l2_ctrl_modify_range(struct ctrl *p,int a,int b,int c,int d){(void)p;(void)a;(void)b;(void)c;(void)d;}
static long to_rate(const long *f,unsigned index,unsigned lanes){return f[index]*2*lanes/10;}
static long to_pixels_per_line(const long *f,unsigned hts,unsigned index,unsigned lanes){return hts*to_rate(f,index,lanes)/144000000;}
'''
unit+='\n'.join(fn(n) for n in ['ov8856_update_pad_format','ov8856_set_format','ov8856_get_format','ov8856_open'])+r'''
int main(void){struct lane lane={.supported_modes={{3280,2464,1,0,4,2488,2488,1928},{1632,1224,0,1,4,2482,2482,1932}},.link_freq_menu_items={360000000,180000000}};
 struct ctrl controls[4]={0};struct ov8856 s={.priv_lane=&lane,.modes_size=2,.cur_mode=&lane.supported_modes[1],.link_freq=&controls[0],.pixel_rate=&controls[1],.vblank=&controls[2],.hblank=&controls[3]};struct v4l2_subdev sd={&s};struct v4l2_subdev_state state={0};struct v4l2_subdev_fh fh={&state};
 for(unsigned c=0;c<2;c++){
  struct v4l2_subdev_format f={.which=1,.format={1632,1224,ov8856_mbus_codes[c],0}};
  assert(!ov8856_set_format(&sd,&state,&f));assert(s.cur_mbus_index==c&&s.cur_mode->width==1632);controls[2].val=3745;
  for(unsigned input=0;input<3;input++){
   struct v4l2_subdev_format get={.which=1,.format={0,0,input?ov8856_mbus_codes[input-1]:0,0}};
   assert(!ov8856_get_format(&sd,&state,&get));assert(get.format.code==ov8856_mbus_codes[c]&&s.cur_mbus_index==c&&controls[2].val==3745);
  }
  f.which=0;f.format=(struct v4l2_mbus_framefmt){3280,2464,ov8856_mbus_codes[1-c],0};assert(!ov8856_set_format(&sd,&state,&f));assert(state.fmt.code==ov8856_mbus_codes[1-c]&&s.cur_mbus_index==c&&s.cur_mode->width==1632&&controls[2].val==3745);
  assert(!ov8856_open(&sd,&fh));assert(s.cur_mbus_index==c&&s.cur_mode->width==1632&&controls[2].val==3745);
 }
 puts("OV8856_ACTUAL_TRY_OPEN_GET_PRESERVE_ACTIVE_BOTH_CFA_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='ov8856-state-') as t:
 p=Path(t);(p/'check.c').write_text(unit);subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-O1','-g','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True);subprocess.run([str(p/'check')],check=True)
