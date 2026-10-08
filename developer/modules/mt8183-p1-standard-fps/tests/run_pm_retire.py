#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual PM/retire/rearm C bodies: proof invalidation on resume and failure retention."""
from pathlib import Path
import re,tempfile,subprocess
r=Path(__file__).resolve().parents[1];s=(r/'duet_p1_continuous.c').read_text()
def fn(n):
 m=re.search(r'^static int '+n+r'\([^;]*\)\n\{',s,re.M);assert m,n
 i=s.index('{',m.start())+1;depth=1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[m.start():i]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#define PM_SUSPEND_PREPARE 1
#define PM_POST_SUSPEND 2
#define NOTIFY_OK 0
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct notifier_block{int unused;};
typedef long long s64;typedef unsigned long long u64;
#define DPW_STOPPED 4
#define RPROC_OFFLINE 0
#define TEST_SIZE 64
#define COUNT(x) ((void)0)
#define dev_info(...) ((void)0)
#define THIS_MODULE ((void*)1)
struct child{void *owner,*dev;};
struct hw{bool finalized;int error;struct{unsigned copy_allocations,copy_frees;}observed;struct{int bayer_id;}profile;};
struct integrated_state{
 struct{bool queue_initialized,running;int lock;unsigned width,height;}video;
 struct{bool pm_ref,clocks_on;void *dev,*base;int irq;}resources;
 struct{bool drained;int state;void *capture_task,*publish_task,*coordinator;}workers;
 struct{bool active,pending;}handoff;
 struct{struct child *sensor,*seninf;bool sensor_pinned,seninf_pinned,prepared;}graph;
 struct{void *cpu;u64 cam_iova;}composer;
 struct{unsigned count,len;char bytes[4];int reply;}channel[2];
 struct hw hw;struct notifier_block system_notifier;bool system_sleep_pending;bool stream_module_ref,workers_active,stop_verified,transport_failed,inputs_idle,sensor_on,seninf_on,scp_owned,ipi_registered[2],capture_attempted,fw_exposed,capture_ok,retain_capture,stopping,video_needs_prepare,cycle_verified;
 int irq_enabled,control_lock,capture_error,input_error,control_error,send_error,stop_error,session_irq_calls,sensor_code;
 u64 session_epoch,retired_epoch;unsigned retired_sessions,retired_cpu_allocations,retired_cpu_frees,stage,commands,cq_descriptor,cq_source,cq_shared_sequence,cq_shared_base,pre_frame_sequence,rearms;
 char stop_snapshot[4];
};
struct device{struct integrated_state *state;};static struct integrated_state *current;static struct child sensor,seninf;
static s64 session_epochs;static bool proof;static int releases,pm_acquisitions,prepares,pins,fail_prepare;static bool runtime_disabled;
static int atomic_read(int*p){return *p;}static void atomic_set(int*p,int v){*p=v;}static s64 atomic64_inc_return(s64*p){return ++*p;}
static void mutex_lock(int*p){assert(!*p);}
static int notifier_from_errno(int r){return r;}
static int mutex_trylock(int*p){return !*p;}static void mutex_unlock(int*p){(void)p;}
static struct integrated_state *dev_get_drvdata(struct device*d){return d->state;}
static bool try_module_get(void*p){(void)p;pins++;return true;}static void module_put(void*p){(void)p;}static void put_device(void*p){(void)p;}
static bool rproc_is(struct integrated_state*s,int p,int mode){assert(s==current&&!p&&!mode);return true;}
static int capture_child_idle(void*p){(void)p;return runtime_disabled?-EAGAIN:0;}
static int cam_put_and_gate(struct integrated_state*s){assert(s==current);return proof?0:-ETIMEDOUT;}
static void synchronize_irq(int n){(void)n;}static void dma_wmb(void){}static void reinit_completion(int*p){*p=0;}
static int duet_p1_hw_release(struct hw*h,bool gate){assert(proof&&gate&&h->finalized&&!h->error);releases++;h->observed.copy_frees=3;return 0;}
static int pm_runtime_resume_and_get(void*d){(void)d;pm_acquisitions++;current->resources.clocks_on=true;proof=false;return 0;}
static int duet_p1_hw_prepare_profile(struct hw*h,void*d,void*b,u64 c,u64 e,unsigned w,unsigned y){(void)d;(void)b;(void)c;(void)e;assert(w&&y&&h==&current->hw&&current->resources.pm_ref);prepares++;return fail_prepare?-ENOMEM:0;}
static int dcv_bayer_index(int n){return n;}
static int video_retire(struct integrated_state*s);
'''
suffix=r'''
static void init(struct integrated_state*s){
 memset(s,0,sizeof(*s));current=s;proof=true;releases=pm_acquisitions=prepares=pins=fail_prepare=0;session_epochs=7;runtime_disabled=false;
 s->video.queue_initialized=true;s->video.width=1632;s->video.height=1224;s->workers.drained=true;s->workers.state=DPW_STOPPED;
 s->stop_verified=s->hw.finalized=s->inputs_idle=s->capture_attempted=s->cycle_verified=true;s->session_epoch=7;
 s->hw.observed.copy_allocations=3;s->graph.sensor=&sensor;s->graph.seninf=&seninf;s->graph.sensor_pinned=s->graph.seninf_pinned=s->graph.prepared=true;s->composer.cpu=calloc(1,TEST_SIZE);assert(s->composer.cpu);
}
int main(void){struct integrated_state s;struct device d={&s};unsigned cases=0;
#define BUSY(field) do{init(&s);s.field=true;assert(resources_system_prepare(&d)==-EBUSY&&!releases&&!pm_acquisitions&&!prepares&&s.capture_attempted);free(s.composer.cpu);cases++;}while(0)
 BUSY(video.running);BUSY(stream_module_ref);BUSY(workers_active);BUSY(scp_owned);BUSY(sensor_on);BUSY(seninf_on);BUSY(resources.pm_ref);BUSY(resources.clocks_on);BUSY(irq_enabled);BUSY(capture_error);BUSY(input_error);BUSY(transport_failed);
 init(&s);proof=false;assert(resources_system_prepare(&d)==-ETIMEDOUT&&!releases&&s.transport_failed&&s.capture_error==-ETIMEDOUT&&s.stream_module_ref&&pins==1);free(s.composer.cpu);cases++;
 init(&s);s.workers.drained=false;assert(resources_system_prepare(&d)==-EBUSY&&!releases&&s.transport_failed);free(s.composer.cpu);cases++;
 init(&s);assert(!resources_system_prepare(&d)&&releases==1&&!pm_acquisitions&&!prepares&&!s.capture_attempted&&!s.cycle_verified&&s.video_needs_prepare&&s.session_epoch==8&&s.retired_cpu_frees==3);proof=false; /* actual system PRE_ON invalidates previous witness */
 assert(!video_rearm(&s)&&pm_acquisitions==1&&prepares==1&&releases==1&&!s.video_needs_prepare&&!s.transport_failed&&!s.cycle_verified&&s.resources.pm_ref);free(s.composer.cpu);cases++;
 init(&s);assert(!resources_system_prepare(&d));proof=false;fail_prepare=1;assert(video_rearm(&s)==-ENOMEM&&s.transport_failed&&s.video_needs_prepare);free(s.composer.cpu);cases++;
 init(&s);assert(!video_rearm(&s)&&releases==1&&pm_acquisitions==1&&prepares==1&&s.cycle_verified);free(s.composer.cpu);cases++;
 init(&s);s.resources.dev=&d;assert(!resources_system_notify(&s.system_notifier,PM_SUSPEND_PREPARE,NULL)&&s.system_sleep_pending&&releases==1);runtime_disabled=true;proof=false;assert(!resources_system_prepare(&d)&&releases==1);assert(!resources_system_notify(&s.system_notifier,PM_POST_SUSPEND,NULL)&&!s.system_sleep_pending);assert(!video_rearm(&s)&&prepares==1&&pm_acquisitions==1&&!s.cycle_verified);free(s.composer.cpu);cases++;
 init(&s);s.resources.dev=&d;s.video.running=true;assert(resources_system_notify(&s.system_notifier,PM_SUSPEND_PREPARE,NULL)==-EBUSY&&!s.system_sleep_pending&&!releases);free(s.composer.cpu);cases++;
 init(&s);s.resources.dev=&d;assert(!resources_system_notify(&s.system_notifier,PM_SUSPEND_PREPARE,NULL));assert(!resources_system_notify(&s.system_notifier,PM_POST_SUSPEND,NULL)&&!s.system_sleep_pending);assert(!video_rearm(&s)&&prepares==1&&releases==1);free(s.composer.cpu);cases++;
 printf("PASS %u actual C PM-retire/rearm cases; guard/proof preserved; no camera/kernel execution\n",cases);return 0;}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);c=p/'test.c';c.write_text(prefix+fn('resources_system_prepare')+fn('resources_system_notify')+fn('video_retire')+fn('video_rearm')+suffix)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
