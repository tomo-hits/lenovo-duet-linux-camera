/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HW_SHIM_H
#define HW_SHIM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef int8_t s8;typedef int16_t s16;typedef int32_t s32;typedef int64_t s64;
typedef u64 dma_addr_t;typedef u64 phys_addr_t;typedef long long loff_t;
#define __packed __attribute__((packed))
#define __iomem
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,y) ((x)=(y))
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(x) (((x)+4095UL)&~4095UL)
#define IS_ALIGNED(x,a) (!((x)&((a)-1)))
#define DMA_BIT_MASK(x) ((1ULL<<(x))-1)
#define GFP_KERNEL 0
#define IOMMU_DOMAIN_DMA 3
struct completion { bool done; };
typedef unsigned int spinlock_t;
struct device { u64 coherent_dma_mask,bus_dma_limit;struct {int usage_count;} power; };
struct mtk_scp { int unused; };
struct iommu_domain { int type;struct {u64 aperture_start,aperture_end;bool force_aperture;}geometry; };
static inline void spin_lock_init(spinlock_t *l){*l=0;}
#define spin_lock_irqsave(l,f) do {(f)=0;if(*(l))abort();*(l)=1;}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(f);if(!*(l))abort();*(l)=0;}while(0)
static inline void init_completion(struct completion *c){c->done=false;}
static inline void reinit_completion(struct completion *c){c->done=false;}
static inline void complete(struct completion *c){c->done=true;}
static inline unsigned long msecs_to_jiffies(unsigned long x){return x;}
static inline int atomic_read(const int *x){return *x;}
static inline bool pm_runtime_active(struct device *d){return d->power.usage_count>0;}
static inline bool use_dma_iommu(struct device *d){(void)d;return true;}
static inline void *get_dma_ops(struct device *d){(void)d;return NULL;}
static inline bool dev_is_dma_coherent(struct device *d){(void)d;return false;}
static inline u64 dma_get_mask(struct device *d){return d->coherent_dma_mask;}
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *d)
{static struct iommu_domain dom={IOMMU_DOMAIN_DMA,{0,0xff7fffff,true}};(void)d;return &dom;}
static inline phys_addr_t iommu_iova_to_phys(struct iommu_domain *d,u64 x){(void)d;return x;}
static inline void dma_rmb(void){}static inline void dma_wmb(void){}
void *dma_alloc_coherent(struct device *,size_t,dma_addr_t *,int);
void dma_free_coherent(struct device *,size_t,void *,dma_addr_t);
void *vzalloc(size_t);void vfree(void *);
u32 readl(const void *);void writel(u32,void *);
u64 ktime_get_ns(void);
unsigned long wait_for_completion_timeout(struct completion *,unsigned long);
int scp_ipi_send(struct mtk_scp *,u32,void *,unsigned int,unsigned int);
#endif
