/* SPDX-License-Identifier: GPL-2.0-only */
#include "pipeline-shim.h"
#include "duet_p1_workers.h"
#include "duet_p1_handoff.h"
_Thread_local struct task_struct *current_task;
int fail_create,create_calls;
atomic_int tasks_live,queues_live;
struct test {
    struct dpw_session w;
    struct dph_handoff h;
    struct completion copy_enter,copy_leave,capture_exit;
    atomic_int copying,input_off,returned,gated,published;
    unsigned mode;
};
static void close_session(void *p){struct test*t=p;dph_close(&t->h);}
static int start_session(void*p){(void)p;return 0;}
static int capture(void*p){struct test*t=p;int ret=0;for(unsigned i=0;i<24&&!ret;i++)ret=dph_send(&t->h,i);complete_all(&t->capture_exit);return ret;}
static int publish_one(void*p,u64 n){struct test*t=p;CHECK(n==(u64)atomic_load(&t->published));atomic_store(&t->copying,1);if(t->mode==1&&n==0){complete_all(&t->copy_enter);wait_for_completion(&t->copy_leave);}atomic_store(&t->copying,0);atomic_fetch_add(&t->published,1);return t->mode==2?-EIO:0;}
static int publish(void*p){struct test*t=p;return dph_run(&t->h,publish_one,t);}
static int off(void*p){struct test*t=p;atomic_fetch_add(&t->input_off,1);return 0;}
static int cpu(void*p,bool committed){struct test*t=p;CHECK(committed);CHECK(atomic_load(&t->input_off)==1);CHECK(!atomic_load(&t->copying));CHECK(!t->w.capture_task&&!t->w.publish_task);atomic_fetch_add(&t->returned,1);return 0;}
static int gate(void*p){struct test*t=p;CHECK(atomic_load(&t->returned)==1);atomic_fetch_add(&t->gated,1);return 0;}
static const struct dpw_ops ops={close_session,start_session,capture,publish,off,cpu,gate};
static void destroy_completion(struct completion*c){CHECK(!pthread_mutex_destroy(&c->lock));CHECK(!pthread_cond_destroy(&c->cv));}
static void run(unsigned mode){struct test t={.mode=mode};create_calls=0;dph_init(&t.h);init_completion(&t.copy_enter);init_completion(&t.copy_leave);init_completion(&t.capture_exit);CHECK(!dpw_init(&t.w,&ops,&t));CHECK(!dpw_start(&t.w));if(mode==1){wait_for_completion(&t.copy_enter);dpw_request_stop(&t.w,0);wait_for_completion(&t.capture_exit);CHECK(atomic_load(&t.copying));CHECK(!atomic_load(&t.returned)&&!atomic_load(&t.gated));CHECK(dph_send(&t.h,99)==-ECANCELED);complete_all(&t.copy_leave);}wait_for_completion(&t.w.stop_done);int ret=dpw_drain(&t.w);CHECK(mode?ret<0:ret==0);CHECK(t.w.drained&&t.w.state==DPW_STOPPED);CHECK(atomic_load(&t.gated)==1&&atomic_load(&t.returned)==1);CHECK(!t.h.active&&!t.h.pending);if(!mode)CHECK(t.h.submitted==24&&t.h.completed==24&&atomic_load(&t.published)==24);CHECK(!atomic_load(&tasks_live)&&!atomic_load(&queues_live));CHECK(!pthread_mutex_destroy(&t.h.lock));CHECK(!pthread_mutex_destroy(&t.w.lock));destroy_completion(&t.h.ready);destroy_completion(&t.h.done);destroy_completion(&t.w.start_done);destroy_completion(&t.w.workers_go);destroy_completion(&t.w.stop_done);destroy_completion(&t.copy_enter);destroy_completion(&t.copy_leave);destroy_completion(&t.capture_exit);}
static int never_publish(void*p,u64 n){(void)p;(void)n;CHECK(false);return -EIO;}
int main(void){for(unsigned i=0;i<40;i++)for(unsigned mode=0;mode<3;mode++)run(mode);struct dph_handoff h={0};dph_init(&h);CHECK(dph_send(&h,0)==-ETIMEDOUT);CHECK(h.closed&&h.pending);CHECK(dph_run(&h,never_publish,NULL)==0);CHECK(!h.pending&&!h.active);CHECK(dph_send(&h,1)==-ECANCELED);puts("{\"status\":\"PASS\",\"executor_mailbox_runs\":120,\"sender_timeout\":true,\"copy_close_join_boundary\":true}");return 0;}
