/* Modified on 2026-10-05: explicit publication license. */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Host boundary fixture for the exact parent session_rearm body. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef long long s64;
typedef unsigned long long u64;
#define READ_ONCE(x) (x)
#define DPW_STOPPED 4
#define RPROC_OFFLINE 0
#define DUET_P1_BURST_FRAMES 30
#define TEST_SIZE 2097152
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
#define dev_info(...) ((void)0)
static bool release_hold=true;
static s64 session_epochs;
static int pm_gets;
#define COUNT(x) (++x)
static int atomic_read(int *p){return *p;}
static void atomic_set(int *p,int v){*p=v;}
static s64 atomic64_inc_return(s64 *p){*p=(s64)((u64)*p+1);return *p;}
static void reinit_completion(int *p){*p=0;}
struct device { int sentinel; };
struct child {struct device *dev;};
struct graph {bool prepared;struct child *sensor,*seninf;int identity;};
struct resource {struct device *dev;void *base;bool pm_ref,clocks_on,irq_owned;int irq;};
struct composer {void *cpu;bool mapped;u64 cam_iova;};
struct workers {bool initialized,drained;int state;void *capture_task,*publish_task,*coordinator;};
struct handoff {bool active,pending;};
struct duet_p1_hw_snapshot {
 int error;bool capture_complete,ring_finalized,frame_pending,done_pending,refill_pending,copy_pending,allocated;
 unsigned ring_pending_sequence,ring_cpu_refs,archived_frames,copy_allocations,copy_frees;
};
struct hw {bool initialized;struct duet_p1_hw_snapshot observed;void *output_cpu,*copy_cpu,*archive_cpu,*compare_cpu;struct {int bayer_id;} profile;};
struct channel {unsigned count,len;char bytes[129];int reply,id;void *state;};
struct integrated_state {int control_lock;
 struct resource resources;struct composer composer;struct graph graph;struct workers workers;struct handoff handoff;
 struct hw hw;struct channel channel[2];
 unsigned rearms,retired_sessions,retired_cpu_allocations,retired_cpu_frees;
 int rearm_error,capture_error,input_error,control_error,send_error,stop_error;
 bool capture_ok,transport_failed,stop_verified,stopping,stopped,retain_capture,workers_active;
 bool inputs_idle,sensor_on,seninf_on,scp_owned,ipi_registered[2],smi_ref,fw_exposed,capture_attempted;
 void *scp_api;int irq_enabled,session_irq_calls;
 u64 session_epoch,retired_epoch;unsigned cq_descriptor,cq_source,cq_shared_sequence,cq_shared_base,pre_frame_sequence;
 unsigned stage,commands,sensor_code;char stop_snapshot[64];
};
static struct integrated_state *current;
static int fault,step,mutations;
static bool offline=true;
static bool rproc_is(struct integrated_state *s,int p,int state){assert(s==current&&!p&&!state);return offline;}
static int capture_child_idle(struct device *d){assert(d);return fault==1?-EIO:0;}
static int cam_put_and_gate(struct integrated_state *s){assert(s==current);if(fault==2)return -EIO;step=1;return 0;}
static void synchronize_irq(int i){assert(i==255&&step==1);step=2;}
static void duet_p1_hw_observe(struct hw *h,struct duet_p1_hw_snapshot *o){assert(step==2);*o=h->observed;}
static int duet_p1_hw_release(struct hw *h,bool gate){assert(gate&&step==2&&!current->capture_ok);mutations++;if(fault==3)return -EBUSY;step=3;h->output_cpu=h->copy_cpu=h->archive_cpu=h->compare_cpu=NULL;h->observed.copy_frees=3;return 0;}
static void dma_wmb(void){assert(step==3);step=4;}
static int pm_runtime_resume_and_get(struct device *d){
 assert(d==current->resources.dev&&step==4);assert(!current->stop_verified);for(unsigned i=0;i<sizeof(current->stop_snapshot);i++)assert(!current->stop_snapshot[i]);
 step=5;mutations++;if(fault==4)return -EIO;if(fault!=5)current->resources.clocks_on=true;return 0;
}
static int duet_p1_hw_prepare(struct hw *h,struct device *d,void *b,u64 c,u64 e){
 assert(h==&current->hw&&d==current->resources.dev&&b==current->resources.base&&c==123&&e==current->session_epoch&&step==5);
 assert(!current->stop_verified&&!current->stopping&&current->resources.pm_ref&&!h->output_cpu);mutations++;step=6;
 h->initialized=true;if(fault==6)return -ENOMEM;h->output_cpu=(void*)1;h->copy_cpu=(void*)2;h->archive_cpu=(void*)3;h->compare_cpu=(void*)4;h->observed.copy_allocations=3;return 0;
}

struct file; struct kobject; struct bin_attribute;
typedef long long loff_t;
static struct device *kobj_to_dev(struct kobject *k){(void)k;return &current->resources.dev[0];}
static void *dev_get_drvdata(struct device *d){assert(d==current->resources.dev);return current;}
static void mutex_lock(int *p){assert(p==&current->control_lock);}
static void mutex_unlock(int *p){assert(p==&current->control_lock);}
static ssize_t duet_p1_hw_frame_log(struct hw *h,char *b,loff_t o,size_t n){(void)b;(void)o;(void)n;assert(h->initialized);return 1;}
