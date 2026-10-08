/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* Bounded-owner P1 HW adapter, pinned Linux 6.18.28-mt81.
 * MMIO/DMA/IPI base: preserved mt8183-p1-ring9; original register/ABI source
 * MediaTek 2019 GPL-2.0, ChromiumOS 527db0b5974bb70364fc692448a55fb37209fe23.
 * Parent retains CAM PM/clocks, SCP, IRQ/RX context and positive OFF gate.
 * Worker profile: thirty frames with six spans; a single capture sender.
 * No module unload/reopen/VB2 promise is made by this diagnostic profile.
 */
#include "mt8183_p1_hw.h"
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iommu-dma.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc/mtk_scp.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#define RAW_STATUS 0x0024U
#define RAW_STATUS2 0x0034U
#define CQ_START 0x0000U
#define CQ_BASE 0x0198U
#define FRAME_SEQUENCE 0x13b8U
#define IMGO_BASE 0x1020U
#define CAM_APERTURE_END 0xff7fffffULL
#define SCP_POOL_START 0x50000000ULL
#define SCP_POOL_END 0x52900000ULL
#define ACK_TIMEOUT_MS 1500U

static int hold_locked(struct mt8183_p1_hw *h,int error)
{
    if(!h->error)WRITE_ONCE(h->error,error<0?error:-EIO);
    if(h->adapter.bound)dpa_hold(&h->adapter,h->error);
    h->observed.error=h->error;h->observed.capture_complete=false;return h->error;
}
static void wake_all(struct mt8183_p1_hw *h)
{ complete(&h->frame_reply);complete(&h->refill_ready);complete(&h->capture_done); }
int mt8183_p1_hw_hold(struct mt8183_p1_hw *h,int error)
{
    unsigned long flags;int ret;if(!h||!h->initialized)return -EINVAL;
    spin_lock_irqsave(&h->lock,flags);ret=hold_locked(h,error);spin_unlock_irqrestore(&h->lock,flags);wake_all(h);return ret;
}
static void sample_locked(struct mt8183_p1_hw *h,struct dpa_sample *p)
{
    p->sequence_before=readl(h->base+FRAME_SEQUENCE);
    p->imgo_iova=readl(h->base+IMGO_BASE);p->cq_iova=readl(h->base+CQ_BASE);
    p->sequence_after=readl(h->base+FRAME_SEQUENCE);p->time_ns=ktime_get_ns();
}
static bool ready_locked(struct mt8183_p1_hw *h)
{
 h->observed.capture_complete=h->finalized&&!h->error;
 return h->observed.capture_complete;
}
static void observe_locked(struct mt8183_p1_hw *h,struct mt8183_p1_hw_snapshot *o)
{
    h->observed.error=h->error;h->observed.allocated=!!h->output_cpu;
    h->observed.copy_allocated=!!h->copy_cpu&&!!h->archive_cpu&&!!h->compare_cpu;
    h->observed.published=h->published;h->observed.frame_pending=h->frame_pending;
    h->observed.done_pending=h->done_pending;h->observed.refill_pending=h->refill_pending;
    h->observed.copy_pending=h->copy_pending||h->publish_pending;h->observed.publish_pending=h->publish_pending;h->observed.inputs_started=h->inputs_started;
    h->observed.stopping=h->stopping;h->observed.ring_finalized=h->finalized;
    h->observed.ring_pending_sequence=h->stream.pending?h->stream.pending_key.fw:0;
    h->observed.ring_cpu_refs=!!h->stream.read.serial+!!h->stream.publisher.serial;
    ready_locked(h);*o=h->observed;
}
int mt8183_p1_hw_prepare_profile(struct mt8183_p1_hw *h,struct device *cam,void __iomem *base,u64 composer,u64 epoch,u32 width,u32 height)
{
    struct iommu_domain *domain;struct dpa_layout layout;struct mt8183_p1_stream_layout raw;
    u64 first,end;size_t off;unsigned int i;int ret;
    if(!h||!cam||!base||!epoch)return -EINVAL;if(h->initialized)return -EALREADY;
    if(!((width==1600&&height==1200)||(width==1632&&height==1224)||(width==3264&&height==2448)))return -EINVAL;
    spin_lock_init(&h->lock);init_completion(&h->frame_reply);init_completion(&h->capture_done);init_completion(&h->refill_ready);
    h->initialized=true;h->cam=cam;h->base=base;
    h->profile=(struct mt8183_p1_stream_profile){.width=width,.height=height,.bayer_id=0,.flags=MT8183_P1_PROFILE_UNVERIFIED_META_ZERO};
    ret=mt8183_p1_stream_raw10_layout(width,height,&raw);if(ret)return ret;
    h->frame_bytes=raw.image_bytes;h->frame_stride=PAGE_ALIGN(h->frame_bytes);
    if(!h->frame_bytes||h->frame_stride<h->frame_bytes)return -EINVAL;
    h->allocation_bytes=h->frame_stride*DPS_SPANS;
    domain=iommu_get_domain_for_dev(cam);
    if(!use_dma_iommu(cam)||get_dma_ops(cam)||dev_is_dma_coherent(cam)||!domain||
       domain->type!=IOMMU_DOMAIN_DMA||domain->geometry.aperture_start||
       domain->geometry.aperture_end!=CAM_APERTURE_END||!domain->geometry.force_aperture||
       cam->coherent_dma_mask!=DMA_BIT_MASK(32)||dma_get_mask(cam)!=DMA_BIT_MASK(32))return -EINVAL;
    h->output_cpu=dma_alloc_coherent(cam,h->allocation_bytes,&h->output_iova,GFP_KERNEL);
    if(!h->output_cpu)return -ENOMEM;
    first=h->output_iova;end=first+h->allocation_bytes;
    if(!first||!IS_ALIGNED(first,PAGE_SIZE)||end<=first||end-1>CAM_APERTURE_END||
       (cam->bus_dma_limit&&end-1>cam->bus_dma_limit)||
       !(end<=composer||first>=composer+BQ_COMPOSER_BYTES)){ret=-ERANGE;goto fail;}
    for(off=0;off<h->allocation_bytes;off+=PAGE_SIZE){
        phys_addr_t phys=iommu_iova_to_phys(domain,first+off);
        if(!phys||!IS_ALIGNED(phys,PAGE_SIZE)||((u64)phys<SCP_POOL_END&&(u64)phys+PAGE_SIZE>SCP_POOL_START)){ret=-EIO;goto fail;}
    }
    h->copy_cpu=vzalloc(DPS_SPANS*h->frame_bytes);if(!h->copy_cpu){ret=-ENOMEM;goto fail;}h->observed.copy_allocations++;
    h->archive_cpu=vzalloc(h->frame_bytes);if(!h->archive_cpu){ret=-ENOMEM;goto fail;}h->observed.copy_allocations++;
    h->compare_cpu=vzalloc(h->frame_bytes);if(!h->compare_cpu){ret=-ENOMEM;goto fail;}h->observed.copy_allocations++;
    ret=dps_init(&h->stream);if(ret)goto fail;h->stream.frame_bytes=h->frame_bytes;
    for(i=0;i<DPS_DESTINATIONS;i++){
        struct dps_ticket t;struct dps_return_token r;
        ret=dps_buffer_ticket(&h->stream,i,&t);if(!ret)ret=dps_buffer_arrive(&h->stream,&t,&r);if(ret)goto fail;
    }
    ret=dps_streamon_guard(&h->stream,epoch,0);if(ret)goto fail;
    ret=bq_init(&h->queue,epoch,0,composer,BQ_COMPOSER_BYTES);if(ret)goto fail;
    layout=(struct dpa_layout){first,h->frame_stride,h->allocation_bytes};
    ret=dpa_bind(&h->adapter,&h->stream,&h->queue,&layout);if(ret)goto fail;
    for(i=0;i<MT8183_P1_LOG_FRAMES;i++){
        struct dps_mapping m;u32 iova;struct mt8183_p1_stream_span spans[MT8183_P1_STREAM_BUFFERS]={0};u8 wire[MT8183_P1_STREAM_FRAME_BYTES];
        ret=dps_mapping(epoch,i,&m);if(!ret)ret=dpa_iova(&h->adapter,&m,&iova);if(ret)goto fail;
        spans[1].iova=iova;spans[1].bytes=h->frame_stride;
        ret=mt8183_p1_stream_encode_frame(wire,sizeof(wire),&h->profile,i+1,spans);if(ret)goto fail;
        h->frames[i].iova=iova;h->frames[i].span=m.span;h->frames[i].generation=m.span_generation;
    }
    memset(h->output_cpu,0,h->allocation_bytes);dma_wmb();
    h->observed.epoch=epoch;h->observed.output_iova=first;h->observed.frame_bytes=h->frame_bytes;
    h->observed.frame_stride=h->frame_stride;h->observed.allocation_bytes=h->allocation_bytes;
    h->observed.image_bytes=DPS_SPANS*h->frame_bytes;h->observed.logical_raw_bytes=0;
    h->observed.copy_allocation_bytes=(DPS_SPANS+2U)*h->frame_bytes;return 0;
fail:
    if(h->compare_cpu){vfree(h->compare_cpu);h->compare_cpu=NULL;h->observed.copy_frees++;}
    if(h->archive_cpu){vfree(h->archive_cpu);h->archive_cpu=NULL;h->observed.copy_frees++;}
    if(h->copy_cpu){vfree(h->copy_cpu);h->copy_cpu=NULL;h->observed.copy_frees++;}
    dma_free_coherent(cam,h->allocation_bytes,h->output_cpu,h->output_iova);h->output_cpu=NULL;h->output_iova=0;return ret;
}
int mt8183_p1_hw_publish(struct mt8183_p1_hw *h)
{
    unsigned long flags;int ret;if(!h||!h->initialized)return -EINVAL;
    spin_lock_irqsave(&h->lock,flags);
    if(!h->output_cpu||h->published)ret=-EPERM;else {ret=dpa_start_begin(&h->adapter);if(!ret)h->published=true;}
    if(ret)hold_locked(h,ret);spin_unlock_irqrestore(&h->lock,flags);return ret;
}
static int submit_frame(struct mt8183_p1_hw *h,struct mtk_scp *scp,bool live)
{
    struct dpa_sample p;struct dps_mapping m;struct bq_send_token t;
    struct mt8183_p1_stream_span spans[MT8183_P1_STREAM_BUFFERS]={0};u8 wire[MT8183_P1_STREAM_FRAME_BYTES];
    unsigned long flags;u32 iova;int ret,finish;bool prepared=false;
    spin_lock_irqsave(&h->lock,flags);
    if(!h->output_cpu||!h->published||h->frame_pending||h->copy_pending||h->publish_pending||h->error||h->stopping){ret=h->error?:-EBUSY;goto out;}
    if(live!=(h->stream.next_logical>=3)||live!=h->inputs_started){ret=-EPROTO;goto hold;}
    ret=dpa_next_map(&h->adapter,&m,&iova);if(ret)goto hold;
    h->frames[m.key.logical%MT8183_P1_LOG_FRAMES]=(struct mt8183_p1_frame_log){.iova=iova,.span=m.span,.generation=m.span_generation};
    spans[1].iova=iova;spans[1].bytes=h->frame_stride;
    ret=mt8183_p1_stream_encode_frame(wire,sizeof(wire),&h->profile,m.key.fw,spans);if(ret)goto hold;
    if(live)sample_locked(h,&p);
    ret=dpa_send_prepare(&h->adapter,live?&p:NULL,&t,&iova);if(ret)goto hold;
    prepared=true;h->frame_pending=true;h->observed.frame_acked=false;
    h->observed.submitted_sequence=m.key.fw;h->frames[m.key.logical%MT8183_P1_LOG_FRAMES].send_begin_ns=ktime_get_ns();
    if(live)h->observed.live_send_attempts++;reinit_completion(&h->frame_reply);
    spin_unlock_irqrestore(&h->lock,flags);
    dma_wmb();ret=scp_ipi_send(scp,MT8183_P1_STREAM_IPI_FRAME,wire,sizeof(wire),0);
    spin_lock_irqsave(&h->lock,flags);h->observed.send_error=ret;
    finish=dpa_send_result(&h->adapter,&t,ret);if(finish)hold_locked(h,finish);
    ret=h->error;spin_unlock_irqrestore(&h->lock,flags);
    if(!ret&&!wait_for_completion_timeout(&h->frame_reply,msecs_to_jiffies(ACK_TIMEOUT_MS)))ret=-ETIMEDOUT;
    spin_lock_irqsave(&h->lock,flags);
    if(live)sample_locked(h,&p);
    finish=dpa_send_complete(&h->adapter,&t,live?&p:NULL,ret?:h->error,true);
    if(finish)hold_locked(h,finish);
    h->frames[m.key.logical%MT8183_P1_LOG_FRAMES].send_end_ns=ktime_get_ns();h->frame_pending=false;
    ret=h->error;goto out;
hold: ret=hold_locked(h,ret);
out: spin_unlock_irqrestore(&h->lock,flags);if(ret&&prepared)wake_all(h);return ret;
}
int mt8183_p1_hw_frame_submit(struct mt8183_p1_hw *h,struct mtk_scp *scp)
{ if(!h||!h->initialized||!scp)return -EINVAL;return submit_frame(h,scp,false); }
int mt8183_p1_hw_inputs_begin(struct mt8183_p1_hw *h)
{
    unsigned long flags;int ret;spin_lock_irqsave(&h->lock,flags);ret=dpa_inputs_begin(&h->adapter);
    if(ret)hold_locked(h,ret);else h->inputs_started=true;spin_unlock_irqrestore(&h->lock,flags);return ret;
}
int mt8183_p1_hw_start_commit(struct mt8183_p1_hw *h)
{
    unsigned long flags;int ret;spin_lock_irqsave(&h->lock,flags);ret=dpa_start_commit(&h->adapter);
    if(ret)hold_locked(h,ret);spin_unlock_irqrestore(&h->lock,flags);return ret;
}
void mt8183_p1_hw_stopping(struct mt8183_p1_hw *h)
{
    unsigned long flags;if(!h||!h->initialized)return;
    spin_lock_irqsave(&h->lock,flags);h->stopping=true;
    if(h->adapter.bound){dpa_stop(&h->adapter);if(h->stream.start_inflight)dpa_start_abort(&h->adapter,h->error?:-ECANCELED);}
    spin_unlock_irqrestore(&h->lock,flags);wake_all(h);
}
int mt8183_p1_hw_inputs_stopped(struct mt8183_p1_hw *h,bool idle)
{
    unsigned long flags;int ret;spin_lock_irqsave(&h->lock,flags);
    ret=dpa_inputs_stopped(&h->adapter,idle);if(ret)hold_locked(h,ret);
    spin_unlock_irqrestore(&h->lock,flags);return ret;
}
static int read_image(struct mt8183_p1_hw *h,u64 logical,bool compare)
{
    struct dps_mapping m;struct dps_read_token t;struct dpa_sample p;
    struct mt8183_p1_frame_log *f;unsigned long flags;void *dma,*shadow;bool match=true;int ret;
    spin_lock_irqsave(&h->lock,flags);
    if(logical>=DPS_MAX_FW_SEQUENCE){ret=hold_locked(h,-ERANGE);goto out;}
    dps_mapping(h->stream.epoch,logical,&m);sample_locked(h,&p);
    ret=dpa_read_begin(&h->adapter,&m.key,compare?DPS_READ_COMPARE:DPS_READ_COPY,&p,&t);
    if(ret){hold_locked(h,ret);goto out;}h->copy_pending=true;f=&h->frames[logical%MT8183_P1_LOG_FRAMES];
    if(compare)f->compare_begin_ns=p.time_ns;else f->copy_begin_ns=p.time_ns;
    dma=(u8*)h->output_cpu+m.span*h->frame_stride;shadow=(u8*)h->copy_cpu+m.span*h->frame_bytes;
    spin_unlock_irqrestore(&h->lock,flags);
    dma_rmb();
    if(compare){
        /* Full DMA read under the existing lease. The single capture caller
         * owns this CPU-only scratch; it is never sent to firmware/publisher.
         * Compare against the earlier shadow, without altering that shadow.
         * Both the copy and equality check remain inside the phase interval. */
        memcpy(h->compare_cpu,dma,h->frame_bytes);
        match=!memcmp(shadow,h->compare_cpu,h->frame_bytes);
    }else memcpy(shadow,dma,h->frame_bytes);
    spin_lock_irqsave(&h->lock,flags);sample_locked(h,&p);
    ret=dpa_read_end(&h->adapter,&t,&p,0,h->frame_bytes,match);
    if(compare){f->compare_end_ns=p.time_ns;f->compared=!ret;f->match=match&&!ret;}
    else {f->copy_end_ns=p.time_ns;f->copied=!ret;}
    /* The guarded COPY owns a private immutable CPU shadow. Transfer bytes
     * synchronously to VB2 now; the capture caller cannot advance/reuse it
     * until this returns. copy_pending prevents release throughout conversion.
     * The later DMA equality check and pure reclaim protocol remain unchanged.
     * They diagnose reuse safety, but must not delay application ownership of
     * an independent completed VB2 buffer. */
    if(!ret&&!compare){
        spin_unlock_irqrestore(&h->lock,flags);
        ret=h->deliver?h->deliver(h->deliver_context,shadow,h->frame_bytes,logical,f->sof_ns):-ENODEV;
        spin_lock_irqsave(&h->lock,flags);
        f->delivered=!ret;f->delivery_ns=ktime_get_ns();
    }
    h->copy_pending=false;
    if(ret)hold_locked(h,ret);
out:spin_unlock_irqrestore(&h->lock,flags);return ret;
}
int mt8183_p1_hw_publish_image(struct mt8183_p1_hw *h,u64 logical)
{
 struct dps_mapping m;struct dps_publish_token t;struct dps_return_token r,arrived;
 struct dps_ticket next;unsigned long flags;int ret,end;
 spin_lock_irqsave(&h->lock,flags);
 if(logical>=DPS_MAX_FW_SEQUENCE||h->publish_pending){ret=-EINVAL;goto hold;}
 dps_mapping(h->stream.epoch,logical,&m);
 ret=dpa_publish_begin(&h->adapter,&m.key,&t);if(ret)goto hold;
 /* Late publication only closes the diagnostic owner/reclaim transaction.
  * Application bytes were transferred once by the guarded COPY caller. */
 h->publish_pending=true;
 ret=h->frames[logical%MT8183_P1_LOG_FRAMES].delivered?0:-EPROTO;
 end=dpa_publish_end(&h->adapter,&t,ret,ret?0:h->frame_bytes,&r);h->publish_pending=false;
 if(!end){end=dpa_return_end(&h->adapter,&r);if(!end&&r.kind!=DPS_RETURN_DONE)end=-ECANCELED;}
 if(!ret)ret=end;if(ret)goto hold;
 h->frames[logical%MT8183_P1_LOG_FRAMES].archived=true;h->frames[logical%MT8183_P1_LOG_FRAMES].published_ns=ktime_get_ns();h->observed.archived_frames++;
 ret=dps_buffer_ticket(&h->stream,r.ticket.index,&next);if(!ret)ret=dps_buffer_arrive(&h->stream,&next,&arrived);if(ret)goto hold;
 ret=dpa_reclaim(&h->adapter,&m.key,true);if(ret)goto hold;h->frames[logical%MT8183_P1_LOG_FRAMES].reclaimed=true;
 spin_unlock_irqrestore(&h->lock,flags);return 0;
hold:ret=hold_locked(h,ret);spin_unlock_irqrestore(&h->lock,flags);return ret;
}
/* STREAMOFF requests a stop at the next capture phase boundary. Closing the
 * pure admission during a live DMA read would deliberately invalidate it.
 * Fault stops still close immediately and retain all allocations. */
