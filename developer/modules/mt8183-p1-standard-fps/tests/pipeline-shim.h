/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WORKERS_SHIM_H
#define WORKERS_SHIM_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
typedef uint64_t u64;
#define msecs_to_jiffies(x) (x)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define IS_ERR(p) ((intptr_t)(p)<0 && (intptr_t)(p)>-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define WQ_MEM_RECLAIM 1
#define CHECK(v) do {if(!(v)){fprintf(stderr,"check failed %s:%d %s\n",__FILE__,__LINE__,#v);abort();}}while(0)
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(p) CHECK(!pthread_mutex_init((p),NULL))
#define spin_lock_irqsave(p,f) do{(f)=0;CHECK(!pthread_mutex_lock(p));}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);CHECK(!pthread_mutex_unlock(p));}while(0)
struct completion {pthread_mutex_t lock;pthread_cond_t cv;unsigned done;};
static inline void init_completion(struct completion *c){CHECK(!pthread_mutex_init(&c->lock,NULL));CHECK(!pthread_cond_init(&c->cv,NULL));c->done=0;}
static inline void reinit_completion(struct completion *c){CHECK(!pthread_mutex_lock(&c->lock));c->done=0;CHECK(!pthread_mutex_unlock(&c->lock));}
static inline void complete_all(struct completion *c){CHECK(!pthread_mutex_lock(&c->lock));c->done=UINT_MAX;CHECK(!pthread_cond_broadcast(&c->cv));CHECK(!pthread_mutex_unlock(&c->lock));}
static inline void complete(struct completion *c){CHECK(!pthread_mutex_lock(&c->lock));if(c->done!=UINT_MAX)c->done++;CHECK(!pthread_cond_signal(&c->cv));CHECK(!pthread_mutex_unlock(&c->lock));}
static inline void wait_for_completion(struct completion *c){CHECK(!pthread_mutex_lock(&c->lock));while(!c->done)CHECK(!pthread_cond_wait(&c->cv,&c->lock));if(c->done!=UINT_MAX)c->done--;CHECK(!pthread_mutex_unlock(&c->lock));}
static inline unsigned long wait_for_completion_timeout(struct completion*c,unsigned long ms){struct timespec t;CHECK(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=(time_t)(ms/1000);t.tv_nsec+=(long)(ms%1000)*1000000;if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}CHECK(!pthread_mutex_lock(&c->lock));int ret=0;while(!c->done&&!ret)ret=pthread_cond_timedwait(&c->cv,&c->lock,&t);CHECK(!ret||ret==ETIMEDOUT);unsigned long ok=!!c->done;if(ok&&c->done!=UINT_MAX)c->done--;CHECK(!pthread_mutex_unlock(&c->lock));return ok;}
struct task_struct {pthread_t thread;pthread_mutex_t lock;pthread_cond_t cv;atomic_bool stop;bool woken;int (*fn)(void*);void *data;int result;atomic_int refs;};
extern _Thread_local struct task_struct *current_task;
extern int fail_create,create_calls;
extern atomic_int tasks_live,queues_live;
static inline void *task_entry(void *p){struct task_struct*t=p;current_task=t;CHECK(!pthread_mutex_lock(&t->lock));while(!t->woken)CHECK(!pthread_cond_wait(&t->cv,&t->lock));CHECK(!pthread_mutex_unlock(&t->lock));t->result=atomic_load(&t->stop)?-EINTR:t->fn(t->data);return NULL;}
static inline struct task_struct *kthread_create(int(*fn)(void*),void*p,const char*name){(void)name;if(++create_calls==fail_create)return (void*)(intptr_t)-ENOMEM;struct task_struct*t=calloc(1,sizeof(*t));CHECK(t);CHECK(!pthread_mutex_init(&t->lock,NULL));CHECK(!pthread_cond_init(&t->cv,NULL));atomic_init(&t->refs,1);atomic_init(&t->stop,false);t->fn=fn;t->data=p;CHECK(!pthread_create(&t->thread,NULL,task_entry,t));atomic_fetch_add(&tasks_live,1);return t;}
static inline void wake_up_process(struct task_struct*t){CHECK(t);CHECK(!pthread_mutex_lock(&t->lock));t->woken=true;CHECK(!pthread_cond_broadcast(&t->cv));CHECK(!pthread_mutex_unlock(&t->lock));}
static inline bool kthread_should_stop(void){CHECK(current_task);return atomic_load(&current_task->stop);}
static inline void get_task_struct(struct task_struct*t){atomic_fetch_add(&t->refs,1);}
static inline void put_task_struct(struct task_struct*t){if(atomic_fetch_sub(&t->refs,1)==1){CHECK(!pthread_mutex_destroy(&t->lock));CHECK(!pthread_cond_destroy(&t->cv));free(t);atomic_fetch_sub(&tasks_live,1);}}
static inline int kthread_stop(struct task_struct*t){CHECK(!pthread_equal(pthread_self(),t->thread));atomic_store(&t->stop,true);wake_up_process(t);CHECK(!pthread_join(t->thread,NULL));int r=t->result;put_task_struct(t);return r;}
struct work_struct {void(*fn)(struct work_struct*);pthread_t thread;bool queued,joined;};
struct workqueue_struct {int unused;};
#define INIT_WORK(w,f) do{(w)->fn=(f);(w)->queued=false;(w)->joined=false;}while(0)
static inline struct workqueue_struct *alloc_ordered_workqueue(const char*n,int flags){(void)n;(void)flags;struct workqueue_struct*q=calloc(1,sizeof(*q));CHECK(q);atomic_fetch_add(&queues_live,1);return q;}
static inline void *work_entry(void*p){struct work_struct*w=p;w->fn(w);return NULL;}
static inline bool queue_work(struct workqueue_struct*q,struct work_struct*w){CHECK(q&&!w->queued);w->queued=true;CHECK(!pthread_create(&w->thread,NULL,work_entry,w));return true;}
static inline void flush_work(struct work_struct*w){CHECK(w->queued);if(!w->joined){CHECK(!pthread_join(w->thread,NULL));w->joined=true;}}
static inline void destroy_workqueue(struct workqueue_struct*q){CHECK(q);free(q);atomic_fetch_sub(&queues_live,1);}
#endif
