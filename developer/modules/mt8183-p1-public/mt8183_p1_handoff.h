/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_HANDOFF_H
#define MT8183_P1_HANDOFF_H
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/types.h>
/* One capture producer, one publisher. Caller joins both before freeing.
 * A close wakes the sender but does not cancel an already executing copy;
 * only publisher return + real thread join proves that copy has ended. */
struct dph_handoff {
    spinlock_t lock;
    struct completion ready, done;
    bool closed, pending, active;
    u64 logical;
    int result;
    unsigned int submitted, completed;
};
void dph_init(struct dph_handoff *);
void dph_close(struct dph_handoff *);
int dph_send(struct dph_handoff *,u64);
int dph_run(struct dph_handoff *,int (*publish)(void *,u64),void *);
#endif
