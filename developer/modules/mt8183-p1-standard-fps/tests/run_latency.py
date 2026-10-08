#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual COPY/delivery/reclaim callsites; fault injection, no camera pixels."""
from pathlib import Path
import re,subprocess,tempfile
m=Path(__file__).resolve().parents[1]
s=(m/'duet_p1_hw.c').read_text()
def fn(name):
 a=re.search(r'^(?:static )?int '+name+r'\(',s,re.M).start();e=s.index('{',a)+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint64_t u64;typedef uint32_t u32;typedef unsigned char u8;
#define DPS_MAX_FW_SEQUENCE 1000
#define DUET_P1_LOG_FRAMES 12
#define DPS_READ_COMPARE 1
#define DPS_READ_COPY 0
#define DPS_RETURN_DONE 0
struct dps_key {u64 logical;};struct dps_mapping {struct dps_key key;unsigned span;};
struct dps_read_token {int kind;};struct dpa_sample {u64 time_ns;};
struct dps_publish_token {int dummy;};struct dps_return_token {int kind;struct {unsigned index;} ticket;};struct dps_ticket {int dummy;};
struct duet_p1_frame_log {u64 sof_ns,copy_begin_ns,copy_end_ns,compare_begin_ns,compare_end_ns,delivery_ns,published_ns;bool copied,compared,match,delivered,archived,reclaimed;};
struct duet_p1_hw {int lock,error,adapter;struct {u64 epoch;} stream;void *output_cpu,*copy_cpu,*compare_cpu;size_t frame_stride,frame_bytes;bool copy_pending,publish_pending;struct duet_p1_frame_log frames[12];struct {unsigned archived_frames;} observed;int (*deliver)(void *,const u8 *,size_t,u64,u64);void *deliver_context;};
static int begin_error,end_error,delivery_error,delivered,read_kind;static u64 ticks;
#define spin_lock_irqsave(p,f) do{f=0;assert(!*p);*p=1;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)f;assert(*p);*p=0;}while(0)
static u64 ktime_get_ns(void){return ++ticks;}
static void sample_locked(struct duet_p1_hw *h,struct dpa_sample *p){assert(h->lock);p->time_ns=ktime_get_ns();}
static int hold_locked(struct duet_p1_hw *h,int r){assert(h->lock);h->error=r;return r;}
static int dps_mapping(u64 epoch,u64 logical,struct dps_mapping *m){assert(epoch==1);m->key.logical=logical;m->span=logical%6;return 0;}
static int dpa_read_begin(int *a,struct dps_key *k,int kind,struct dpa_sample *p,struct dps_read_token *t){(void)a;(void)k;(void)p;t->kind=read_kind=kind;return begin_error;}
static int dpa_read_end(int *a,struct dps_read_token *t,struct dpa_sample *p,int r,size_t n,bool match){(void)a;(void)t;(void)p;assert(!r&&n==8);return end_error?:(!match?-EPROTO:0);}
static void dma_rmb(void){}
static int dpa_publish_begin(int *a,struct dps_key *k,struct dps_publish_token *t){(void)a;(void)k;(void)t;return 0;}
static int dpa_publish_end(int *a,struct dps_publish_token *t,int r,size_t n,struct dps_return_token *o){(void)a;(void)t;assert(n==(r?0:8));o->kind=0;return r;}
static int dpa_return_end(int *a,struct dps_return_token *t){(void)a;(void)t;return 0;}
static int dps_buffer_ticket(void *s,unsigned i,struct dps_ticket *t){(void)s;(void)i;(void)t;return 0;}
static int dps_buffer_arrive(void *s,struct dps_ticket *t,struct dps_return_token *r){(void)s;(void)t;(void)r;return 0;}
static int dpa_reclaim(int *a,struct dps_key *k,bool b){(void)a;(void)k;assert(b);return 0;}
static int deliver(void *ctx,const u8 *bytes,size_t n,u64 logical,u64 ts){struct duet_p1_hw *h=ctx;assert(!h->lock&&h->copy_pending&&!h->publish_pending);assert(read_kind==DPS_READ_COPY&&n==8&&ts==77);assert(h->frames[logical%12].copied&&!h->frames[logical%12].compared);assert(bytes==(u8*)h->copy_cpu+(logical%6)*8);delivered++;return delivery_error;}
'''
unit+=fn('read_image')+'\n'+fn('duet_p1_hw_publish_image')+r'''
int main(void){unsigned char dma[48],shadow[48],compare[8];memset(dma,4,sizeof dma);memset(shadow,0,sizeof shadow);
 struct duet_p1_hw h={.stream={.epoch=1},.output_cpu=dma,.copy_cpu=shadow,.compare_cpu=compare,.frame_stride=8,.frame_bytes=8,.deliver=deliver};h.deliver_context=&h;h.frames[0].sof_ns=77;
 begin_error=-EIO;assert(read_image(&h,0,false)==-EIO&&!delivered&&!h.copy_pending);begin_error=0;
 end_error=-EPROTO;assert(read_image(&h,0,false)==-EPROTO&&!delivered&&!h.copy_pending);end_error=0;
 delivery_error=-EFAULT;assert(read_image(&h,0,false)==-EFAULT&&delivered==1&&!h.copy_pending&&!h.frames[0].delivered);delivery_error=0;
 assert(!read_image(&h,0,false)&&delivered==2&&h.frames[0].delivered&&!h.copy_pending);
 assert(!memcmp(dma,shadow,8));assert(!read_image(&h,0,true)&&delivered==2&&h.frames[0].compared);
 assert(!duet_p1_hw_publish_image(&h,0)&&delivered==2&&h.frames[0].archived&&h.frames[0].reclaimed&&!h.publish_pending);
 shadow[0]^=1;assert(read_image(&h,0,true)==-EPROTO&&delivered==2&&!h.copy_pending);
 h.frames[0].delivered=false;assert(duet_p1_hw_publish_image(&h,0)==-EPROTO&&delivered==2&&!h.publish_pending);
 puts("LOW_LATENCY_ACTUAL_COPY_AND_COMPLETION_FAULTS_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='duet-latency-') as t:
 p=Path(t);(p/'test.c').write_text(unit)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
