/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "duet_p1_bqueue.h"

static bool map_equal(const struct dps_mapping *a, const struct dps_mapping *b)
{
    return dps_key_equal(&a->key, &b->key) && a->span == b->span && a->cq == b->cq &&
           a->span_generation == b->span_generation && a->cq_generation == b->cq_generation;
}
static int index_of(const struct bq_queue *q, const struct dps_key *key)
{
    u32 i;
    if (!q || !q->initialized || !key) return -1;
    for (i = 0; i < BQ_JOBS; i++)
        if (q->jobs[i].state != BQ_FREE && dps_key_equal(&q->jobs[i].map.key, key)) return (int)i;
    return -1;
}
const struct bq_job *bq_lookup(const struct bq_queue *q, const struct dps_key *key)
{
    int i = index_of(q, key);
    return i < 0 ? NULL : &q->jobs[i];
}
static struct bq_job *job_for(struct bq_queue *q, const struct dps_key *key)
{
    int i = index_of(q, key);
    return i < 0 ? NULL : &q->jobs[i];
}
static bool live(const struct bq_queue *q)
{
    return q && q->initialized && q->published && !q->stopping && !q->stopped && !q->error;
}
int bq_hold(struct bq_queue *q, int error)
{
    if (!q || !q->initialized) return -EINVAL;
    if (!q->error) q->error = error < 0 ? error : -EINVAL;
    q->stopping = true;
    return q->error;
}
void bq_stop(struct bq_queue *q)
{
    if (q && q->initialized && !q->stopped) q->stopping = true;
}
static bool slot_owns(const struct bq_queue *q, const struct dps_mapping *m)
{
    return m->cq < BQ_SLOTS && q->slots[m->cq].state != BQ_CQ_FREE &&
           dps_key_equal(&q->slots[m->cq].owner, &m->key) &&
           q->slots[m->cq].generation == m->cq_generation;
}
static bool accepted(const struct bq_job *j)
{
    return j && j->send_returned && !j->send_errno && j->ack && !j->fw_unknown && !j->cancelled;
}
static bool guard_ok(const struct bq_queue *q, const struct bq_guard *g, u64 phase)
{
    const struct dps_observation *o;
    const struct bq_job *current_frame, *future;
    struct dps_mapping fm;
    if (!live(q) || !g || q->arm.serial || phase < 2 || phase > DPS_MAX_FW_SEQUENCE) return false;
    o = &g->observation;
    if (!o->guard_valid || !o->time_ns || o->epoch != q->epoch || o->phase_ordinal != phase ||
        o->done_ordinal != phase - 1 || o->sequence_before != phase || o->sequence_after != phase ||
        q->last_sof != phase || q->last_done != phase - 1 ||
        o->current_frame.key.logical != phase - 1 || !dps_key_equal(&q->active, &o->current_frame.key)) return false;
    current_frame = bq_lookup(q, &q->active);
    if (!accepted(current_frame) || current_frame->state != BQ_ACTIVE || !map_equal(&current_frame->map, &o->current_frame) ||
        o->time_ns < current_frame->sof_ns || o->time_ns < q->last_event_ns || !slot_owns(q, &current_frame->map) ||
        q->slots[current_frame->map.cq].state != BQ_CQ_APPLIED) return false;
    if (dps_mapping(q->epoch, phase, &fm)) return false;
    future = bq_lookup(q, &fm.key);
    return accepted(future) && future->state == BQ_ARMED && slot_owns(q, &fm) &&
           q->slots[fm.cq].state == BQ_CQ_ARMED &&
           g->cq_iova == (u64)q->composer_iova + BQ_CQ_STRIDE * fm.cq;
}
static bool retirement(const struct bq_queue *q, const struct bq_job *j, const struct bq_guard *g)
{
    u64 phase;
    if (!j || !accepted(j) || j->state != BQ_DONE || !j->done_ns || !g) return false;
    phase = g->observation.phase_ordinal;
    if (!guard_ok(q, g, phase) || g->observation.time_ns < j->done_ns ||
        dps_key_equal(&q->active, &j->map.key)) return false;
    if (j->retired)
        return phase >= j->retired_phase && g->observation.time_ns >= j->retired_ns &&
               !slot_owns(q, &j->map) && j->retired_by.epoch == q->epoch &&
               j->retired_by.logical == j->map.key.logical + BQ_SLOTS;
    return phase == (u64)j->map.key.fw + 1 && slot_owns(q, &j->map) &&
           q->slots[j->map.cq].state == BQ_CQ_APPLIED && q->next_logical == j->map.key.logical + BQ_SLOTS;
}
static bool send_token_ok(const struct bq_queue *q, const struct bq_send_token *t)
{
    return t && t->serial && t->serial == q->sender.serial && map_equal(&t->map, &q->sender.map);
}
int bq_init(struct bq_queue *q, u64 epoch, u64 limit, u64 base, u64 bytes)
{
    const unsigned char *p = (const unsigned char *)q;
    size_t i;
    if (!q || !epoch || (limit && (limit < 6 || limit > DPS_MAX_FW_SEQUENCE)) ||
        !base || (base & 0xfff) || bytes != BQ_COMPOSER_BYTES || base > 0xffffffffULL - (bytes - 1)) return -EINVAL;
    for (i = 0; i < sizeof(*q); i++) if (p[i]) return -EBUSY;
    q->initialized = true; q->epoch = epoch; q->limit = limit; q->composer_iova = (u32)base;
    return 0;
}
int bq_publish(struct bq_queue *q)
{
    if (!q || !q->initialized) return -EINVAL;
    if (q->published || q->stopping || q->stopped || q->error) return bq_hold(q, -EBUSY);
    q->published = true; return 0;
}
int bq_reserve(struct bq_queue *q, const struct dps_mapping *m, const struct bq_guard *g)
{
    struct dps_mapping expected, oldmap;
    struct bq_job *old = NULL, *free_job = NULL;
    struct bq_slot *slot;
    u32 i;
    if (!live(q) || !m || q->sender.serial || q->arm.serial || q->tail) return bq_hold(q, -EBUSY);
    if (q->next_logical >= DPS_MAX_FW_SEQUENCE || (q->limit && q->next_logical >= q->limit)) return bq_hold(q, -EOVERFLOW);
    if (q->next_submit != q->next_logical || dps_mapping(q->epoch, q->next_logical, &expected) || !map_equal(m, &expected))
        return bq_hold(q, -EPROTO);
    for (i = 0; i < BQ_JOBS; i++) if (q->jobs[i].state == BQ_FREE) { free_job = &q->jobs[i]; break; }
    if (!free_job) return bq_hold(q, -ENOSPC);
    slot = &q->slots[m->cq];
    if (m->key.logical < 3) {
        if (g || q->last_sof || slot->state != BQ_CQ_FREE) return bq_hold(q, -EPROTO);
    } else {
        if (dps_mapping(q->epoch, m->key.logical - 3, &oldmap)) return bq_hold(q, -EOVERFLOW);
        old = job_for(q, &oldmap.key);
        if (!retirement(q, old, g) || old->retired || !slot_owns(q, &oldmap)) return bq_hold(q, -EPROTO);
    }
    if (old) {
        old->retired = true; old->retired_by = m->key;
        old->retired_phase = g->observation.phase_ordinal; old->retired_ns = g->observation.time_ns;
    }
    memset(free_job, 0, sizeof(*free_job)); free_job->map = *m; free_job->state = BQ_RESERVED;
    slot->owner = m->key; slot->generation = m->cq_generation; slot->state = BQ_CQ_RESERVED;
    q->next_logical++;
    return 0;
}
int bq_send_commit(struct bq_queue *q, const struct dps_key *key, struct bq_send_token *out)
{
    struct bq_job *j;
    u64 serial;
    if (!live(q) || !out || q->sender.serial || q->arm.serial || q->tail) return bq_hold(q, -EBUSY);
    j = job_for(q, key);
    if (!j || j->state != BQ_RESERVED || key->logical != q->next_submit || !slot_owns(q, &j->map) ||
        q->slots[j->map.cq].state != BQ_CQ_RESERVED || q->completions != q->next_submit) return bq_hold(q, -EPROTO);
    if (dps_checked_next(q->serial, &serial) || q->send_commits == ~(u64)0) return bq_hold(q, -EOVERFLOW);
    q->serial = serial; q->send_commits++; q->next_submit++;
    q->sender = (struct bq_send_token){ .serial = serial, .map = j->map };
    j->state = BQ_SENT; j->fw_unknown = true;
    q->slots[j->map.cq].state = BQ_CQ_WRITING;
    *out = q->sender; return 0;
}
int bq_send_result(struct bq_queue *q, const struct bq_send_token *t, int result)
{
    struct bq_job *j;
    if (!q || !q->initialized || !send_token_ok(q, t)) return bq_hold(q, -ESTALE);
    j = job_for(q, &t->map.key);
    if (!j || j->send_returned) return bq_hold(q, -EPROTO);
    j->send_returned = true; j->send_errno = result > 0 ? -EIO : result;
    if (q->stopping || q->error) j->cancelled = true;
    if (result) return bq_hold(q, j->send_errno);
    if (j->ack) j->fw_unknown = false;
    return q->stopping ? -ECANCELED : 0;
}
int bq_ack(struct bq_queue *q, u32 channel, const void *data, size_t len, const struct bq_guard *g)
{
    const unsigned char *b = data;
    struct bq_job *j;
    u32 seq;
    bool cancelled;
    if (!q || !q->initialized || !b || len < 6 || len > 129 || (channel != 10 && channel != 11) || b[0] != 4 || b[1] != 5)
        return bq_hold(q, -EPROTO);
    seq = (u32)b[2] | (u32)b[3] << 8 | (u32)b[4] << 16 | (u32)b[5] << 24;
    j = job_for(q, &q->sender.map.key);
    if (!q->sender.serial || !j || seq != j->map.key.fw || j->ack || j->state != BQ_SENT ||
        !slot_owns(q, &j->map) || q->slots[j->map.cq].state != BQ_CQ_WRITING) return bq_hold(q, -EPROTO);
    cancelled = q->stopping || q->error;
    if (!cancelled && j->map.key.logical >= 3 && !guard_ok(q, g, j->map.key.logical - 1)) return bq_hold(q, -ETIME);
    if (!cancelled && j->map.key.logical < 3 && (g || q->last_sof)) return bq_hold(q, -EPROTO);
    j->ack = true; j->ack_channel = channel;
    if (j->send_returned && !j->send_errno) j->fw_unknown = false;
    if (q->acks == ~(u64)0) return bq_hold(q, -EOVERFLOW);
    q->acks++;
    if (cancelled) { j->cancelled = true; return -ECANCELED; }
    j->state = BQ_COMPOSED; q->slots[j->map.cq].state = BQ_CQ_READY;
    return 0;
}
int bq_send_end(struct bq_queue *q, const struct bq_send_token *t, bool drained)
{
    struct bq_job *j;
    if (!q || !q->initialized || !send_token_ok(q, t)) return bq_hold(q, -ESTALE);
    j = job_for(q, &t->map.key);
    if (!drained || !j || !j->send_returned) return -EBUSY;
    if (!q->stopping && !q->error && !accepted(j)) return -EBUSY;
    if (!q->stopping && !q->error) {
        if (q->completions == ~(u64)0) { bq_hold(q, -EOVERFLOW); }
        else q->completions++;
    }
    memset(&q->sender, 0, sizeof(q->sender));
    return q->error ? q->error : q->stopping ? -ECANCELED : 0;
}
int bq_make_proof(const struct bq_queue *q, const struct dps_key *retired,
                  const struct bq_send_token *completion, const struct bq_guard *g,
                  struct dps_queue_proof *out)
{
    struct dps_queue_proof proof = { 0 };
    const struct bq_job *j;
    if (!q || !q->initialized || !out) return -EINVAL;
    proof.epoch = q->epoch; proof.next_logical = q->next_logical;
    proof.error = q->error; proof.stopping = q->stopping || q->stopped || !q->published;
    j = bq_lookup(q, &q->sender.map.key);
    proof.no_pending = !q->arm.serial && (!q->sender.serial || accepted(j));
    if (completion) {
        if (!send_token_ok(q, completion)) return -ESTALE;
        proof.completion_key = completion->map.key;
        proof.completion_accepted = accepted(j) && !proof.stopping && !proof.error &&
            (completion->map.key.logical < 3 ? !g && !q->last_sof :
             guard_ok(q, g, completion->map.key.logical - 1));
    }
    if (retired) {
        j = bq_lookup(q, retired);
        if (!j) return -ENOENT;
        proof.retired_key = j->map.key; proof.retired_cq_slot = j->map.cq;
        proof.retired_cq_generation = j->map.cq_generation;
        proof.retired_accepted = accepted(j); proof.retired_done = j->state == BQ_DONE;
        proof.retired_unreferenced = retirement(q, j, g);
    }
    *out = proof; return 0;
}
int bq_irq(struct bq_queue *q, u32 status, u32 seq, u64 ns, struct bq_irq_action *out)
{
    struct bq_irq_action a = { 0 };
    struct bq_job *done = NULL, *current_frame = NULL, *next = NULL;
    struct dps_mapping m;
    u64 serial = 0;
    bool terminal = false, cancelled;
    if (!q || !q->initialized || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!q->published || q->stopped || q->arm.serial) return bq_hold(q, -EPROTO);
    if (status & BQ_IRQ_ERROR) return bq_hold(q, -EIO);
    cancelled = q->stopping || q->error;
    if (!(status & (BQ_IRQ_SOF | BQ_IRQ_DONE))) return q->error;
    if (!ns || ns < q->last_event_ns) return bq_hold(q, -EPROTO);
    if (status & BQ_IRQ_DONE) {
        done = job_for(q, &q->active);
        if (!done || done->state != BQ_ACTIVE || ns < done->sof_ns || q->dones == ~(u64)0 ||
            done->map.key.fw != q->last_done + 1) return bq_hold(q, -EPROTO);
    }
    if (status & BQ_IRQ_SOF) {
        if (!seq || seq > DPS_MAX_FW_SEQUENCE || seq != q->last_sof + 1 ||
            (q->active.epoch && !done) || dps_mapping(q->epoch, (u64)seq - 1, &m)) return bq_hold(q, -EPROTO);
        current_frame = job_for(q, &m.key);
        if (!accepted(current_frame) || !slot_owns(q, &m) ||
            (seq == 1 ? current_frame->state != BQ_COMPOSED || q->slots[m.cq].state != BQ_CQ_READY :
                        current_frame->state != BQ_ARMED || q->slots[m.cq].state != BQ_CQ_ARMED) ||
            q->sofs == ~(u64)0) return bq_hold(q, -EPROTO);
        terminal = q->limit && seq == q->limit && q->tail && q->next_logical == q->limit &&
                   q->completions == q->limit && !q->sender.serial;
        if (!cancelled && !terminal) {
            if (q->limit && seq >= q->limit) return bq_hold(q, -ETIME);
            if (dps_mapping(q->epoch, seq, &m)) return bq_hold(q, -EOVERFLOW);
            next = job_for(q, &m.key);
            if (!accepted(next) || next->state != BQ_COMPOSED || !slot_owns(q, &m) ||
                q->slots[m.cq].state != BQ_CQ_READY) return bq_hold(q, -ETIME);
            if (dps_checked_next(q->serial, &serial) || q->cq_arms_completed == ~(u64)0) return bq_hold(q, -EOVERFLOW);
        }
    }
    /* Validation above is read-only: invalid combined IRQ commits neither half.
     * Only a handful of job/slot/counter fields change; no whole-queue clone. */
    if (done) {
        done->state = BQ_DONE; done->done_ns = ns; done->cancelled |= cancelled;
        q->dones++; q->last_done = done->map.key.fw; memset(&q->active, 0, sizeof(q->active));
        a.done = true; a.done_key = done->map.key;
    }
    if (current_frame) {
        current_frame->state = BQ_ACTIVE; current_frame->sof_ns = ns; current_frame->cancelled |= cancelled;
        q->slots[current_frame->map.cq].state = BQ_CQ_APPLIED;
        q->active = current_frame->map.key; q->sofs++; q->last_sof = seq;
        a.sof = true; a.sof_key = current_frame->map.key; a.terminal = terminal;
    }
    if (next) {
        q->serial = serial;
        q->arm = (struct bq_arm_token){ .serial = serial, .map = next->map,
            .iova = q->composer_iova + BQ_CQ_STRIDE * next->map.cq };
        next->state = BQ_ARMING; q->slots[next->map.cq].state = BQ_CQ_ARMING;
        a.arm = true; a.token = q->arm;
    }
    q->last_event_ns = ns; a.cancelled = cancelled; *out = a;
    return q->error ? q->error : 0;
}
int bq_arm_finish(struct bq_queue *q, const struct bq_arm_token *t, int result)
{
    struct bq_job *j;
    if (!q || !q->initialized || !t || !t->serial || t->serial != q->arm.serial ||
        t->iova != q->arm.iova || !map_equal(&t->map, &q->arm.map)) return bq_hold(q, -ESTALE);
    j = job_for(q, &t->map.key);
    if (!j || j->state != BQ_ARMING || !slot_owns(q, &j->map) ||
        q->slots[j->map.cq].state != BQ_CQ_ARMING) return bq_hold(q, -EPROTO);
    memset(&q->arm, 0, sizeof(q->arm));
    if (result || q->stopping || q->error) { j->cancelled = true; return bq_hold(q, result < 0 ? result : -ECANCELED); }
    if (q->cq_arms_completed == ~(u64)0) return bq_hold(q, -EOVERFLOW);
    j->state = BQ_ARMED; q->slots[j->map.cq].state = BQ_CQ_ARMED; q->cq_arms_completed++;
    return 0;
}
int bq_enter_tail(struct bq_queue *q, const struct bq_guard *g)
{
    if (!live(q) || !q->limit || q->tail || q->sender.serial || q->arm.serial ||
        q->next_logical != q->limit || q->next_submit != q->limit || q->completions != q->limit ||
        q->send_commits != q->limit || q->acks != q->limit || !guard_ok(q, g, q->limit - 2)) return bq_hold(q, -EPROTO);
    q->tail = true; return 0;
}
bool bq_terminal(const struct bq_queue *q)
{
    return live(q) && q->tail && q->limit && q->last_sof == q->limit && q->last_done == q->limit &&
           q->sofs == q->limit && q->dones == q->limit && !q->active.epoch && !q->sender.serial &&
           !q->arm.serial && q->cq_arms_completed == q->limit - 1 && q->acks == q->limit && q->completions == q->limit;
}
bool bq_refs_zero(const struct bq_queue *q, const struct dps_key *key)
{
    const struct bq_job *j = bq_lookup(q, key);
    if (!live(q) || !j || j->state != BQ_DONE || !accepted(j) || !j->retired || slot_owns(q, &j->map)) return false;
    return !dps_key_equal(&q->active, key) &&
           !(q->sender.serial && dps_key_equal(&q->sender.map.key, key)) &&
           !(q->arm.serial && dps_key_equal(&q->arm.map.key, key));
}
int bq_forget(struct bq_queue *q, const struct dps_key *key, bool journal)
{
    struct bq_job *j = job_for(q, key);
    if (!j) return -ENOENT;
    if (!journal || !bq_refs_zero(q, key)) return -EBUSY;
    memset(j, 0, sizeof(*j)); return 0;
}
int bq_finish_stop(struct bq_queue *q, bool gate, bool drained)
{
    if (!q || !q->initialized || !q->stopping) return -EINVAL;
    if (!drained || q->sender.serial || q->arm.serial) return -EBUSY;
    if (!gate) return bq_hold(q, -EIO);
    memset(q->jobs, 0, sizeof(q->jobs)); memset(q->slots, 0, sizeof(q->slots));
    memset(&q->active, 0, sizeof(q->active)); q->published = false; q->stopped = true;
    return 0;
}
