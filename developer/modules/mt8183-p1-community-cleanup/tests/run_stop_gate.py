#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Extract real stop/gate functions; fake providers test error propagation only."""
from pathlib import Path
import re, subprocess, tempfile, json
root = Path(__file__).resolve().parents[1]
source = (root/'duet_p1_continuous.c').read_text()
def extract(name):
    m = re.search(r'^static int '+name+r'\(', source, re.M); assert m
    e = source.index('{', m.start())+1; depth = 1
    while depth:
        depth += (source[e]=='{')-(source[e]=='}'); e += 1
    return source[m.start():e]
head = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define READ_ONCE(x) (x)
#define RPROC_RUNNING 1
#define RPROC_OFFLINE 0
struct duet_p1_hw_snapshot {int unused;};
struct mtk_isp_scp_p1_cmd {int unused;};
struct hw {int error;};
struct integrated_state {
 bool stopped, stopping, capture_attempted, scp_owned, ipi_registered[2], stop_verified;
 struct hw hw;
 struct {bool irq_owned, pm_ref, clocks_on; int irq;} resources;
 int irq_enabled, stage, stop_error;
 void *rproc, *scp_api;
 struct {int first_error, input_error, cpu_error;} workers;
};
static int input_error, shutdown_error, gate_error, snapshot_error, disarm_error;
static int encode_error, send_error, finalize_error, power, run;
static int inputs, masked, synced, latched, shutdowns, unregistered, gated, finalized, disarmed, sent;
static void duet_p1_hw_stopping(struct hw *h) {(void)h;}
static int capture_inputs_stop(struct integrated_state *s) {(void)s; inputs++;return input_error;}
static int atomic_xchg(int *p,int n) {int old=*p;*p=n;return old;}
static void disable_irq(int irq) {(void)irq;masked++;}
static void synchronize_irq(int irq) {(void)irq;synced++;}
static void duet_p1_hw_irq(struct hw *h) {(void)h;latched++;}
static bool rproc_is(struct integrated_state *s,int p,int r) {(void)s;return power==p&&run==r;}
static int rproc_shutdown(void *r) {(void)r;shutdowns++;if(shutdown_error)return shutdown_error;power=0;run=0;return 0;}
static void scp_ipi_unregister(void *scp,int id) {(void)scp;assert(id==10||id==11);unregistered++;}
static int cam_put_and_gate(struct integrated_state *s) {(void)s;gated++;return gate_error;}
static int duet_p1_hw_snapshot(struct hw *h,struct duet_p1_hw_snapshot *s) {(void)h;(void)s;return snapshot_error;}
static int stream_disarm(struct integrated_state *s) {(void)s;disarmed++;return disarm_error;}
static int duet_p1_encode_deinit(struct mtk_isp_scp_p1_cmd *c) {(void)c;return encode_error;}
static int send_cmd(struct integrated_state *s,struct mtk_isp_scp_p1_cmd *c) {(void)s;(void)c;sent++;return send_error;}
static void duet_p1_hw_hold(struct hw *h,int e) {h->error=e;}
static int duet_p1_hw_ring_finalize(struct hw *h,bool off) {(void)h;assert(off);finalized++;return finalize_error;}
'''
# The real callback uses the actual per-channel firmware IDs.
head = head.replace('int irq_enabled, stage, stop_error;', 'int irq_enabled, stage, stop_error; struct {int id;} channel[2];')
body = r'''
static struct integrated_state fresh(void) {
 input_error=shutdown_error=gate_error=snapshot_error=disarm_error=encode_error=send_error=finalize_error=0;
 inputs=masked=synced=latched=shutdowns=unregistered=gated=finalized=disarmed=sent=0;
 power=1;run=1;
 return (struct integrated_state){.capture_attempted=true,.scp_owned=true,.ipi_registered={true,true},.channel={{10},{11}},.irq_enabled=1,.resources={true,true,true,23}};
}
int main(void) {
 struct integrated_state s=fresh(); assert(worker_gate(&s)==0);
 assert(s.stop_verified&&s.stopping&&!s.scp_owned&&!s.ipi_registered[0]&&!s.ipi_registered[1]);
 assert(s.stop_error==0&&inputs==1&&masked==1&&latched==1&&shutdowns==1&&unregistered==2&&gated==1&&finalized==1);
 s=fresh(); input_error=-EIO; assert(worker_gate(&s)==-EIO); assert(!s.stop_verified&&!masked&&!shutdowns&&!gated&&!finalized);
 s=fresh(); shutdown_error=-ETIMEDOUT; assert(worker_gate(&s)==-ETIMEDOUT); assert(!s.stop_verified&&s.scp_owned&&!unregistered&&!gated&&!finalized);
 s=fresh(); power=2; assert(worker_gate(&s)==-EBUSY); assert(!shutdowns&&!gated&&!finalized);
 s=fresh(); s.scp_owned=false; assert(worker_gate(&s)==-EBUSY); assert(!unregistered&&!gated&&!finalized);
 s=fresh(); gate_error=-EUCLEAN; assert(worker_gate(&s)==-EUCLEAN); assert(!s.stop_verified&&!finalized);
 s=fresh(); snapshot_error=-EPROTO; assert(worker_gate(&s)==-EPROTO); assert(s.stop_verified&&!disarmed&&!sent&&!finalized);
 s=fresh(); disarm_error=-ENXIO; assert(worker_gate(&s)==-ENXIO); assert(s.stop_verified&&!sent&&!finalized);
 s=fresh(); encode_error=-EINVAL; assert(worker_gate(&s)==-EINVAL); assert(s.stop_verified&&!sent&&!finalized);
 s=fresh(); send_error=-ETIMEDOUT; assert(worker_gate(&s)==-ETIMEDOUT); assert(s.stop_verified&&sent==1&&!finalized);
 s=fresh(); s.workers.first_error=-EPIPE; gate_error=-EUCLEAN; assert(worker_gate(&s)==-EPIPE); assert(s.stop_error==-EUCLEAN&&!s.stop_verified&&!finalized);
 s=fresh(); s.workers.input_error=-ENODEV; assert(worker_gate(&s)==-ENODEV); assert(s.stop_verified&&!finalized);
 s=fresh(); s.workers.cpu_error=-ENOMEM; assert(worker_gate(&s)==-ENOMEM); assert(s.stop_verified&&!finalized);
 s=fresh(); finalize_error=-EIO; assert(worker_gate(&s)==-EIO); assert(s.stop_verified&&finalized==1);
 s=fresh(); s.irq_enabled=0; assert(worker_gate(&s)==0); assert(!masked&&synced==1&&finalized==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='duet-p1-stop-') as tmp:
    c=Path(tmp)/'stop.c';exe=Path(tmp)/'stop'
    c.write_text(head+extract('stream_stop')+extract('worker_gate')+body)
    subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)
print(json.dumps(dict(status='PASS',actual_functions=['stream_stop','worker_gate'],cases=15,scope='Synthetic hardware/provider wrappers; real stop proof and concurrency require hardware validation')))
