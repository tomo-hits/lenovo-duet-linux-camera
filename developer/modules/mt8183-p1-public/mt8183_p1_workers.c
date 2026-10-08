/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
/* New session executor, Linux 6.18.28-mt81. No camera registers or payload. */
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/sched/task.h>
#include "mt8183_p1_workers.h"

static void error_locked(struct dpw_session *s, int error)
{
    if (error && !s->first_error)
        s->first_error = error < 0 ? error : -EIO;
}

bool dpw_stopping(struct dpw_session *s)
{
    unsigned long flags;
    bool stop;
    spin_lock_irqsave(&s->lock, flags);
    stop = s->stop_queued;
    spin_unlock_irqrestore(&s->lock, flags);
    return stop;
}

void dpw_request_stop(struct dpw_session *s, int error)
{
    unsigned long flags;
    bool queue = false;
    /* Close HW admission BEFORE a coordinator or start handoff can observe
     * the request. This callback takes its own short HW lock, not ours. */
    s->ops->close(s->context);
    spin_lock_irqsave(&s->lock, flags);
    error_locked(s, error);
    if (!s->stop_queued) {
        s->stop_queued = true;
        s->state = DPW_STOPPING;
        queue = true;
    }
    spin_unlock_irqrestore(&s->lock, flags);
    if (queue)
        queue_work(s->coordinator, &s->stop_work);
}

static int capture_thread(void *data)
{
    struct dpw_session *s = data;
    int ret = 0;
    wait_for_completion(&s->workers_go);
    if (!kthread_should_stop() && !dpw_stopping(s))
        ret = s->ops->capture(s->context);
    /* A finite diagnostic can finish normally. The same coordinator handles
     * user cancellation, worker faults, and normal finite completion. */
    dpw_request_stop(s, ret);
    return ret;
}

static int publish_thread(void *data)
{
    struct dpw_session *s = data;
    int ret = 0;
    wait_for_completion(&s->workers_go);
    if (!kthread_should_stop() && !dpw_stopping(s))
        ret = s->ops->publish(s->context);
    /* Publisher's normal exit still ends this session. A real unlimited
     * publisher returns only on cancellation or a terminal condition. */
    dpw_request_stop(s, ret);
    return ret;
}

static void stop_coordinator(struct work_struct *work)
{
    struct dpw_session *s = container_of(work, struct dpw_session, stop_work);
    unsigned long flags;
    wait_for_completion(&s->start_done);
    s->input_error = s->ops->input_off(s->context);
    /* An unsuccessful start also opens the worker gate; they see STOP and
     * leave without touching the payload. Wake before kthread_stop(). */
    complete_all(&s->workers_go);
    if (s->capture_task) {
        kthread_stop(s->capture_task);
        put_task_struct(s->capture_task);
        s->capture_task = NULL;
    }
    if (s->publish_task) {
        kthread_stop(s->publish_task);
        put_task_struct(s->publish_task);
        s->publish_task = NULL;
    }
    s->cpu_error = s->ops->return_cpu(s->context, s->committed);
    s->gate_error = s->ops->gate(s->context);
    spin_lock_irqsave(&s->lock, flags);
    error_locked(s, s->input_error);
    error_locked(s, s->cpu_error);
    error_locked(s, s->gate_error);
    s->state = s->input_error || s->cpu_error || s->gate_error ? DPW_HELD : DPW_STOPPED;
    spin_unlock_irqrestore(&s->lock, flags);
    complete_all(&s->stop_done);
    /* Owner must still flush this work after completion, before freeing. */
}

int dpw_init(struct dpw_session *s, const struct dpw_ops *ops, void *context)
{
    int ret;
    if (!s || s->initialized || !ops || !ops->close || !ops->start ||
        !ops->capture || !ops->publish || !ops->input_off ||
        !ops->return_cpu || !ops->gate)
        return -EINVAL;
    spin_lock_init(&s->lock);
    init_completion(&s->start_done);
    init_completion(&s->workers_go);
    init_completion(&s->stop_done);
    INIT_WORK(&s->stop_work, stop_coordinator);
    s->ops = ops;
    s->context = context;
    s->state = DPW_READY;
    s->coordinator = alloc_ordered_workqueue("mt8183-p1-stop", WQ_MEM_RECLAIM);
    if (!s->coordinator)
        return -ENOMEM;
    s->capture_task = kthread_create(capture_thread, s, "mt8183-p1-cap");
    if (IS_ERR(s->capture_task)) {
        ret = PTR_ERR(s->capture_task);
        s->capture_task = NULL;
        goto fail;
    }
    get_task_struct(s->capture_task);
    s->publish_task = kthread_create(publish_thread, s, "mt8183-p1-pub");
    if (IS_ERR(s->publish_task)) {
        ret = PTR_ERR(s->publish_task);
        s->publish_task = NULL;
        goto fail;
    }
    get_task_struct(s->publish_task);
    s->initialized = true;
    return 0;
fail:
    /* No HW callback was exposed, workers have not been woken. Linux's
     * kthread_stop-before-first-wake returns without entering threadfn. */
    if (s->capture_task) {
        kthread_stop(s->capture_task);
        put_task_struct(s->capture_task);
        s->capture_task = NULL;
    }
    destroy_workqueue(s->coordinator);
    s->coordinator = NULL;
    return ret;
}

int dpw_start(struct dpw_session *s)
{
    unsigned long flags;
    int ret;
    if (!s || !s->initialized)
        return -EINVAL;
    spin_lock_irqsave(&s->lock, flags);
    if (s->start_claimed || s->drained) {
        spin_unlock_irqrestore(&s->lock, flags);
        return -EALREADY;
    }
    s->start_claimed = true;
    ret = s->stop_queued ? -ECANCELED : 0;
    if (!ret)
        s->state = DPW_STARTING;
    spin_unlock_irqrestore(&s->lock, flags);
    if (!ret)
        ret = s->ops->start(s->context);
    spin_lock_irqsave(&s->lock, flags);
    s->start_error = ret;
    s->committed = !ret;
    error_locked(s, ret);
    if (!ret && !s->stop_queued)
        s->state = DPW_RUNNING;
    spin_unlock_irqrestore(&s->lock, flags);
    /* Wake parked threads BEFORE the handoff can let the coordinator join
     * them and clear their pointers. workers_go still blocks payload entry. */
    wake_up_process(s->capture_task);
    wake_up_process(s->publish_task);
    /* Handoff MUST precede waiting for stop. No start callback below. */
    complete_all(&s->start_done);
    if (ret)
        dpw_request_stop(s, ret);
    complete_all(&s->workers_go);
    return ret;
}

int dpw_drain(struct dpw_session *s)
{
    unsigned long flags;
    bool never_started;
    if (!s || !s->initialized)
        return -EINVAL;
    spin_lock_irqsave(&s->lock, flags);
    if (s->drained) {
        int ret = s->first_error;
        spin_unlock_irqrestore(&s->lock, flags);
        return ret;
    }
    never_started = !s->start_claimed;
    if (never_started)
        s->start_claimed = true;
    spin_unlock_irqrestore(&s->lock, flags);
    dpw_request_stop(s, 0);
    if (never_started)
        complete_all(&s->start_done);
    wait_for_completion(&s->stop_done);
    flush_work(&s->stop_work);
    destroy_workqueue(s->coordinator);
    s->coordinator = NULL;
    s->drained = true;
    return s->first_error;
}
