/* SPDX-License-Identifier: GPL-2.0-only */
#include "hw-shim.h"
#include "duet_p1_hw.h"
static unsigned checks,allocations,frees,allocation_calls,fail_allocation,send_calls,cancel_send,write_calls,fail_write,corrupt_shadow,corrupt_offset;
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
{reset(h);C(duet_p1_hw_prepare_profile(h,&cam,regs,0xfd000000,1,1600,1200)==0);h->profile.bayer_id=3;}
static void initial(struct duet_p1_hw *h)
{C(duet_p1_hw_publish(h)==0);for(unsigned i=0;i<3;i++)C(duet_p1_hw_frame_submit(h,&scp)==0);C(duet_p1_hw_inputs_begin(h)==0);C(duet_p1_hw_start_commit(h)==0);}

static u32 le32(const u8 *b){return (u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24);}
int main(void){
 struct duet_p1_hw h;struct duet_p1_stream_layout layout;u8 config[129],frame[60];
 struct duet_p1_stream_span spans[7]={0};
 prepare(&h);
 C(h.frame_bytes==2400000 && h.frame_stride==2400256);
 C(h.allocation_bytes==14401536 && h.observed.copy_allocation_bytes==19200000);
 C(!duet_p1_stream_raw10_layout(1600,1200,&layout));C(layout.stride==2000 && layout.image_bytes==2400000);
 C(!duet_p1_stream_encode_config(config,sizeof config,&h.profile));C(le32(config+20)==1600 && le32(config+24)==1200);
 for(unsigned i=0;i<6;i++){
  struct dps_mapping map;u32 iova;
  C(!dps_mapping(1,i,&map));C(!dpa_iova(&h.adapter,&map,&iova));
  C(iova==h.output_iova+i*2400256U);C((u64)iova+2400000<=h.output_iova+h.allocation_bytes);
  spans[1]=(struct duet_p1_stream_span){.iova=iova,.bytes=2400256};
  C(!duet_p1_stream_encode_frame(frame,sizeof frame,&h.profile,i+1,spans));
  spans[1].bytes=2399999;memset(frame,0xaa,sizeof frame);
  C(duet_p1_stream_encode_frame(frame,sizeof frame,&h.profile,i+1,spans)<0);
  for(unsigned j=0;j<sizeof frame;j++)C(frame[j]==0xaa);
 }
 initial(&h);C(duet_p1_hw_release(&h,false)==-EBUSY);
 duet_p1_hw_stopping(&h);C(!duet_p1_hw_inputs_stopped(&h,true));C(!duet_p1_hw_return_cpu(&h));
 C(!duet_p1_hw_ring_finalize(&h,true));C(!duet_p1_hw_release(&h,true));C(allocations==frees);
 for(unsigned fault=1;fault<=4;fault++){
  reset(&h);fail_allocation=fault;C(duet_p1_hw_prepare_profile(&h,&cam,regs,0xfd000000,1,1600,1200)==-ENOMEM);C(allocations==frees);
 }
 printf("{\"status\":\"PASS\",\"checks\":%u,\"scope\":\"front-geometry-six-spans-wire-bounds-rollback-gated-release\"}\n",checks);
}
