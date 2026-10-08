/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* RAM-only conventional V4L2 capture frontend, standard CPU RAW10 output.
 * Linux 6.18.28 VB2 core owns MMAP/read buffers; no user DMA addresses. */
#include "camera_video.h"
#include <linux/errno.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-vmalloc.h>
#include "camera_raw.h"
/* The capture frontend preserves CFA order; no colour conversion here. */
static const u32 dcv_bayer_codes[] = { MEDIA_BUS_FMT_SBGGR10_1X10,
 MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10 };
static const u32 dcv_bayer_pixels[] = { V4L2_PIX_FMT_SBGGR10,
 V4L2_PIX_FMT_SGBRG10, V4L2_PIX_FMT_SGRBG10, V4L2_PIX_FMT_SRGGB10 };
static int dcv_bayer_index(u32 code)
{ unsigned int i; for(i=0;i<4;i++)if(dcv_bayer_codes[i]==code)return i;return -EINVAL; }
static int dcv_pixel_index(u32 pixel)
{ unsigned int i;for(i=0;i<4;i++)if(dcv_bayer_pixels[i]==pixel)return i;return -EINVAL; }
static int setup(struct vb2_queue *q,unsigned int *n,unsigned int *planes,unsigned int sizes[],struct device *alloc_devs[])
{
 (void)q;(void)alloc_devs;
 struct dcv_video *v=vb2_get_drv_priv(q);u32 bytes=v->width*v->height*2U;
 if(*planes)return *planes==1&&sizes[0]>=bytes?0:-EINVAL;
 *planes=1;sizes[0]=bytes;if(*n<4)*n=4;return 0;
}
static int prepare(struct vb2_buffer *b)
{struct dcv_video *v=vb2_get_drv_priv(b->vb2_queue);u32 bytes=v->width*v->height*2U;if(vb2_plane_size(b,0)<bytes)return -EINVAL;vb2_set_plane_payload(b,0,bytes);return 0;}
static void queue_buffer(struct vb2_buffer *b)
{
 struct dcv_video *v=vb2_get_drv_priv(b->vb2_queue);
 struct dcv_buffer *x=container_of(to_vb2_v4l2_buffer(b),struct dcv_buffer,vb);unsigned long f;
 spin_lock_irqsave(&v->buffers_lock,f);list_add_tail(&x->link,&v->buffers);spin_unlock_irqrestore(&v->buffers_lock,f);
}
static void return_buffers(struct dcv_video *v,enum vb2_buffer_state state)
{
 unsigned long f;
 for(;;){
  struct dcv_buffer *x;
  spin_lock_irqsave(&v->buffers_lock,f);
  if(list_empty(&v->buffers)){spin_unlock_irqrestore(&v->buffers_lock,f);break;}
  x=list_first_entry(&v->buffers,struct dcv_buffer,link);list_del_init(&x->link);
  spin_unlock_irqrestore(&v->buffers_lock,f);vb2_buffer_done(&x->vb.vb2_buf,state);
 }
}
static int start(struct vb2_queue *q,unsigned int count)
{
 struct dcv_video *v=vb2_get_drv_priv(q);int ret;(void)count;
 if(v->blocked){return_buffers(v,VB2_BUF_STATE_QUEUED);return v->last_error?:-EIO;}
 /* start may publish synchronously before return; admission opens first.
  * A failed start must join every producer before QUEUED rollback. */
 ret=video_device_pipeline_alloc_start(&v->vdev);
 if(ret){return_buffers(v,VB2_BUF_STATE_QUEUED);return ret;}
 v->pipeline_started=true;
 WRITE_ONCE(v->running,true);
 ret=v->start(v->context);
 if(ret){video_device_pipeline_stop(&v->vdev);v->pipeline_started=false;}
 if(ret){WRITE_ONCE(v->running,false);v->last_error=ret;v->blocked=true;return_buffers(v,VB2_BUF_STATE_QUEUED);}
 return ret;
}
static void stop(struct vb2_queue *q)
{
 struct dcv_video *v=vb2_get_drv_priv(q);int ret;
 WRITE_ONCE(v->running,false);ret=v->stop(v->context);
 /* Hardware callbacks must return only after capture/publisher real joins.
  * Failure holds the CAM resources and blocks reopen, but VB2 CPU buffers
  * are safe to return after those joins. Never return an in-flight buffer. */
 if(ret){v->blocked=true;v->last_error=ret;}
 if(v->pipeline_started){video_device_pipeline_stop(&v->vdev);v->pipeline_started=false;}
 return_buffers(v,VB2_BUF_STATE_ERROR);
}
static const struct vb2_ops buffer_ops={.queue_setup=setup,.buf_prepare=prepare,.buf_queue=queue_buffer,.start_streaming=start,.stop_streaming=stop,.wait_prepare=vb2_ops_wait_prepare,.wait_finish=vb2_ops_wait_finish};
static int querycap(struct file *f,void *fh,struct v4l2_capability *c)
{
 (void)f;(void)fh;strscpy(c->driver,"mtk-cam-p1-raw",sizeof(c->driver));strscpy(c->card,"MT8183 P1 RAW",sizeof(c->card));strscpy(c->bus_info,"platform:mtk-cam-p1-raw",sizeof(c->bus_info));return 0;
}
static int enumfmt(struct file *f,void *fh,struct v4l2_fmtdesc *d)
{
 (void)f;(void)fh;
 /* IO_MC enumeration must filter the caller's media bus code. Returning
  * unrelated CFA layouts makes Simple construct an invalid RAW pipeline. */
 if(d->mbus_code){int index=dcv_bayer_index(d->mbus_code);if(index<0||d->index)return -EINVAL;d->pixelformat=dcv_bayer_pixels[index];return 0;}
 if(d->index>=4)return -EINVAL;d->pixelformat=dcv_bayer_pixels[d->index];return 0;
}
static int format(struct file *f,void *fh,struct v4l2_format *fmt)
{
 struct v4l2_pix_format *p=&fmt->fmt.pix;int index=dcv_pixel_index(p->pixelformat);(void)f;(void)fh;
 if(index<0)index=3;
 if(fmt->type!=V4L2_BUF_TYPE_VIDEO_CAPTURE)return -EINVAL;
 u32 w=p->width,h=p->height;
 if(w<=1600&&h<=1200){w=1600;h=1200;}else if(w<=1632&&h<=1224){w=1632;h=1224;}else{w=3264;h=2448;}
 memset(p,0,sizeof(*p));p->width=w;p->height=h;p->pixelformat=dcv_bayer_pixels[index];
 p->field=V4L2_FIELD_NONE;p->bytesperline=p->width*2;p->sizeimage=p->width*p->height*2U;
 p->colorspace=V4L2_COLORSPACE_RAW;p->xfer_func=V4L2_XFER_FUNC_NONE;p->quantization=V4L2_QUANTIZATION_FULL_RANGE;return 0;
}
static int setfmt(struct file *f,void *fh,struct v4l2_format *fmt)
{struct dcv_video *v=video_drvdata(f);if(vb2_is_busy(&v->queue))return -EBUSY;int ret=format(f,fh,fmt);if(!ret){v->pixel_format=fmt->fmt.pix.pixelformat;v->width=fmt->fmt.pix.width;v->height=fmt->fmt.pix.height;}return ret;}
static int getfmt(struct file *f,void *fh,struct v4l2_format *fmt)
{struct dcv_video *v=video_drvdata(f);fmt->fmt.pix.pixelformat=v->pixel_format;fmt->fmt.pix.width=v->width;fmt->fmt.pix.height=v->height;return format(f,fh,fmt);}
static int framesizes(struct file *f,void *fh,struct v4l2_frmsizeenum *e)
{(void)f;(void)fh;if(e->index>2||dcv_pixel_index(e->pixel_format)<0)return -EINVAL;e->type=V4L2_FRMSIZE_TYPE_DISCRETE;e->discrete.width=e->index==2?3264:e->index?1632:1600;e->discrete.height=e->index==2?2448:e->index?1224:1200;return 0;}
static int enuminput(struct file *f,void *fh,struct v4l2_input *i)
{(void)f;(void)fh;if(i->index)return -EINVAL;strscpy(i->name,"Camera input",sizeof(i->name));i->type=V4L2_INPUT_TYPE_CAMERA;return 0;}
static int getinput(struct file *f,void *fh,unsigned int *i){(void)f;(void)fh;*i=0;return 0;}
static int setinput(struct file *f,void *fh,unsigned int i){(void)f;(void)fh;return i?-EINVAL:0;}
static int validate_link(struct media_link *link)
{
 struct v4l2_subdev_format fmt={.which=V4L2_SUBDEV_FORMAT_ACTIVE,.pad=link->source->index};
 struct v4l2_subdev *source;struct dcv_video *v=container_of(link->sink->entity,struct dcv_video,vdev.entity);int ret,index;
 if(!is_media_entity_v4l2_subdev(link->source->entity))return -EINVAL;
 source=media_entity_to_v4l2_subdev(link->source->entity);
 ret=v4l2_subdev_call_state_active(source,pad,get_fmt,&fmt);
 if(ret)return ret;
 index=dcv_bayer_index(fmt.format.code);
 return index>=0&&dcv_bayer_pixels[index]==v->pixel_format&&fmt.format.width==v->width&&fmt.format.height==v->height&&fmt.format.field==V4L2_FIELD_NONE?0:-EPIPE;
}
static const struct media_entity_operations entity_ops={.link_validate=validate_link};
static const struct v4l2_ioctl_ops ioctls={.vidioc_querycap=querycap,.vidioc_enum_input=enuminput,.vidioc_g_input=getinput,.vidioc_s_input=setinput,.vidioc_enum_fmt_vid_cap=enumfmt,.vidioc_g_fmt_vid_cap=getfmt,.vidioc_try_fmt_vid_cap=format,.vidioc_s_fmt_vid_cap=setfmt,.vidioc_enum_framesizes=framesizes,.vidioc_reqbufs=vb2_ioctl_reqbufs,.vidioc_create_bufs=vb2_ioctl_create_bufs,.vidioc_querybuf=vb2_ioctl_querybuf,.vidioc_qbuf=vb2_ioctl_qbuf,.vidioc_dqbuf=vb2_ioctl_dqbuf,.vidioc_expbuf=vb2_ioctl_expbuf,.vidioc_streamon=vb2_ioctl_streamon,.vidioc_streamoff=vb2_ioctl_streamoff};
/* The device core can expose the minor before all media links are ready.
 * Refuse opens until the complete graph is published. Parent V4L2 refcount
 * keeps this embedded state alive through racing failed opens on unwind. */
