#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Actual video start/stop module pin callsites, synthetic hardware callbacks."""
from pathlib import Path
import re, subprocess, tempfile

r = Path(__file__).resolve().parents[1]
source = (r / 'duet_p1_continuous.c').read_text()
def function(name):
    m = re.search(r'^static int ' + name + r'\(', source, re.M)
    assert m, name
    end = source.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]

unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define THIS_MODULE ((void *)1)
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define READ_ONCE(x) (x)
#define smp_store_release(p,v) (*(p)=(v))
struct mutex {int unused;};
struct observed {unsigned sof_count,done_count;};
struct hw {int error;bool finalized;struct observed observed;void *deliver,*deliver_context,*frame_start,*frame_start_context;unsigned bayer_id;struct {unsigned bayer_id;} profile;};
struct workers {bool initialized,drained;int stop_done;int fail;void *capture_task,*publish_task;};
struct integrated_state {struct mutex control_lock;bool stream_module_ref,cycle_verified,capture_attempted,retain_capture,workers_active,transport_failed,capture_ok,stop_verified,inputs_idle,scp_owned,ipi_registered[2];int capture_error,irq_enabled;uint32_t sensor_code;uint64_t session_epoch;struct {bool prepared;void *sensor;} graph;struct hw hw;struct workers workers;int handoff;struct {void *dev;} resources;struct {uint64_t delivered,dropped;} video;};
struct v4l2_subdev_format {unsigned which,pad;struct {uint32_t code;} format;};
static int fail_at, call_number, module_refs, joins;
static bool admission=true;
static int maybe_fail(void) {return ++call_number==fail_at?-EIO:0;}
static void mutex_lock(struct mutex *m) {(void)m;}
static void mutex_unlock(struct mutex *m) {(void)m;}
static bool try_module_get(void *m) {(void)m;if(!admission)return false;module_refs++;return true;}
static void module_put(void *m) {(void)m;assert(module_refs>0);module_refs--;}
static int video_rearm(struct integrated_state *s) {(void)s;return maybe_fail();}
static int stream_cycle(struct integrated_state *s) {int ret=maybe_fail();s->cycle_verified=!ret;return ret;}
static int capture_prepare(struct integrated_state *s) {int ret=maybe_fail();s->graph.prepared=!ret;return ret;}
static int get_format(struct v4l2_subdev_format *f) {f->format.code=3;return maybe_fail();}
#define v4l2_subdev_call(s,p,f,st,fmt) get_format(fmt)
static int dcv_bayer_index(uint32_t code) {return code<4?(int)code:-EINVAL;}
static int stream_boot_init(struct integrated_state *s) {(void)s;return maybe_fail();}
static int camera_worker_ops;
static void *video_deliver,*video_frame_start;
static void dph_init(int *h) {*h=0;}
static int dpw_init(struct workers *w,void *ops,void *ctx) {(void)ops;(void)ctx;int ret=maybe_fail();w->initialized=!ret;return ret;}
static void sched_set_fifo_low(void *task) {(void)task;}
static int dpw_start(struct workers *w) {int ret=maybe_fail();w->fail=ret;return ret;}
static void wait_for_completion(int *c) {(void)c;}
static int dpw_drain(struct workers *w) {joins++;w->drained=true;return w->fail;}
static void duet_p1_hw_request_stop(struct hw *h) {(void)h;}
static int atomic_read(int *v) {return *v;}
#define dev_info(...) ((void)0)
'''
unit += function('video_start') + '\n' + function('video_stop')
unit += r'''
static struct integrated_state fresh(void) {
 struct integrated_state s={0};s.stop_verified=true;s.inputs_idle=true;s.hw.finalized=true;
 return s;
}
int main(void) {
 for(int stage=1;stage<=7;stage++) {
  struct integrated_state s=fresh();module_refs=call_number=joins=0;fail_at=stage;
  assert(video_start(&s)==-EIO);assert(module_refs==1&&s.stream_module_ref);
  assert(video_start(&s)==-EBUSY);assert(module_refs==1);
  /* A fault fixture is discarded. It is never reset into a fake recovered stream. */
 }
 struct integrated_state s=fresh();fail_at=0;call_number=module_refs=joins=0;
 assert(video_start(&s)==0&&module_refs==1);assert(video_stop(&s)==0);
 assert(joins==1&&module_refs==0&&!s.stream_module_ref);
 assert(video_start(&s)==0&&module_refs==1);assert(video_stop(&s)==0&&module_refs==0);
 for(int error=0;error<6;error++) {
  s=fresh();call_number=module_refs=0;assert(video_start(&s)==0);
  if(error==0)s.hw.error=-EIO;
  if(error==1)s.stop_verified=false;
  if(error==2)s.inputs_idle=false;
  if(error==3)s.scp_owned=true;
  if(error==4)s.ipi_registered[0]=true;
  if(error==5)s.irq_enabled=1;
  assert(video_stop(&s)==-EBUSY&&module_refs==1&&s.stream_module_ref);
 }
 s=fresh();admission=false;module_refs=0;assert(video_start(&s)==-ENODEV&&module_refs==0);
 puts("PASS actual start/stop pins:7 start faults,6 stop gate faults,success/restart,going admission; mocks do not prove kernel module removal");
}
'''
with tempfile.TemporaryDirectory(prefix='duet-module-lifetime-') as tmp:
    d = Path(tmp)
    (d / 'test.c').write_text(unit)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(d / 'test.c'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
assert 'graph->subdev.owner = THIS_MODULE' in source
assert 'kthread_bind(' not in source and source.count('sched_set_fifo_low(')==2
assert 'cpufreq_cpu_get(' not in source
