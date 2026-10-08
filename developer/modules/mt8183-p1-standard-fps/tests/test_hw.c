/* SPDX-License-Identifier: GPL-2.0-only */
#include "hw-shim.h"
#include "duet_p1_hw.h"
static unsigned checks,scenarios,allocations,frees,allocation_calls,fail_allocation,send_calls,cancel_send,write_calls,fail_write,corrupt_shadow,corrupt_offset;
static u32 regs[0x2000/4];static u64 clock_ns;static struct duet_p1_hw *active;
#define C(x) do{checks++;if(!(x)){fprintf(stderr,"%s:%d %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
void *dma_alloc_coherent(struct device *d,size_t n,dma_addr_t *a,int flags)
{(void)d;(void)flags;allocation_calls++;if(allocation_calls==fail_allocation)return NULL;*a=0xf0000000;allocations++;return calloc(1,n);}
void dma_free_coherent(struct device *d,size_t n,void *p,dma_addr_t a){(void)d;(void)n;(void)a;C(p!=NULL);frees++;free(p);}
void *vzalloc(size_t n){allocation_calls++;if(allocation_calls==fail_allocation)return NULL;allocations++;return calloc(1,n);}
void vfree(void *p){C(p!=NULL);frees++;free(p);}
u32 readl(const void *p){return *(const u32*)p;}
void writel(u32 x,void *p){*(u32*)p=x;write_calls++;if(write_calls==fail_write)regs[0x13b8/4]++;}
u64 ktime_get_ns(void){clock_ns+=1000;return clock_ns;}
static void event(u32 status,u32 seq)
{
    C(!active->lock);regs[0x24/4]=status;regs[0x13b8/4]=seq;
    if(status&BQ_IRQ_SOF){u32 span=(seq-1)%6;regs[0x1020/4]=(u32)active->output_iova+span*(u32)active->frame_stride;
        /* Deterministic synthetic bytes, never a camera image. */
        memset((u8*)active->output_cpu+span*active->frame_stride,(int)seq,active->frame_bytes);}
    if(corrupt_shadow && seq==5)((u8*)active->copy_cpu)[corrupt_offset]^=1;
    duet_p1_hw_irq(active);regs[0x24/4]=0;
}
unsigned long wait_for_completion_timeout(struct completion *c,unsigned long timeout)
{
    (void)timeout;C(!active->lock);
    if(!c->done&&c==&active->refill_ready){
        if(!active->queue.last_sof)event(BQ_IRQ_SOF,1);
        if(!c->done)event(BQ_IRQ_DONE|BQ_IRQ_SOF,active->queue.last_sof+1);
    }
    if(!c->done&&c==&active->capture_done){
        while(!active->error&&active->queue.last_sof<DUET_P1_BURST_FRAMES)
            event(BQ_IRQ_DONE|BQ_IRQ_SOF,active->queue.last_sof+1);
        if(!active->error)event(BQ_IRQ_DONE,DUET_P1_BURST_FRAMES);
    }
    if(c->done){c->done=false;return 1;}return 0;
}
int scp_ipi_send(struct mtk_scp *scp,u32 channel,void *packet,unsigned int length,unsigned int wait)
{
    const u8 *p=packet;u32 seq=(u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
    unsigned char ack[6]={4,5,p[0],p[1],p[2],p[3]};(void)scp;
    C(!active->lock && channel==11 && length==60 && !wait);send_calls++;
    if(seq==1){regs[0x13b8/4]=1;regs[0x198/4]=0xfd0012c0;}
    if(cancel_send==send_calls)duet_p1_hw_stopping(active);
    duet_p1_hw_rx(active,channel,ack,sizeof(ack));return 0;
}
static struct device cam={.coherent_dma_mask=0xffffffff,.power={1}};static struct mtk_scp scp;
static void reset(struct duet_p1_hw *h)
{memset(h,0,sizeof(*h));memset(regs,0,sizeof(regs));active=h;allocation_calls=fail_allocation=send_calls=cancel_send=write_calls=fail_write=corrupt_shadow=corrupt_offset=0;}
static void prepare(struct duet_p1_hw *h)
{reset(h);C(duet_p1_hw_prepare(h,&cam,regs,0xfd000000,1)==0);h->profile.bayer_id=3;}
static void initial(struct duet_p1_hw *h)
{C(duet_p1_hw_publish(h)==0);for(unsigned i=0;i<3;i++)C(duet_p1_hw_frame_submit(h,&scp)==0);C(duet_p1_hw_inputs_begin(h)==0);C(duet_p1_hw_start_commit(h)==0);}
static int sync_publish(void *context,u64 logical)
{return duet_p1_hw_publish_image(context,logical);}
static void normal(void)
{
    struct duet_p1_hw h;struct duet_p1_hw_snapshot snap;unsigned char bytes[32];scenarios++;prepare(&h);
    C(h.compare_cpu&&h.compare_cpu!=h.copy_cpu&&h.compare_cpu!=h.archive_cpu&&h.compare_cpu!=h.output_cpu);
    C(h.observed.copy_allocations==3&&h.observed.copy_allocation_bytes==88800000);
    initial(&h);
    C(duet_p1_hw_refill_remaining(&h,&scp,sync_publish,&h)==0);C(duet_p1_hw_wait_done(&h)==0);
    C(h.terminal_seen && send_calls==30 && h.observed.archived_frames==24);
    C(duet_p1_hw_release(&h,false)==-EBUSY);
    duet_p1_hw_stopping(&h);C(duet_p1_hw_inputs_stopped(&h,true)==0);
    C(duet_p1_hw_return_cpu(&h)==0);
    C(h.stream.spans[0].owned && !h.queue.stopped);
    C(duet_p1_hw_ring_finalize(&h,true)==0);C(h.observed.archived_frames==30);
    for(unsigned i=0;i<30;i++){
        C(duet_p1_hw_read_output(&h,(char*)bytes,(loff_t)i*2496960,32,true)==32);
        for(unsigned j=0;j<32;j++)C(bytes[j]==i+1);
        C(duet_p1_hw_read_output(&h,(char*)bytes,((loff_t)i+1)*2496960-16,16,true)==16);
        for(unsigned j=0;j<16;j++)C(bytes[j]==i+1);
    }
    C(duet_p1_hw_read_output(&h,(char*)bytes,72000000,32,true)==0);
    C(duet_p1_hw_read_output(&h,(char*)bytes,-1,32,true)==-EINVAL);
    C(duet_p1_hw_read_output(&h,(char*)bytes,0,32,false)==-EPERM);
    duet_p1_hw_observe(&h,&snap);C(snap.ring_finalized&&!snap.ring_pending_sequence&&!snap.ring_cpu_refs);
    C(duet_p1_hw_release(&h,true)==0);C(allocations==frees);
    duet_p1_hw_observe(&h,&snap);C(!snap.capture_complete&&!snap.allocated&&!snap.copy_allocated&&snap.copy_frees==3&&!h.compare_cpu);
}
static void failures(void)
{
    for(unsigned stage=1;stage<=4;stage++){
        struct duet_p1_hw h;scenarios++;reset(&h);fail_allocation=stage;
        C(duet_p1_hw_prepare(&h,&cam,regs,0xfd000000,1)==-ENOMEM);C(allocations==frees);
        C(duet_p1_hw_release(&h,false)==0);
    }
    for(unsigned send=1;send<=3;send++){
        struct duet_p1_hw h;scenarios++;prepare(&h);C(duet_p1_hw_publish(&h)==0);cancel_send=send;
        for(unsigned n=1;n<send;n++)C(duet_p1_hw_frame_submit(&h,&scp)==0);
        C(duet_p1_hw_frame_submit(&h,&scp)<0);C(!h.frame_pending&&!h.stream.pending&&!h.queue.sender.serial);
        C(duet_p1_hw_inputs_stopped(&h,true)==0);C(duet_p1_hw_release(&h,false)==-EBUSY);
        C(duet_p1_hw_release(&h,true)==0);C(allocations==frees);
    }
}
static void live_failures(void)
{
    for(unsigned fault=0;fault<6;fault++){
        struct duet_p1_hw h;scenarios++;prepare(&h);initial(&h);
        if(fault==0)cancel_send=7;else if(fault==1)fail_write=3;else {static const unsigned off[]={0,31,1199999,2399999};corrupt_shadow=1;corrupt_offset=off[fault-2];}
        C(duet_p1_hw_refill_remaining(&h,&scp,sync_publish,&h)<0);
        C(!h.stream.pending&&!h.queue.sender.serial&&!h.stream.read.serial&&!h.copy_pending);
        if(fault==1){C(h.observed.cq_writes==write_calls);C(h.observed.cq_arms_accepted+1==write_calls);}
        duet_p1_hw_stopping(&h);C(duet_p1_hw_inputs_stopped(&h,true)==0);
        C(duet_p1_hw_release(&h,false)==-EBUSY);C(duet_p1_hw_release(&h,true)==0);C(allocations==frees);
    }
}
int main(void)
{_Static_assert(sizeof(struct duet_p1_frame_log)==104,"numeric log schema");normal();failures();live_failures();printf("{\"status\":\"PASS\",\"scenarios\":%u,\"checks\":%u}\n",scenarios,checks);}
