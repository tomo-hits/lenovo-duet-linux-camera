/* Modified on 2026-10-05: explicit publication license. */
/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ov02a10-telemetry.h"
#include "ov02a10-pace.h"
#undef static_assert
#define static_assert(x) _Static_assert(x, #x)
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define BIT(n) (1U << (n))
#define U64_MAX UINT64_MAX
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define PAGE_SIZE 4096
#define dev_err(...) ((void)0)
#define V4L2_CID_EXPOSURE 1
#define V4L2_CID_ANALOGUE_GAIN 2
#define V4L2_CID_VBLANK 3
#define V4L2_CID_TEST_PATTERN 4

static atomic_ulong checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } atomic_fetch_add(&checks,1); } while (0)
struct mutex { pthread_mutex_t native; unsigned rank; };
static _Thread_local unsigned held_locks;
static _Thread_local bool getter_thread;
static atomic_bool getter_has_registry;
#define DEFINE_MUTEX(name) struct mutex name = {PTHREAD_MUTEX_INITIALIZER,1}
static void mutex_init(struct mutex *m) { CHECK(!pthread_mutex_init(&m->native,NULL)); m->rank=2; }
static void mutex_destroy(struct mutex *m) { CHECK(!pthread_mutex_destroy(&m->native)); }
static void mutex_lock(struct mutex *m) {
 CHECK(!(held_locks & BIT(m->rank)));
 if (m->rank==1) CHECK(!(held_locks & BIT(2)));
 CHECK(!pthread_mutex_lock(&m->native)); held_locks|=BIT(m->rank);
 if (m->rank==1 && getter_thread) atomic_store(&getter_has_registry,true);
}
static void mutex_unlock(struct mutex *m) {
 CHECK(held_locks & BIT(m->rank)); held_locks &= ~BIT(m->rank);
 CHECK(!pthread_mutex_unlock(&m->native));
}
#define lockdep_assert_held(m) CHECK(held_locks & BIT((m)->rank))
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(name) struct list_head name={&name,&name}
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h; h->prev=h; }
static void list_add_tail(struct list_head *v, struct list_head *h) {
 v->prev=h->prev; v->next=h; h->prev->next=v; h->prev=v;
}
static void list_del_init(struct list_head *v) { v->prev->next=v->next; v->next->prev=v->prev; INIT_LIST_HEAD(v); }
#define list_for_each_entry(pos,h,m) for (struct list_head *cursor=(h)->next; cursor!=(h) && ((pos)=container_of(cursor,__typeof__(*(pos)),m),true); cursor=cursor->next)
struct kobject { unsigned id; };
struct device { struct kobject kobj; };
struct device_attribute { int unused; };
struct attribute_group { int unused; };
static const struct attribute_group ov02a10_telemetry_group;
struct clk;
struct gpio_desc;
struct regulator_bulk_data { int unused; };
struct media_pad { int unused; };
struct v4l2_mbus_framefmt { u32 code; };
struct v4l2_ctrl_handler { int unused; };
struct v4l2_ctrl {
 struct v4l2_ctrl_handler *handler;
 int id,val,current;
 union { s32 *p_s32; } p_cur;
 s64 minimum,maximum,step,default_value;
};
struct v4l2_subdev { struct v4l2_ctrl_handler *ctrl_handler; void *client; };
struct i2c_client { int unused; };
static struct i2c_client client;
static struct i2c_client *v4l2_get_subdevdata(struct v4l2_subdev *s) { return s->client; }
static atomic_ullong ticks;
static u64 ktime_get_ns(void) { return atomic_fetch_add(&ticks,7)+100; }
static unsigned sleep_count;
static void usleep_range(unsigned lo, unsigned hi) { if(lo==140000) { CHECK(hi==141000); atomic_fetch_add(&ticks,140000000); return; } CHECK(lo==250 && hi==350); sleep_count++; atomic_fetch_add(&ticks,300000); }
static int sysfs_create_group(struct kobject *,const struct attribute_group *);
static void sysfs_remove_group(struct kobject *,const struct attribute_group *);
static int i2c_smbus_write_byte_data(struct i2c_client *,u8,u8);
static int i2c_smbus_read_byte_data(struct i2c_client *,u8);
static int pm_runtime_resume_and_get(struct device *);
static int pm_runtime_get_if_in_use(struct device *);
static int pm_runtime_put(struct device *);
static bool pm_runtime_active(struct device *);
static int __v4l2_ctrl_modify_range(struct v4l2_ctrl *,s64,s64,s64,s64);
static int __v4l2_ctrl_handler_setup(struct v4l2_ctrl_handler *);
static int sysfs_emit_at(char *buf,int at,const char *format,...) __attribute__((format(printf,3,4)));
static int sysfs_emit_at(char *buf,int at,const char *format,...) {
 CHECK(at>=0 && at<PAGE_SIZE);
 va_list ap; va_start(ap,format); int n=vsnprintf(buf+at,PAGE_SIZE-at,format,ap); va_end(ap);
 CHECK(n>=0); return n<PAGE_SIZE-at ? n : PAGE_SIZE-at-1;
}

#define V4L2_CID_HFLIP 101
#define V4L2_CID_VFLIP 102
#define MEDIA_BUS_FMT_SBGGR10_1X10 0x3007
#define MEDIA_BUS_FMT_SGBRG10_1X10 0x300e
#define MEDIA_BUS_FMT_SGRBG10_1X10 0x300a
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
static void __v4l2_ctrl_grab(struct v4l2_ctrl *c,bool grab){(void)c;(void)grab;}
