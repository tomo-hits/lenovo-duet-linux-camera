/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* Modified on 2026-10-05: idle publisher waits for capture or explicit STOP. */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/jiffies.h>
#include "mt8183_p1_handoff.h"
void dph_init(struct dph_handoff *h)
{
    spin_lock_init(&h->lock);
    init_completion(&h->ready);
    init_completion(&h->done);
}
void dph_close(struct dph_handoff *h)
{
    unsigned long flags;
    spin_lock_irqsave(&h->lock,flags);
    h->closed=true;
    spin_unlock_irqrestore(&h->lock,flags);
    complete_all(&h->ready);
    complete_all(&h->done);
}
int dph_send(struct dph_handoff *h,u64 logical)
{
    unsigned long flags;int ret;
    spin_lock_irqsave(&h->lock,flags);
    if(h->closed||h->pending||h->active){
        ret=h->closed?-ECANCELED:-EBUSY;
        spin_unlock_irqrestore(&h->lock,flags);return ret;
    }
    reinit_completion(&h->done);
    h->logical=logical;h->pending=true;h->result=-EINPROGRESS;h->submitted++;
    spin_unlock_irqrestore(&h->lock,flags);
    complete(&h->ready);
    ret=wait_for_completion_timeout(&h->done,msecs_to_jiffies(1500))?0:-ETIMEDOUT;
    spin_lock_irqsave(&h->lock,flags);
    if(!ret)ret=h->closed?-ECANCELED:h->result;
    if(!ret&&(h->pending||h->active))ret=-EPROTO;
    spin_unlock_irqrestore(&h->lock,flags);
    /* Even timeout never grants permission to reuse or free the shadow. */
    if(ret)dph_close(h);
    return ret;
}
int dph_run(struct dph_handoff *h,int (*publish)(void *,u64),void *context)
{
    unsigned long flags;u64 logical;int ret;
    for(;;){
        /* Idle publication owns no pending copy. The capture worker bounds
         * SOF/firmware progress and closes this handoff on failure or STOP.
         * Startup needs SOF5; its latency follows the sensor frame period.
         * Keep the sender's active-copy deadline separate from this wait. */
        wait_for_completion(&h->ready);
        spin_lock_irqsave(&h->lock,flags);
        if(h->closed){h->pending=false;spin_unlock_irqrestore(&h->lock,flags);return 0;}
        if(!h->pending||h->active){spin_unlock_irqrestore(&h->lock,flags);dph_close(h);return -EPROTO;}
        logical=h->logical;h->active=true;
        spin_unlock_irqrestore(&h->lock,flags);
        ret=publish(context,logical);
        spin_lock_irqsave(&h->lock,flags);
        h->active=false;h->pending=false;h->result=ret;h->completed++;
        spin_unlock_irqrestore(&h->lock,flags);
        complete(&h->done);
        if(ret){dph_close(h);return ret;}
        /* Last publication is not terminal capture: wait for capture STOP. */
    }
}
