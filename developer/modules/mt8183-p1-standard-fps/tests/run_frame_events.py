#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual IRQ/subscription/event functions; synthetic parser/core wrappers."""
from pathlib import Path
import re,subprocess,tempfile,json
root=Path(__file__).resolve().parents[1]
def fn(file,name):
 s=(root/file).read_text();m=re.search(r'^(?:static )?(?:void|int) '+name+r'\(',s,re.M);assert m
 e=s.index('{',m.start())+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
unit=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t u32;typedef uint64_t u64;
#define DUET_P1_LOG_FRAMES 2
#define RAW_STATUS 0
#define RAW_STATUS2 4
#define CQ_BASE 8
#define READ_ONCE(x) (x)
struct key {u64 logical;u32 fw;};
struct bq_irq_action {bool done,sof,arm;struct key done_key,sof_key;struct {u32 iova;} token;};
struct dpa_sample {u32 sequence_before,imgo_iova;u64 time_ns;};
struct duet_p1_frame_log {u64 done_ns,sof_ns;u32 sof_imgo,sof_cq_sequence;};
struct duet_p1_hw {bool initialized;int lock,error;void *base,*output_cpu;struct {u32 irq_count,irq_status,irq_status2,irq_sequence,cq_writes,sof_count,done_count,done_sequence,cq_arms_accepted;} observed;struct duet_p1_frame_log frames[2];struct {u32 sofs,dones,last_done,cq_arms_completed;} queue;int adapter,refill_ready,capture_done;bool terminal_seen;void (*frame_start)(void *,u64);void *frame_start_context;};
static struct bq_irq_action action;static int plan_error,finish_error,events,wakes;
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!*(p));*(p)=1;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(*(p));*(p)=0;}while(0)
static u32 readl(void *p){return *(u32*)p;}static void writel(u32 v,void *p){*(u32*)p=v;}
static int hold_locked(struct duet_p1_hw *h,int e){h->error=e;return e;}
static void sample_locked(struct duet_p1_hw *h,struct dpa_sample *s){(void)h;*s=(struct dpa_sample){.time_ns=123,.imgo_iova=456};}
static int dpa_irq_plan(int *p,u32 status,struct dpa_sample *s,struct bq_irq_action *a){(void)p;(void)status;(void)s;*a=action;return plan_error;}
static int dpa_irq_finish(int *a,struct bq_irq_action *b,struct dpa_sample *s,int e){(void)a;(void)b;(void)s;(void)e;return finish_error;}
static void dma_wmb(void){}static bool dpa_terminal(int *p){(void)p;return false;}static bool ready_locked(struct duet_p1_hw *h){(void)h;return false;}
static void complete(int *p){(*p)++;}static void wake_all(struct duet_p1_hw *h){(void)h;wakes++;}
static void callback(void *p,u64 logical){struct duet_p1_hw *h=p;assert(!h->lock);assert(logical==7);events++;}
#define V4L2_EVENT_FRAME_SYNC 4
struct v4l2_fh {int unused;};struct v4l2_event_subscription {u32 type,id;};struct v4l2_event {u32 type;union {struct {u32 frame_sequence;}frame_sync;}u;};struct v4l2_subdev {int unused;};
struct integrated_state {struct {struct v4l2_subdev subdev;}graph;};
static struct v4l2_event last_event;static unsigned subscriptions;
static int v4l2_event_subscribe(struct v4l2_fh *f,struct v4l2_event_subscription *s,unsigned n,void *p){(void)f;(void)s;assert(n==8&&!p);subscriptions++;return 0;}
static void v4l2_subdev_notify_event(struct v4l2_subdev *s,struct v4l2_event *e){(void)s;last_event=*e;}
'''
unit+='\n'+fn('duet_p1_hw.c','duet_p1_hw_irq')+'\n'+fn('duet_p1_continuous.c','graph_subscribe_event')+'\n'+fn('duet_p1_continuous.c','video_frame_start')
unit+=r'''
int main(void){u32 regs[3]={0};struct duet_p1_hw h={.initialized=true,.base=regs,.output_cpu=regs,.frame_start=callback};h.frame_start_context=&h;
 action.sof=true;action.sof_key=(struct key){.logical=7,.fw=8};duet_p1_hw_irq(&h);assert(events==1&&h.frames[1].sof_ns==123&&h.refill_ready==1);
 action.sof=false;duet_p1_hw_irq(&h);assert(events==1);action.sof=true;plan_error=-EIO;duet_p1_hw_irq(&h);assert(events==1&&h.error==-EIO&&wakes==1);
 h.error=0;plan_error=0;action.arm=true;finish_error=-EIO;duet_p1_hw_irq(&h);assert(events==1&&h.error==-EIO&&wakes==2);
 struct v4l2_fh fh={0};struct v4l2_subdev sd={0};struct v4l2_event_subscription sub={.type=4};assert(!graph_subscribe_event(&sd,&fh,&sub));sub.id=1;assert(graph_subscribe_event(&sd,&fh,&sub)==-EINVAL);sub.id=0;sub.type=5;assert(graph_subscribe_event(&sd,&fh,&sub)==-EINVAL);assert(subscriptions==1);
 struct integrated_state s={0};video_frame_start(&s,7);assert(last_event.type==4&&last_event.u.frame_sync.frame_sequence==7);puts("FRAME_EVENT_ACTUAL_FUNCTIONS_PASS");}
'''
with tempfile.TemporaryDirectory(prefix='raw-events-') as t:
 p=Path(t);(p/'test.c').write_text(unit)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
