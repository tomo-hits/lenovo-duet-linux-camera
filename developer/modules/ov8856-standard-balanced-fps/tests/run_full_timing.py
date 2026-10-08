#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual C mode table and timing helpers: programmed HTS must match V4L2 units."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[1];s=(r/'ov8856.c').read_text()
def function(name):
 m=re.search(r'^static u64 '+name+r'\(',s,re.M);assert m; a=m.start();e=s.index('{',a)+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
a=s.index('static const struct ov8856_reg lane_4_mode_3264x2448[]');b=s.index('\n};',a)+3
m=re.search(r'\.width = 3264,.*?\.hts = (\d+),.*?\.vts_min = (\d+),.*?\.link_freq_index = (\d+),',s,re.S);assert m
unit=r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t u64;typedef int64_t s64;typedef uint32_t u32;typedef uint8_t u8;
#define OV8856_RGB_DEPTH 10
#define OV8856_SCLK 144000000ULL
#define do_div(a,b) ((a)/=(b))
struct ov8856_reg {unsigned address,val;};
"""+s[a:b]+'\n'+function('to_rate')+'\n'+function('to_pixels_per_line')+r"""
int main(void){
 const s64 menu[]={360000000,180000000};unsigned hts=0,vts=0;
 for(unsigned i=0;i<sizeof(lane_4_mode_3264x2448)/sizeof(lane_4_mode_3264x2448[0]);i++){
  const struct ov8856_reg *r=&lane_4_mode_3264x2448[i];
  if(r->address==0x380c)hts=(hts&255)|(r->val<<8);
  if(r->address==0x380d)hts=(hts&65280)|r->val;
  if(r->address==0x380e)vts=(vts&255)|(r->val<<8);
  if(r->address==0x380f)vts=(vts&65280)|r->val;
 }
 assert(hts==MODE_HTS&&vts==MODE_VTS);assert(hts==3864);
 u64 pixels=to_pixels_per_line(menu,hts,MODE_LINK,4),rate=to_rate(menu,MODE_LINK,4);
 assert(pixels==7728&&pixels-3264==4464&&rate==288000000);
 double line=(double)pixels/rate,readout=line*2448,period=line*vts;
 assert(readout>.064&&readout<.066&&period>.099&&period<.100);
 printf("ACTUAL_OV8856_TABLE_TIMING_PASS hts=%u hblank=%llu readout_ms=%.3f minimum_period_ms=%.3f\n",hts,(unsigned long long)(pixels-3264),readout*1000,period*1000);
}
"""
unit=unit.replace('MODE_HTS',m[1]).replace('MODE_VTS',m[2]).replace('MODE_LINK',m[3])
with tempfile.TemporaryDirectory(prefix='ov8856-line-time-') as t:
 p=Path(t);(p/'check.c').write_text(unit);subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True);subprocess.run([str(p/'check')],check=True)
