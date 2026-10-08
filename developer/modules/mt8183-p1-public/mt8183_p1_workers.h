/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_WORKERS_H
#define MT8183_P1_WORKERS_H
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct dpw_session;
/* RAM-only session executor. These callbacks do NOT constitute a hardware
 * stop proof. The owner retains session/context/code/provider references until
 * dpw_drain() returns, including when gate() fails. No callback takes the
 * endpoint/VB2 mutex. No callback may free session/context. DPW_HELD after
 * drain does NOT authorize releasing HW context, DMA or provider references;
 * those remain owner-held until a separate positive recovery proves safety.
 *
 * close(): IRQ-safe, idempotent; closes the actual ownership admission under
 * its HW lock, wakes every IPI/read/publisher wait, and returns without waiting.
 * start(): process context; checks closed admission after each external step,
 * completes/aborts the pure start transaction, and then returns. Zero means
 * publication was committed; there must be no failing start operation after
 * that commit. A concurrent stop after commit is an ordinary stream stop.
 * capture()/publish(): bounded waits, no endpoint mutex; drain their committed
 * tokens even on stop. They may call dpw_request_stop(), never dpw_drain().
 * input_off(): consumes each started input's reference once, while CAM stays
 * powered. gate(): runs only AFTER start handoff + both real worker joins +
 * CPU destination returns; drains IRQ/IPI/SCP and checks positive CAM gate.
 * return_cpu(): returns remaining CPU destinations exactly once; committed
 * selects ERROR versus failed-start QUEUED. It never releases CAM DMA.
 */
struct dpw_ops {
    void (*close)(void *context);
    int (*start)(void *context);
    int (*capture)(void *context);
    int (*publish)(void *context);
    int (*input_off)(void *context);
    int (*return_cpu)(void *context, bool committed);
    int (*gate)(void *context);
};
enum dpw_state { DPW_READY, DPW_STARTING, DPW_RUNNING, DPW_STOPPING,
                 DPW_STOPPED, DPW_HELD };
struct dpw_session {
    spinlock_t lock;
    const struct dpw_ops *ops;
    void *context;
    struct workqueue_struct *coordinator;
    struct work_struct stop_work;
    struct task_struct *capture_task, *publish_task;
    struct completion start_done, workers_go, stop_done;
    enum dpw_state state;
    bool initialized, start_claimed, committed, stop_queued, drained;
    int first_error, start_error, input_error, cpu_error, gate_error;
};

/* Caller-owned, zeroed object, process context. Creates parked workers before
 * any hardware start. No reuse/reset in place; each epoch gets a fresh object.
 * Even init failure drains any created thread before returning. */
int dpw_init(struct dpw_session *, const struct dpw_ops *, void *);
int dpw_start(struct dpw_session *);
/* IRQ-safe, including before/during start and from either data worker. */
void dpw_request_stop(struct dpw_session *, int error);
bool dpw_stopping(struct dpw_session *);
/* Endpoint/process context only, never a callback/IRQ. One endpoint owner
 * serializes start/drain; async request_stop remains legal. Wait is not a
 * timeout substitute for join. Returns first error, or zero for normal stop.
 * HELD state remains authoritative even after this returns. */
int dpw_drain(struct dpw_session *);
#endif
