/* SPDX-License-Identifier: GPL-2.0-only */
/* Host wrappers: actual frontend and pad/control functions are extracted. */
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
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,y) __atomic_store_n(&(x),(y),__ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define smp_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define dev_info(...) ((void)0)
struct mutex {pthread_mutex_t lock;};
static void mutex_init(struct mutex *m){assert(!pthread_mutex_init(&m->lock,NULL));}
static void mutex_lock(struct mutex *m){assert(!pthread_mutex_lock(&m->lock));}
static void mutex_unlock(struct mutex *m){assert(!pthread_mutex_unlock(&m->lock));}
typedef struct mutex spinlock_t;
#define spin_lock_irqsave(m,f) do{(f)=0;mutex_lock(m);}while(0)
#define spin_unlock_irqrestore(m,f) do{(void)(f);mutex_unlock(m);}while(0)
struct list_head {struct list_head *next,*prev;};
static void INIT_LIST_HEAD(struct list_head *l){l->next=l->prev=l;}
static bool list_empty(struct list_head *l){return l->next==l;}
static void list_add_tail(struct list_head *x,struct list_head *l){x->prev=l->prev;x->next=l;l->prev->next=x;l->prev=x;}
static void list_del_init(struct list_head *x){x->prev->next=x->next;x->next->prev=x->prev;INIT_LIST_HEAD(x);}
#define list_first_entry(l,t,m) container_of((l)->next,t,m)
struct v4l2_subdev;struct media_entity {bool subdev;struct v4l2_subdev *sd;};
struct media_pad {struct media_entity *entity;unsigned int flags,index;};
struct video_device {struct media_entity entity;void *priv;};struct v4l2_device;struct device;struct media_entity;
enum vb2_buffer_state {VB2_BUF_STATE_QUEUED,VB2_BUF_STATE_ERROR,VB2_BUF_STATE_DONE};
struct vb2_queue {void *priv;bool busy;};
struct dma_buf {unsigned int begins,ends;int begin_error,end_error;};
#define DMA_BIDIRECTIONAL 0
static int dma_buf_begin_cpu_access(struct dma_buf *b,int dir){assert(dir==DMA_BIDIRECTIONAL);b->begins++;return b->begin_error;}
static int dma_buf_end_cpu_access(struct dma_buf *b,int dir){assert(dir==DMA_BIDIRECTIONAL);b->ends++;return b->end_error;}
struct vb2_buffer {struct vb2_queue *vb2_queue;struct {struct dma_buf *dbuf;} planes[1];u8 *out;unsigned int payload;size_t capacity;u64 timestamp;unsigned int done;enum vb2_buffer_state state;};
struct vb2_v4l2_buffer {struct vb2_buffer vb2_buf;u32 sequence,field;};
static struct vb2_v4l2_buffer *to_vb2_v4l2_buffer(struct vb2_buffer *b){return container_of(b,struct vb2_v4l2_buffer,vb2_buf);}
static void *vb2_get_drv_priv(struct vb2_queue *q){return q->priv;}
static size_t vb2_plane_size(struct vb2_buffer *b,unsigned int p){assert(!p);return b->capacity;}
static pthread_mutex_t lease_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lease_cond=PTHREAD_COND_INITIALIZER;
static bool lease_hold,lease_entered,lease_release;
static void *vb2_plane_vaddr(struct vb2_buffer *b,unsigned int p){
 assert(!p);pthread_mutex_lock(&lease_lock);
 if(lease_hold){lease_entered=true;pthread_cond_broadcast(&lease_cond);while(!lease_release)pthread_cond_wait(&lease_cond,&lease_lock);}
 pthread_mutex_unlock(&lease_lock);return b->out;
}
static void vb2_set_plane_payload(struct vb2_buffer *b,unsigned int p,unsigned int n){assert(!p);b->payload=n;}
static void vb2_buffer_done(struct vb2_buffer *b,enum vb2_buffer_state state){b->done++;b->state=state;}
static bool vb2_is_busy(struct vb2_queue *q){return q->busy;}
struct file {void *priv;};
static void *video_drvdata(struct file *file){return file->priv;}
static unsigned int open_calls,pipeline_starts,pipeline_stops;
static int pipeline_error;
static int v4l2_fh_open(struct file *file){(void)file;open_calls++;return 0;}
static int video_device_pipeline_alloc_start(struct video_device *dev){(void)dev;pipeline_starts++;return pipeline_error;}
static void video_device_pipeline_stop(struct video_device *dev){(void)dev;pipeline_stops++;}
#define V4L2_FIELD_NONE 0
#define V4L2_PIX_FMT_SRGGB10 0x30314752U
#define V4L2_COLORSPACE_RAW 11
#define V4L2_XFER_FUNC_NONE 5
#define V4L2_QUANTIZATION_FULL_RANGE 1
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_SUBDEV_FORMAT_TRY 0
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
#define MEDIA_DEV_NOTIFY_PRE_LINK_CH 0
#define MEDIA_LNK_FL_ENABLED 1
#define P1_NUM_PADS 2
struct v4l2_pix_format {u32 width,height,pixelformat,field,bytesperline,sizeimage,colorspace,priv,flags,ycbcr_enc,quantization,xfer_func;};
struct v4l2_format {u32 type;union {struct v4l2_pix_format pix;} fmt;};
struct v4l2_mbus_framefmt {u32 width,height,code,field,colorspace,xfer_func,quantization;};
struct v4l2_subdev_state {struct v4l2_mbus_framefmt fmt[2];};
static struct v4l2_mbus_framefmt *v4l2_subdev_state_get_format(struct v4l2_subdev_state *s,unsigned int p){assert(p<2);return &s->fmt[p];}
struct v4l2_subdev_format {u32 pad,stream,which;struct v4l2_mbus_framefmt format;};
struct v4l2_subdev_mbus_code_enum {u32 pad,index,stream,code;};
struct v4l2_subdev_frame_size_enum {u32 pad,index,stream,code,min_width,max_width,min_height,max_height;};
struct v4l2_ctrl {u32 id;int value;long long minimum,maximum;};
struct v4l2_ctrl_handler {struct v4l2_ctrl ctrl[4];};
struct v4l2_subdev {struct v4l2_ctrl_handler *ctrl_handler;struct v4l2_mbus_framefmt format;int get_error;};

static bool is_media_entity_v4l2_subdev(struct media_entity *e){return e->subdev;}
static struct v4l2_subdev *media_entity_to_v4l2_subdev(struct media_entity *e){return e->sd;}
static int mock_active_get(struct v4l2_subdev *sd,struct v4l2_subdev_format *f){if(sd->get_error)return sd->get_error;f->format=sd->format;return 0;}
#define v4l2_subdev_call_state_active(sd,o,f,arg) mock_active_get(sd,arg)
#define V4L2_CID_VBLANK 1
#define V4L2_CID_EXPOSURE 2
#define V4L2_CID_ANALOGUE_GAIN 3
#define V4L2_CID_TEST_PATTERN 4
static struct v4l2_ctrl *v4l2_ctrl_find(struct v4l2_ctrl_handler *h,u32 id){for(unsigned int i=0;i<4;i++)if(h->ctrl[i].id==id)return &h->ctrl[i];return NULL;}
static int v4l2_ctrl_g_ctrl(struct v4l2_ctrl *c){return c->value;}
struct media_device {int unused;};
struct media_link {struct {struct media_device *mdev;} graph_obj;struct media_pad *source,*sink;};

#define MEDIA_BUS_FMT_SBGGR10_1X10 0x3007
#define MEDIA_BUS_FMT_SGBRG10_1X10 0x300e
#define MEDIA_BUS_FMT_SGRBG10_1X10 0x300a
#define V4L2_PIX_FMT_SBGGR10 1001
#define V4L2_PIX_FMT_SGBRG10 1002
#define V4L2_PIX_FMT_SGRBG10 1003

struct v4l2_fmtdesc {unsigned mbus_code,index,pixelformat;};
