/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_raw.h"
typedef uint32_t u32;typedef uint64_t u64;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define smp_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define COUNT(x) ((void)0)
struct completion {pthread_mutex_t lock;pthread_cond_t cond;bool done;};
static void init_completion(struct completion *c){assert(!pthread_mutex_init(&c->lock,NULL));assert(!pthread_cond_init(&c->cond,NULL));c->done=false;}
static void complete(struct completion *c){pthread_mutex_lock(&c->lock);c->done=true;pthread_cond_broadcast(&c->cond);pthread_mutex_unlock(&c->lock);}
static void wait_for_completion(struct completion *c){pthread_mutex_lock(&c->lock);while(!c->done)pthread_cond_wait(&c->cond,&c->lock);pthread_mutex_unlock(&c->lock);}
struct device {int unused;};struct media_entity;struct fwnode_handle {int unused;};struct media_pad {int unused;};struct media_link {int unused;};struct v4l2_async_notifier {int unused;};
struct mutex {int unused;};typedef struct mutex spinlock_t;struct list_head {int unused;};
struct vb2_v4l2_buffer {int unused;};struct vb2_queue {bool released;};
struct v4l2_subdev {void *owner;struct device *dev;bool unregistered,cleaned;struct {bool cleaned;} entity;};
struct video_device {struct {bool cleaned;} entity;bool unregistered;};
struct media_device {bool unregistered,cleaned;};
struct v4l2_device {unsigned int refs;pthread_mutex_t lock;void (*release)(struct v4l2_device *);bool unregistered;};
static struct completion base_put;
static void module_put(void *owner){(void)owner;}
static void put_device(struct device *dev){(void)dev;}
static void v4l2_async_nf_unregister(struct v4l2_async_notifier *n){(void)n;}
static void v4l2_async_nf_cleanup(struct v4l2_async_notifier *n){(void)n;}
static void media_device_unregister(struct media_device *m){m->unregistered=true;}
static void v4l2_device_unregister_subdev(struct v4l2_subdev *s){s->unregistered=true;}
static void v4l2_device_unregister(struct v4l2_device *v){v->unregistered=true;}
static void v4l2_device_put(struct v4l2_device *v){
 pthread_mutex_lock(&v->lock);assert(v->refs);bool last=!--v->refs;pthread_mutex_unlock(&v->lock);
 complete(&base_put);if(last)v->release(v);
}
static void v4l2_subdev_cleanup(struct v4l2_subdev *s){s->cleaned=true;}
static void media_entity_cleanup(void *e){bool *cleaned=e;*cleaned=true;}
static void media_device_cleanup(struct media_device *m){m->cleaned=true;}
static void fwnode_handle_put(struct fwnode_handle *f){(void)f;}
static void video_unregister_device(struct video_device *v){v->unregistered=true;}
static void vb2_queue_release(struct vb2_queue *q){q->released=true;}