void mt8183_p1_hw_request_stop(struct mt8183_p1_hw *h)
{unsigned long flags;spin_lock_irqsave(&h->lock,flags);WRITE_ONCE(h->stop_requested,true);spin_unlock_irqrestore(&h->lock,flags);complete(&h->refill_ready);}
int mt8183_p1_hw_refill_remaining(struct mt8183_p1_hw *h,struct mtk_scp *scp,
                               int (*publish)(void *,u64),void *context)
{
    unsigned long flags;u64 n;int ret=0;struct dpa_sample p;
    spin_lock_irqsave(&h->lock,flags);
    if(!publish||h->error||h->stopping||h->stream.state!=DPS_RUNNING||h->refill_pending||!pm_runtime_active(h->cam)||
       atomic_read(&h->cam->power.usage_count)<=0){ret=h->error?:-EPERM;goto out;}
    h->refill_pending=true;
    for(n=3;n<DPS_MAX_FW_SEQUENCE;n++){
        if(READ_ONCE(h->stop_requested))break;
        while(!h->error&&!h->stopping&&!READ_ONCE(h->stop_requested)&&h->queue.last_sof<n-1){
            reinit_completion(&h->refill_ready);spin_unlock_irqrestore(&h->lock,flags);
            if(!wait_for_completion_timeout(&h->refill_ready,msecs_to_jiffies(ACK_TIMEOUT_MS)))ret=-ETIMEDOUT;
            spin_lock_irqsave(&h->lock,flags);if(ret)hold_locked(h,ret);
        }
        if(READ_ONCE(h->stop_requested))break;
        if(h->error||h->stopping){ret=h->error?:-ECANCELED;break;}
        sample_locked(h,&p);ret=dpa_phase_begin(&h->adapter,&p);if(ret){hold_locked(h,ret);break;}
        spin_unlock_irqrestore(&h->lock,flags);
        if(n>=6)ret=read_image(h,n-6,true);
        if(!ret)ret=submit_frame(h,scp,true);
        /* Publication is CPU-only. First profile serializes it, measures the
         * full cost, and still requires a fresh same-phase COPY end guard. */
        if(!ret&&n>=6)ret=publish(context,n-6);
        if(!ret)ret=read_image(h,n-3,false);
        spin_lock_irqsave(&h->lock,flags);if(ret){hold_locked(h,ret);break;}
    }
    if(!ret&&!READ_ONCE(h->stop_requested)){ret=-EOVERFLOW;hold_locked(h,ret);}
    h->refill_pending=false;
out:spin_unlock_irqrestore(&h->lock,flags);return ret;
}
int mt8183_p1_hw_wait_done(struct mt8183_p1_hw *h)
{
    unsigned long flags;int ret=0;spin_lock_irqsave(&h->lock,flags);
    if(h->error){ret=h->error;goto out;}if(ready_locked(h))goto out;
    h->done_pending=true;spin_unlock_irqrestore(&h->lock,flags);
    if(!wait_for_completion_timeout(&h->capture_done,msecs_to_jiffies(ACK_TIMEOUT_MS)))ret=-ETIMEDOUT;
    spin_lock_irqsave(&h->lock,flags);h->done_pending=false;
    if(ret||!ready_locked(h))hold_locked(h,ret?:-EPROTO);ret=h->error;
out:spin_unlock_irqrestore(&h->lock,flags);return ret;
}
void mt8183_p1_hw_rx(struct mt8183_p1_hw *h,u32 id,const void *data,size_t len)
{
    struct mt8183_p1_stream_rx rx;struct dpa_sample p;unsigned long flags;bool live;int ret;
    if(!h||!h->initialized)return;
    ret=mt8183_p1_stream_observe(id,data,len,&rx);if(ret||rx.kind!=MT8183_P1_STREAM_RX_FRAME_ACK)return;
    spin_lock_irqsave(&h->lock,flags);if(!h->output_cpu){spin_unlock_irqrestore(&h->lock,flags);return;}
    live=h->queue.sender.map.key.logical>=3;if(live)sample_locked(h,&p);
    ret=dpa_ack(&h->adapter,id,data,len,live?&p:NULL);
    h->observed.ack_count++;h->observed.ack_channel=id;h->observed.ack_sequence=rx.sequence;
    if(ret)hold_locked(h,ret);else h->observed.frame_acked=true;
    spin_unlock_irqrestore(&h->lock,flags);complete(&h->frame_reply);if(ret)wake_all(h);
}
void mt8183_p1_hw_irq(struct mt8183_p1_hw *h)
{
    struct dpa_sample p;struct bq_irq_action a={0};unsigned long flags;int ret=0;bool wake=false;
    if(!h||!h->initialized)return;spin_lock_irqsave(&h->lock,flags);
    if(!h->base||!h->output_cpu){ret=hold_locked(h,-ENODEV);goto out;}
    if(h->error){ret=h->error;goto out;}
    h->observed.irq_count++;h->observed.irq_status=readl(h->base+RAW_STATUS);h->observed.irq_status2=readl(h->base+RAW_STATUS2);
    sample_locked(h,&p);h->observed.irq_sequence=p.sequence_before;
    ret=dpa_irq_plan(&h->adapter,h->observed.irq_status,&p,&a);if(ret){hold_locked(h,ret);goto out;}
    if(a.done){struct mt8183_p1_frame_log *f=&h->frames[a.done_key.logical%MT8183_P1_LOG_FRAMES];f->done_ns=p.time_ns;}
    if(a.sof){struct mt8183_p1_frame_log *f=&h->frames[a.sof_key.logical%MT8183_P1_LOG_FRAMES];f->sof_ns=p.time_ns;f->sof_imgo=p.imgo_iova;f->sof_cq_sequence=a.sof_key.fw;}
    if(a.arm){dma_wmb();writel(a.token.iova,h->base+CQ_BASE);h->observed.cq_writes++;sample_locked(h,&p);ret=dpa_irq_finish(&h->adapter,&a,&p,0);if(ret)hold_locked(h,ret);}
    h->observed.sof_count=h->queue.sofs;h->observed.done_count=h->queue.dones;h->observed.done_sequence=h->queue.last_done;
    h->observed.cq_arms_accepted=h->queue.cq_arms_completed;
    if(dpa_terminal(&h->adapter))h->terminal_seen=true;wake=ready_locked(h);
out:spin_unlock_irqrestore(&h->lock,flags);
    if(!ret&&a.sof&&h->frame_start)h->frame_start(h->frame_start_context,a.sof_key.logical);
    if(a.sof||a.done)complete(&h->refill_ready);if(ret)wake_all(h);else if(wake)complete(&h->capture_done);
}
int mt8183_p1_hw_snapshot(struct mt8183_p1_hw *h,struct mt8183_p1_hw_snapshot *out)
{
    unsigned long flags;int ret=0;if(!h||!h->initialized||!out)return -EINVAL;
    spin_lock_irqsave(&h->lock,flags);
    if(!h->base||!pm_runtime_active(h->cam)||atomic_read(&h->cam->power.usage_count)<=0)ret=-EPERM;
    else {h->observed.cq_start=readl(h->base+CQ_START);h->observed.cq_base=readl(h->base+CQ_BASE);
        h->observed.frame_sequence=readl(h->base+FRAME_SEQUENCE);h->observed.registers_valid=true;}
    observe_locked(h,out);spin_unlock_irqrestore(&h->lock,flags);return ret;
}
void mt8183_p1_hw_observe(struct mt8183_p1_hw *h,struct mt8183_p1_hw_snapshot *out)
{
    unsigned long flags;if(!out)return;memset(out,0,sizeof(*out));if(!h||!h->initialized)return;
    spin_lock_irqsave(&h->lock,flags);observe_locked(h,out);spin_unlock_irqrestore(&h->lock,flags);
}
/* Parent proves IRQ/IPI/SCP drain and CAM power OFF before calling this.
 * Snapshot the final six owners before clearing either owner state. */
