/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "mt8183_p1_adapter.h"

static bool ready(const struct dpa_adapter *a)
{ return a && a->bound && a->stream && a->queue; }
static const struct dps_record *pure_record(const struct dpa_adapter *a, const struct dps_key *key)
{
    u32 i;
    for (i = 0; i < DPS_RECORDS; i++)
        if (a->stream->records[i].used && dps_key_equal(&a->stream->records[i].map.key, key))
            return &a->stream->records[i];
    return NULL;
}
static bool same_map(const struct dps_mapping *a, const struct dps_mapping *b)
{
    return dps_key_equal(&a->key, &b->key) && a->span == b->span && a->cq == b->cq &&
           a->span_generation == b->span_generation && a->cq_generation == b->cq_generation;
}
static bool pure_accepted(const struct dpa_adapter *a, const struct dps_mapping *m)
{
    const struct dps_record *r = pure_record(a, &m->key);
    const struct dps_span *span;
    if (!r || !r->accepted || !same_map(&r->map, m) || m->span >= DPS_SPANS) return false;
    span = &a->stream->spans[m->span];
    return span->owned && dps_key_equal(&span->owner, &m->key) && span->generation == m->span_generation;
}
int dpa_hold(struct dpa_adapter *a, int error)
{
    if (!ready(a)) return -EINVAL;
    bq_hold(a->queue, error); dps_hold(a->stream, error);
    return a->stream->error;
}
void dpa_stop(struct dpa_adapter *a)
{ if (ready(a)) { bq_stop(a->queue); dps_request_stop(a->stream, 0); } }
int dpa_bind(struct dpa_adapter *a, struct dps_stream *s, struct bq_queue *q, const struct dpa_layout *l)
{
    u64 total, end, cq_end;
    if (!a || !s || !q || !l || a->bound || a->stream || a->queue ||
        !s->initialized || s->state != DPS_STARTING || s->start_inflight || s->next_logical ||
        !q->initialized || q->published || q->stopping || q->stopped || q->error || q->next_logical ||
        s->epoch != q->epoch || s->frame_limit != q->limit ||
        !l->image_iova || (l->image_iova & 4095) || l->span_stride < s->frame_bytes ||
        (l->span_stride & 4095) || l->span_stride > ~(u64)0 / DPS_SPANS) return -EINVAL;
    total = l->span_stride * DPS_SPANS;
    if (total != l->allocation_bytes || total > 0x100000000ULL ||
        l->image_iova > 0xffffffffULL - (total - 1)) return -ERANGE;
    end = l->image_iova + total; cq_end = (u64)q->composer_iova + BQ_COMPOSER_BYTES;
    if (l->image_iova < cq_end && (u64)q->composer_iova < end) return -EINVAL;
    a->stream = s; a->queue = q; a->layout = *l; a->inputs_live = false; a->bound = true;
    return 0;
}
int dpa_iova(const struct dpa_adapter *a, const struct dps_mapping *m, u32 *out)
{
    struct dps_mapping exact;
    u64 offset, address;
    if (!ready(a) || !m || !out || dps_mapping(a->queue->epoch, m->key.logical, &exact) ||
        !same_map(m, &exact) || m->key.fw > DPS_MAX_FW_SEQUENCE) return -EINVAL;
    offset = a->layout.span_stride * m->span;
    if (offset > a->layout.allocation_bytes || a->stream->frame_bytes > a->layout.allocation_bytes - offset ||
        a->layout.image_iova > 0xffffffffULL - offset) return -ERANGE;
    address = a->layout.image_iova + offset;
    if (address > 0xffffffffULL - (a->stream->frame_bytes - 1)) return -ERANGE;
    *out = (u32)address; return 0;
}
int dpa_start_begin(struct dpa_adapter *a)
{
    int ret;
    if (!ready(a)) return -EINVAL;
    ret = dps_start_begin(a->stream); if (ret) return dpa_hold(a, ret);
    ret = bq_publish(a->queue); return ret ? dpa_hold(a, ret) : 0;
}
int dpa_inputs_begin(struct dpa_adapter *a)
{
    u32 i;
    if (!ready(a)) return -EINVAL;
    if (a->inputs_live || a->stream->state != DPS_STARTING || !a->stream->start_inflight ||
        a->stream->pending || a->stream->next_logical != 3 || a->queue->completions != 3 ||
        a->queue->sender.serial || a->queue->last_sof || a->queue->stopping || a->queue->error)
        return dpa_hold(a, -EPROTO);
    for (i = 0; i < 3; i++) {
        struct dps_mapping m;
        dps_mapping(a->queue->epoch, i, &m);
        if (!pure_accepted(a, &m)) return dpa_hold(a, -EPROTO);
    }
    a->inputs_live = true; return 0;
}
int dpa_start_commit(struct dpa_adapter *a)
{
    int ret;
    if (!ready(a)) return -EINVAL;
    if (!a->inputs_live) return dpa_hold(a, -EPROTO);
    ret = dps_start_commit(a->stream); return ret ? dpa_hold(a, ret) : 0;
}
void dpa_start_abort(struct dpa_adapter *a, int error)
{ if (ready(a)) { bq_hold(a->queue, error ? error : -ECANCELED); dps_start_abort(a->stream, error); } }
int dpa_inputs_stopped(struct dpa_adapter *a, bool idle)
{
    if (!ready(a)) return -EINVAL;
    if (!idle) return dpa_hold(a, -EIO);
    a->inputs_live = false; return 0;
}
int dpa_observe(const struct dpa_adapter *a, const struct dpa_sample *p, struct bq_guard *out)
{
    struct bq_guard g = { 0 };
    struct dps_mapping current_frame, future;
    const struct bq_job *cj, *fj;
    const struct bq_queue *q;
    u32 iova;
    if (!ready(a) || !p || !out || !a->inputs_live) return -EINVAL;
    q = a->queue;
    if (q->stopping || q->error || q->stopped || q->arm.serial || a->stream->error ||
        p->sequence_before < 2 || p->sequence_before > DPS_MAX_FW_SEQUENCE ||
        p->sequence_before != p->sequence_after || p->sequence_before != q->last_sof ||
        q->last_done != p->sequence_before - 1 || !p->time_ns ||
        p->time_ns < q->last_event_ns || p->time_ns < a->stream->capture_observed_ns) return -ETIME;
    if (dps_mapping(q->epoch, (u64)p->sequence_before - 1, &current_frame) ||
        dps_mapping(q->epoch, p->sequence_before, &future) || dpa_iova(a, &current_frame, &iova)) return -ERANGE;
    cj = bq_lookup(q, &current_frame.key); fj = bq_lookup(q, &future.key);
    if (p->imgo_iova != iova || !pure_accepted(a, &current_frame) || !pure_accepted(a, &future) ||
        !dps_key_equal(&q->active, &current_frame.key) || !cj || cj->state != BQ_ACTIVE || !same_map(&cj->map, &current_frame) ||
        !fj || fj->state != BQ_ARMED || !same_map(&fj->map, &future) ||
        !cj->ack || !cj->send_returned || cj->send_errno || cj->fw_unknown || cj->cancelled ||
        !fj->ack || !fj->send_returned || fj->send_errno || fj->fw_unknown || fj->cancelled ||
        q->slots[current_frame.cq].state != BQ_CQ_APPLIED || !dps_key_equal(&q->slots[current_frame.cq].owner, &current_frame.key) ||
        q->slots[current_frame.cq].generation != current_frame.cq_generation ||
        q->slots[future.cq].state != BQ_CQ_ARMED || !dps_key_equal(&q->slots[future.cq].owner, &future.key) ||
        q->slots[future.cq].generation != future.cq_generation ||
        p->cq_iova != (u64)q->composer_iova + BQ_CQ_STRIDE * future.cq) return -ETIME;
    g.observation = (struct dps_observation){ .epoch = q->epoch, .time_ns = p->time_ns,
        .phase_ordinal = p->sequence_before, .done_ordinal = q->last_done,
        .sequence_before = p->sequence_before, .sequence_after = p->sequence_after,
        .current_frame = current_frame, .guard_valid = true };
    g.cq_iova = p->cq_iova; *out = g; return 0;
}
int dpa_next_map(const struct dpa_adapter *a, struct dps_mapping *m, u32 *iova)
{
    struct dps_mapping next;
    u32 address;
    int ret;
    if (!ready(a) || !m || !iova || a->stream->next_logical != a->queue->next_logical ||
        a->stream->pending || a->queue->sender.serial || a->queue->stopping || a->queue->error ||
        a->queue->tail || a->queue->next_logical >= DPS_MAX_FW_SEQUENCE ||
        (a->queue->limit && a->queue->next_logical >= a->queue->limit)) return -EBUSY;
    ret = dps_mapping(a->queue->epoch, a->queue->next_logical, &next);
    if (!ret) ret = dpa_iova(a, &next, &address);
    if (ret) return ret;
    *m = next; *iova = address; return 0;
}
int dpa_send_prepare(struct dpa_adapter *a, const struct dpa_sample *sample, struct bq_send_token *out, u32 *iova)
{
    struct dps_mapping m, reserved, old;
    struct dps_queue_proof proof;
    struct bq_guard g;
    struct bq_send_token t = { 0 };
    const struct bq_guard *gp = NULL;
    const struct dps_key *retired = NULL;
    u32 address;
    int ret;
    if (!ready(a) || !out || !iova) return -EINVAL;
    ret = dpa_next_map(a, &m, &address); if (ret) return dpa_hold(a, ret);
    if (m.key.logical >= 3) {
        ret = dpa_observe(a, sample, &g); if (ret) return dpa_hold(a, ret);
        gp = &g; dps_mapping(a->queue->epoch, m.key.logical - 3, &old); retired = &old.key;
    } else if (sample || a->inputs_live) return dpa_hold(a, -EPROTO);
    ret = bq_make_proof(a->queue, retired, NULL, gp, &proof);
    if (!ret) ret = dps_reserve(a->stream, &proof, gp ? &gp->observation : NULL, &reserved);
    if (!ret && !same_map(&m, &reserved)) ret = -EPROTO;
    if (!ret) ret = bq_reserve(a->queue, &reserved, gp);
    if (!ret) ret = bq_send_commit(a->queue, &reserved.key, &t);
    if (!ret) ret = dps_mark_sent(a->stream, &reserved.key);
    if (ret) {
        dpa_hold(a, ret);
        if (a->stream->pending && dps_key_equal(&a->stream->pending_key, &m.key))
            dps_send_complete(a->stream, &m.key, NULL, NULL, ret);
        if (t.serial) { bq_send_result(a->queue, &t, -ECANCELED); bq_send_end(a->queue, &t, true); }
        return ret;
    }
    *out = t; *iova = address; return 0;
}
int dpa_send_result(struct dpa_adapter *a, const struct bq_send_token *t, int result)
{
    int ret;
    if (!ready(a)) return -EINVAL;
    ret = bq_send_result(a->queue, t, result); return ret ? dpa_hold(a, ret) : 0;
}
int dpa_ack(struct dpa_adapter *a, u32 channel, const void *bytes, size_t len, const struct dpa_sample *sample)
{
    struct bq_guard g;
    const struct bq_guard *gp = NULL;
    int ret;
    if (!ready(a)) return -EINVAL;
    if (!a->queue->stopping && a->queue->sender.map.key.logical >= 3) {
        ret = dpa_observe(a, sample, &g);
        if (ret) dpa_hold(a, ret); else gp = &g;
    } else if (!a->queue->stopping && (sample || a->inputs_live)) dpa_hold(a, -EPROTO);
    ret = bq_ack(a->queue, channel, bytes, len, gp);
    return ret ? dpa_hold(a, ret) : 0;
}
int dpa_send_complete(struct dpa_adapter *a, const struct bq_send_token *t, const struct dpa_sample *sample,
                       int wait_result, bool drained)
{
    struct bq_guard g;
    struct dps_queue_proof proof;
    const struct bq_guard *gp = NULL;
    const struct bq_job *job;
    int ret, end;
    if (!ready(a) || !t) return -EINVAL;
    if (!t->serial || t->serial != a->queue->sender.serial || !same_map(&t->map, &a->queue->sender.map))
        return dpa_hold(a, -ESTALE);
    job = bq_lookup(a->queue, &t->map.key);
    if (!drained || !job || !job->send_returned) return -EBUSY;
    if (!wait_result && !a->queue->stopping && !a->stream->error && !job->ack) return -EAGAIN;
    ret = wait_result;
    if (!ret && (a->queue->stopping || a->stream->error)) ret = a->stream->error ? a->stream->error : -ECANCELED;
    if (!ret && t->map.key.logical >= 3) { ret = dpa_observe(a, sample, &g); if (!ret) gp = &g; }
    if (!ret && t->map.key.logical < 3 && (sample || a->inputs_live)) ret = -EPROTO;
    if (ret) dpa_hold(a, ret);
    end = bq_make_proof(a->queue, NULL, t, gp, &proof);
    if (end) { dpa_hold(a, end); if (!ret) ret = end; memset(&proof, 0, sizeof(proof)); }
    end = dps_send_complete(a->stream, &t->map.key, &proof, gp ? &gp->observation : NULL, ret);
    if (end) { dpa_hold(a, end); if (!ret) ret = end; }
    end = bq_send_end(a->queue, t, true);
    if (end) { dpa_hold(a, end); if (!ret) ret = end; }
    return ret;
}
int dpa_phase_begin(struct dpa_adapter *a, const struct dpa_sample *sample)
{
    struct bq_guard g; struct dps_queue_proof p; int ret;
    if (!ready(a)) return -EINVAL;
    if (a->queue->tail) return -ENODATA;
    ret = dpa_observe(a, sample, &g);
    if (!ret) ret = bq_make_proof(a->queue, NULL, NULL, &g, &p);
    if (!ret) ret = dps_phase_begin(a->stream, &p, &g.observation);
    return ret ? dpa_hold(a, ret) : 0;
}
int dpa_read_begin(struct dpa_adapter *a, const struct dps_key *key, enum dps_read_kind kind,
                   const struct dpa_sample *sample, struct dps_read_token *out)
{
    struct bq_guard g; struct dps_queue_proof p; int ret;
    if (!ready(a)) return -EINVAL;
    ret = dpa_observe(a, sample, &g);
    if (!ret) ret = bq_make_proof(a->queue, key, NULL, &g, &p);
    if (!ret) ret = dps_read_begin(a->stream, key, kind, &p, &g.observation, out);
    return ret ? dpa_hold(a, ret) : 0;
}
int dpa_read_end(struct dpa_adapter *a, const struct dps_read_token *t, const struct dpa_sample *sample,
                 int result, size_t bytes, bool match)
{
    struct bq_guard g = { 0 }; struct dps_queue_proof p = { 0 }; int ret, end;
    if (!ready(a) || !t) return -EINVAL;
    ret = result;
    if (!ret) ret = dpa_observe(a, sample, &g);
    if (!ret) ret = bq_make_proof(a->queue, &t->frame.key, NULL, &g, &p);
    if (ret) dpa_hold(a, ret);
    end = dps_read_end(a->stream, t, &p, &g.observation, ret, bytes, match);
    return end ? dpa_hold(a, end) : 0;
}
int dpa_publish_begin(struct dpa_adapter *a, const struct dps_key *key, struct dps_publish_token *out)
{
    int ret; if (!ready(a)) return -EINVAL;
    ret = dps_publish_begin(a->stream, key, out); return ret ? dpa_hold(a, ret) : 0;
}
int dpa_publish_end(struct dpa_adapter *a, const struct dps_publish_token *t, int result,
                    size_t bytes, struct dps_return_token *out)
{
    int ret; if (!ready(a)) return -EINVAL;
    ret = dps_publish_finish(a->stream, t, result, bytes, out);
    if (ret || a->stream->error) dpa_hold(a, ret ? ret : a->stream->error);
    return ret; /* A committed ERROR token still must be delivered exactly once. */
}
int dpa_return_end(struct dpa_adapter *a, const struct dps_return_token *t)
{
    int ret; if (!ready(a)) return -EINVAL;
    ret = dps_return_end(a->stream, t); return ret ? dpa_hold(a, ret) : 0;
}
int dpa_reclaim(struct dpa_adapter *a, const struct dps_key *key, bool journal)
{
    int ret; bool refs;
    if (!ready(a)) return -EINVAL;
    refs = bq_refs_zero(a->queue, key);
    ret = dps_reclaim(a->stream, key, refs, journal);
    if (ret) return ret;
    ret = bq_forget(a->queue, key, journal);
    return ret ? dpa_hold(a, ret) : 0;
}
static bool irq_physical(const struct dpa_adapter *a, const struct dpa_sample *p, const struct dps_key *key)
{
    struct dps_mapping m; u32 address;
    return p && p->time_ns && p->time_ns >= a->queue->last_event_ns &&
        p->sequence_before == key->fw && p->sequence_after == key->fw &&
        !dps_mapping(a->queue->epoch, key->logical, &m) && pure_accepted(a, &m) &&
        !dpa_iova(a, &m, &address) && p->imgo_iova == address;
}
int dpa_irq_plan(struct dpa_adapter *a, u32 status, const struct dpa_sample *sample, struct bq_irq_action *out)
{
    struct bq_irq_action action = { 0 }; int ret;
    if (!ready(a) || !sample || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!a->inputs_live && !a->queue->stopping && (status & (BQ_IRQ_SOF | BQ_IRQ_DONE))) return dpa_hold(a, -EPROTO);
    ret = bq_irq(a->queue, status, sample->sequence_before, sample->time_ns, &action);
    if (ret) return dpa_hold(a, ret);
    if (action.sof && !action.cancelled && !irq_physical(a, sample, &action.sof_key)) ret = -ETIME;
    if (action.arm && !pure_accepted(a, &action.token.map)) ret = -ETIME;
    if (ret) {
        dpa_hold(a, ret);
        if (action.arm) bq_arm_finish(a->queue, &action.token, ret);
        return ret;
    }
    *out = action; return 0;
}
int dpa_irq_finish(struct dpa_adapter *a, const struct bq_irq_action *action, const struct dpa_sample *after, int result)
{
    int ret;
    if (!ready(a) || !action || !action->arm) return -EINVAL;
    if (!result && (!irq_physical(a, after, &action->sof_key) || after->cq_iova != action->token.iova)) result = -ETIME;
    ret = bq_arm_finish(a->queue, &action->token, result);
    return ret ? dpa_hold(a, ret) : 0;
}
int dpa_enter_tail(struct dpa_adapter *a, const struct dpa_sample *sample)
{
    struct bq_guard g; int ret;
    if (!ready(a)) return -EINVAL;
    if (a->stream->phase_active || a->stream->read.serial || a->stream->pending ||
        a->stream->next_logical != a->queue->limit || a->stream->frame_limit != a->queue->limit)
        return dpa_hold(a, -EPROTO);
    ret = dpa_observe(a, sample, &g);
    if (!ret) ret = bq_enter_tail(a->queue, &g);
    return ret ? dpa_hold(a, ret) : 0;
}
bool dpa_terminal(const struct dpa_adapter *a)
{
    u64 n;
    if (!ready(a) || !bq_terminal(a->queue) || a->stream->pending || a->stream->read.serial || a->stream->phase_active ||
        a->stream->error || a->stream->next_logical != a->queue->limit) return false;
    for (n = a->queue->limit - DPS_SPANS; n < a->queue->limit; n++) {
        struct dps_mapping m; const struct bq_job *j;
        if (dps_mapping(a->queue->epoch, n, &m) || !pure_accepted(a, &m)) return false;
        j = bq_lookup(a->queue, &m.key);
        if (!j || j->state != BQ_DONE || !same_map(&j->map, &m)) return false;
    }
    return true;
}
int dpa_stop_return_one(struct dpa_adapter *a, struct dps_return_token *out)
{
    if (!ready(a)) return -EINVAL;
    if (a->queue->sender.serial || a->queue->arm.serial) return -EBUSY;
    return dps_stop_return_one(a->stream, out);
}
int dpa_finish_stop(struct dpa_adapter *a, bool gate, bool drained, bool journal)
{
    u32 i; int ret;
    if (!ready(a)) return -EINVAL;
    if (!drained || !journal || a->stream->start_inflight || a->stream->pending || a->stream->read.serial ||
        a->stream->publisher.serial || a->queue->sender.serial || a->queue->arm.serial || !a->queue->stopping ||
        (a->stream->state != DPS_STOPPING && a->stream->state != DPS_ERROR_HELD)) return -EBUSY;
    for (i = 0; i < DPS_RETURNS; i++) if (a->stream->returns[i].used) return -EBUSY;
    for (i = 0; i < DPS_DESTINATIONS; i++)
        if (a->stream->destinations[i].state != DPS_DEST_FREE && a->stream->destinations[i].state != DPS_DEST_TICKET) return -EBUSY;
    for (i = 0; i < DPS_SPANS; i++) if (a->stream->spans[i].read_refs) return -EBUSY;
    for (i = 0; i < DPS_SHADOWS; i++) if (a->stream->shadows[i].capture_refs || a->stream->shadows[i].publisher_refs) return -EBUSY;
    gate = gate && !a->inputs_live;
    ret = dps_stop_finish(a->stream, gate, true);
    if (ret) { bq_finish_stop(a->queue, false, true); return ret; }
    ret = bq_finish_stop(a->queue, true, true);
    return ret ? dpa_hold(a, ret) : 0;
}
