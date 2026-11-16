/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_CAMERA_VIDEO_H
#define MT8183_CAMERA_VIDEO_H
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/v4l2-dev.h>
#include "camera_raw.h"
#include <media/videobuf2-v4l2.h>
#define DCV_WIDTH DCV_RAW_WIDTH
#define DCV_HEIGHT DCV_RAW_HEIGHT
#define DCV_BYTES (DCV_WIDTH*DCV_HEIGHT*2U)
struct dcv_buffer {struct vb2_v4l2_buffer vb;struct list_head link;};
/* Session outcomes and lifetime totals; snapshot all fields under buffers_lock. */
struct dcv_video_stats {
 u64 stream_id,delivered,no_buffer,stopping,errors,total_delivered,total_dropped;
 bool running;
};
/* VB2 buffers contain CPU-owned normalized RAW bytes only, never CAM DMA spans. */
struct dcv_video {
 struct video_device vdev;struct vb2_queue queue;struct mutex lock;
 struct media_pad pad;bool queue_initialized,entity_initialized,node_registered;
 spinlock_t buffers_lock;struct list_head buffers;void *context;
 int (*start)(void *);int (*stop)(void *);
 u32 width,height;
 u32 pixel_format;bool registered,running,blocked,pipeline_started;int last_error;
 struct dcv_video_stats stats;
};
int dcv_register(struct dcv_video *,struct device *,struct v4l2_device *,void *,int (*)(void *),int (*)(void *),struct media_entity *,unsigned int);
/* Probe unwind: unregister admission, wait for parent V4L2 refs, then cleanup. */
void dcv_unregister(struct dcv_video *);
void dcv_cleanup(struct dcv_video *);
void dcv_get_stats(struct dcv_video *,struct dcv_video_stats *);
/* Capture caller owns the private immutable shadow through synchronous conversion.
 * Return 1 for a completed VB2 buffer, 0 for a benign discard, or a negative error.
 * A discarded frame completes the handoff, but is not an application delivery. */
int dcv_deliver(struct dcv_video *,const u8 *,size_t,u64,u64);
#endif