static int open_video(struct file *file)
{
 struct dcv_video *v=video_drvdata(file);
 if(!smp_load_acquire(&v->registered))return -ENODEV;
 return v4l2_fh_open(file);
}
static const struct v4l2_file_operations fops={.owner=THIS_MODULE,.open=open_video,.release=vb2_fop_release,.read=vb2_fop_read,.poll=vb2_fop_poll,.mmap=vb2_fop_mmap,.unlocked_ioctl=video_ioctl2};
void dcv_unregister(struct dcv_video *v)
{
 smp_store_release(&v->registered,false);
 if(v->node_registered){video_unregister_device(&v->vdev);v->node_registered=false;}
}
void dcv_cleanup(struct dcv_video *v)
{
 /* Only after the parent V4L2 release completion, including failed opens. */
 if(v->queue_initialized){vb2_queue_release(&v->queue);v->queue_initialized=false;}
 if(v->entity_initialized){media_entity_cleanup(&v->vdev.entity);v->entity_initialized=false;}
}
int dcv_register(struct dcv_video *v,struct device *dev,struct v4l2_device *v4l2,void *context,int (*on)(void *),int (*off)(void *),struct media_entity *source,unsigned int source_pad)
{
 int ret;if(!v||!dev||!v4l2||!context||!on||!off||!source)return -EINVAL;
 v->width=1632;v->height=1224;v->pixel_format=V4L2_PIX_FMT_SRGGB10;mutex_init(&v->lock);spin_lock_init(&v->buffers_lock);INIT_LIST_HEAD(&v->buffers);v->context=context;v->start=on;v->stop=off;
 v->queue=(struct vb2_queue){.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,.io_modes=VB2_MMAP|VB2_DMABUF|VB2_READ,.drv_priv=v,.buf_struct_size=sizeof(struct dcv_buffer),.ops=&buffer_ops,.mem_ops=&vb2_vmalloc_memops,.timestamp_flags=V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,.lock=&v->lock,.dev=dev,.min_queued_buffers=2};
 ret=vb2_queue_init(&v->queue);if(ret)return ret;v->queue_initialized=true;
 v->pad.flags=MEDIA_PAD_FL_SINK;
 v->vdev.entity.ops=&entity_ops;
 ret=media_entity_pads_init(&v->vdev.entity,1,&v->pad);if(ret)return ret;v->entity_initialized=true;
 strscpy(v->vdev.name,"MT8183 P1 RAW",sizeof(v->vdev.name));v->vdev.v4l2_dev=v4l2;v->vdev.fops=&fops;v->vdev.ioctl_ops=&ioctls;v->vdev.release=video_device_release_empty;v->vdev.lock=&v->lock;v->vdev.queue=&v->queue;v->vdev.device_caps=V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_STREAMING|V4L2_CAP_READWRITE|V4L2_CAP_IO_MC;video_set_drvdata(&v->vdev,v);
 /* Sensor controls stay on the sensor subdevice; no duplicate video cache. */
 ret=video_register_device(&v->vdev,VFL_TYPE_VIDEO,-1);if(ret)return ret;v->node_registered=true;
 /* The target core can return success after an MC registration failure.
  * Refuse an incomplete entity/interface before creating a data link. */
 if(v->vdev.entity.graph_obj.mdev!=v4l2->mdev||!v->vdev.intf_devnode)return -ENODEV;
 ret=media_create_pad_link(source,source_pad,&v->vdev.entity,0,MEDIA_LNK_FL_ENABLED|MEDIA_LNK_FL_IMMUTABLE);
 if(ret)return ret;
 /* Caller publishes subdev nodes and only then sets registered with release. */
 return 0;
}
int dcv_deliver(struct dcv_video *v,const u8 *raw,size_t bytes,u64 logical,u64 timestamp)
{
 struct dcv_buffer *b;unsigned long f;int ret,end_ret;void *out;struct dma_buf *dbuf;
 if(!v||!raw||bytes!=v->width*v->height*5U/4U)return -EINVAL;
 spin_lock_irqsave(&v->buffers_lock,f);
 if(!READ_ONCE(v->running)||list_empty(&v->buffers)){v->dropped++;spin_unlock_irqrestore(&v->buffers_lock,f);return 0;}
 b=list_first_entry(&v->buffers,struct dcv_buffer,link);list_del_init(&b->link);spin_unlock_irqrestore(&v->buffers_lock,f);
 /* vmalloc imported mappings do not synchronize an external exporter for
  * CPU writes. Balance access before publishing a DONE buffer to libcamera. */
 dbuf=b->vb.vb2_buf.planes[0].dbuf;
 ret=dbuf?dma_buf_begin_cpu_access(dbuf,DMA_BIDIRECTIONAL):0;
 if(!ret){
  out=vb2_plane_vaddr(&b->vb.vb2_buf,0);ret=out?dcv_raw10_unpack(raw,bytes,out,v->width*v->height*2U):-EFAULT;
  if(dbuf){end_ret=dma_buf_end_cpu_access(dbuf,DMA_BIDIRECTIONAL);if(!ret)ret=end_ret;}
 }
 b->vb.sequence=(u32)logical;b->vb.field=V4L2_FIELD_NONE;b->vb.vb2_buf.timestamp=timestamp;vb2_set_plane_payload(&b->vb.vb2_buf,0,ret?0:v->width*v->height*2U);
 vb2_buffer_done(&b->vb.vb2_buf,ret?VB2_BUF_STATE_ERROR:VB2_BUF_STATE_DONE);if(!ret)v->delivered++;return ret;
}
MODULE_IMPORT_NS("DMA_BUF");