static int finish_owners(struct mt8183_p1_hw *h,bool gate)
{
    struct dps_return_token r;int ret;
    while((ret=dpa_stop_return_one(&h->adapter,&r))>0){ret=dpa_return_end(&h->adapter,&r);if(ret)return ret;}
    if(ret)return ret;return dpa_finish_stop(&h->adapter,gate,true,true);
}
/* Workers have joined; return CPU destinations without clearing the final
 * six DMA/span owners. Only ring_finalize/release after the positive HW gate
 * may finish those owners. No DMA/MMIO access here. */
int mt8183_p1_hw_return_cpu(struct mt8183_p1_hw *h)
{
    struct dps_return_token r;unsigned long flags;int ret;
    spin_lock_irqsave(&h->lock,flags);
    if(!h->stopping||h->frame_pending||h->done_pending||h->refill_pending||h->copy_pending||h->publish_pending){ret=-EBUSY;goto out;}
    while((ret=dpa_stop_return_one(&h->adapter,&r))>0){ret=dpa_return_end(&h->adapter,&r);if(ret)break;}
out:spin_unlock_irqrestore(&h->lock,flags);return ret;
}
int mt8183_p1_hw_ring_finalize(struct mt8183_p1_hw *h,bool gate)
{
 unsigned long flags;int ret;spin_lock_irqsave(&h->lock,flags);
 if(!gate||!h->stopping||h->adapter.inputs_live||h->frame_pending||h->done_pending||h->refill_pending||h->copy_pending||h->publish_pending||h->finalized){ret=-EBUSY;goto out;}
 /* No finite tail copy. Joined workers + physical OFF allow old DMA owners
  * to finish; CPU outputs already returned by their independent leases. */
 ret=finish_owners(h,true);if(!ret)h->finalized=true;
out:if(ret)hold_locked(h,ret);spin_unlock_irqrestore(&h->lock,flags);return ret;
}
ssize_t mt8183_p1_hw_read_output(struct mt8183_p1_hw *h,char *buf,loff_t off,size_t count,bool gate)
{(void)h;(void)buf;(void)off;(void)count;(void)gate;return -EOPNOTSUPP;}
ssize_t mt8183_p1_hw_frame_log(struct mt8183_p1_hw *h,char *buf,loff_t off,size_t count)
{
    unsigned long flags;size_t bytes=sizeof(h->frames);
    if(off<0||(!buf&&count))return -EINVAL;if((u64)off>=bytes)return 0;
    if(count>bytes-(size_t)off)count=bytes-(size_t)off;
    spin_lock_irqsave(&h->lock,flags);memcpy(buf,(u8*)h->frames+(size_t)off,count);spin_unlock_irqrestore(&h->lock,flags);return count;
}
int mt8183_p1_hw_release(struct mt8183_p1_hw *h,bool gate)
{
    unsigned long flags;void *dma,*shadow,*archive,*compare;int ret=0;
    if(!h||!h->initialized)return 0;spin_lock_irqsave(&h->lock,flags);
    if((h->published&&!gate)||h->frame_pending||h->done_pending||h->refill_pending||h->copy_pending||h->publish_pending){ret=-EBUSY;goto out;}
    if(h->published&&!h->queue.stopped){ret=finish_owners(h,gate);if(ret)goto out;}
    dma=h->output_cpu;shadow=h->copy_cpu;archive=h->archive_cpu;compare=h->compare_cpu;h->output_cpu=NULL;h->copy_cpu=NULL;h->archive_cpu=NULL;h->compare_cpu=NULL;h->base=NULL;
    h->observed.registers_valid=false;spin_unlock_irqrestore(&h->lock,flags);
    if(dma)dma_free_coherent(h->cam,h->allocation_bytes,dma,h->output_iova);
    if(shadow){vfree(shadow);h->observed.copy_frees++;}if(archive){vfree(archive);h->observed.copy_frees++;}if(compare){vfree(compare);h->observed.copy_frees++;}return 0;
out:spin_unlock_irqrestore(&h->lock,flags);return ret;
}

int mt8183_p1_hw_prepare(struct mt8183_p1_hw *h,struct device *cam,void __iomem *base,u64 composer,u64 epoch)
{ return mt8183_p1_hw_prepare_profile(h,cam,base,composer,epoch,1632,1224); }
