/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_HW_H
#define MT8183_P1_HW_H
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include "mt8183_p1_adapter.h"
#include "mt8183_p1_stream_codec.h"
#define MT8183_P1_BURST_FRAMES 30U /* legacy metadata only; live limit is zero */
#define MT8183_P1_LOG_FRAMES 12U
#define MT8183_P1_IMAGE_SPANS 6U
#define MT8183_P1_INITIAL_FRAMES 3U
#define MT8183_P1_REFILLS (MT8183_P1_BURST_FRAMES-3U)
#define MT8183_P1_DIAGNOSTIC_COPIES (MT8183_P1_BURST_FRAMES-3U)
#define MT8183_P1_REUSES (MT8183_P1_BURST_FRAMES-6U)
/* Numeric finite journal, NOT a third owner table. Copied before pair reclaim.
 * This first HW profile is bounded diagnostic capture, not yet a VB2 node. */
struct mt8183_p1_frame_log {
    u64 sof_ns, done_ns, send_begin_ns, send_end_ns;
    u64 copy_begin_ns, copy_end_ns, compare_begin_ns, compare_end_ns, published_ns, delivery_ns;
    u32 iova, span, generation, sof_imgo, sof_cq_sequence;
    bool copied, compared, match, archived, reclaimed, delivered;
};
struct mt8183_p1_hw_snapshot {
    u32 cq_start,cq_base,frame_sequence,irq_status,irq_status2,irq_sequence,irq_count;
    u32 ack_count,ack_channel,ack_sequence,submitted_sequence,cq_writes,done_count,sof_count,done_sequence;
    u32 image_bytes,logical_raw_bytes,allocation_bytes,frame_bytes,frame_stride;
    dma_addr_t output_iova;
    u64 epoch;
    int error,send_error;
    bool allocated,published,frame_pending,frame_acked,registers_valid;
    bool done_pending,capture_complete,inputs_started,stopping,refill_pending,copy_pending,publish_pending;
    bool copy_allocated,ring_finalized;
    u32 live_send_attempts,copy_allocation_bytes,copy_allocations,copy_frees;
    u32 ring_pending_sequence,ring_cpu_refs,archived_frames,cq_arms_accepted;
};
struct device;struct mtk_scp;
struct mt8183_p1_hw {
    spinlock_t lock;
    struct completion frame_reply,capture_done,refill_ready;
    struct dps_stream stream;
    struct bq_queue queue;
    struct dpa_adapter adapter;
    struct mt8183_p1_stream_profile profile;
    struct device *cam;
    void __iomem *base;
    void *output_cpu,*copy_cpu,*archive_cpu,*compare_cpu;
    dma_addr_t output_iova;
    size_t allocation_bytes,frame_bytes,frame_stride;
    bool initialized,published,frame_pending,done_pending,refill_pending,copy_pending,publish_pending;
    bool inputs_started,stopping,terminal_seen,finalized;
    int error;
    struct mt8183_p1_hw_snapshot observed;
    struct mt8183_p1_frame_log frames[MT8183_P1_LOG_FRAMES];
    bool stop_requested;
    int (*deliver)(void *,const u8 *,size_t,u64,u64);
    void *deliver_context;
    /* Immutable for the active epoch; called after HW spinlock release. */
    void (*frame_start)(void *,u64);
    void *frame_start_context;
};
int mt8183_p1_hw_prepare_profile(struct mt8183_p1_hw *,struct device *,void __iomem *,u64,u64,u32,u32);
int mt8183_p1_hw_prepare(struct mt8183_p1_hw *,struct device *,void __iomem *,u64,u64);
int mt8183_p1_hw_publish(struct mt8183_p1_hw *);
int mt8183_p1_hw_frame_submit(struct mt8183_p1_hw *,struct mtk_scp *);
int mt8183_p1_hw_inputs_begin(struct mt8183_p1_hw *);
int mt8183_p1_hw_start_commit(struct mt8183_p1_hw *);
int mt8183_p1_hw_inputs_stopped(struct mt8183_p1_hw *,bool);
int mt8183_p1_hw_refill_remaining(struct mt8183_p1_hw *,struct mtk_scp *,
                               int (*publish)(void *,u64),void *);
int mt8183_p1_hw_publish_image(struct mt8183_p1_hw *,u64);
int mt8183_p1_hw_return_cpu(struct mt8183_p1_hw *);
void mt8183_p1_hw_request_stop(struct mt8183_p1_hw *);
int mt8183_p1_hw_wait_done(struct mt8183_p1_hw *);
int mt8183_p1_hw_hold(struct mt8183_p1_hw *,int);
void mt8183_p1_hw_stopping(struct mt8183_p1_hw *);
void mt8183_p1_hw_rx(struct mt8183_p1_hw *,u32,const void *,size_t);
void mt8183_p1_hw_irq(struct mt8183_p1_hw *);
int mt8183_p1_hw_snapshot(struct mt8183_p1_hw *,struct mt8183_p1_hw_snapshot *);
void mt8183_p1_hw_observe(struct mt8183_p1_hw *,struct mt8183_p1_hw_snapshot *);
int mt8183_p1_hw_ring_finalize(struct mt8183_p1_hw *,bool);
int mt8183_p1_hw_release(struct mt8183_p1_hw *,bool);
ssize_t mt8183_p1_hw_read_output(struct mt8183_p1_hw *,char *,loff_t,size_t,bool);
/* Caller owns control mutex; finite diagnostics only, no payload. */
ssize_t mt8183_p1_hw_frame_log(struct mt8183_p1_hw *,char *,loff_t,size_t);
#endif
